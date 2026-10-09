// license:BSD-3-Clause
//
// The shared half of Apple audio: one AVAudioEngine, one source node, one
// render block, for macOS and iOS alike. See audio_apple.h for why the two
// platforms share this and what stays apart.
//
// The work is the same on both: call fill, convert to what the unit wants,
// count, and hand the device's workgroup to the parallel slave thread. What
// differs between the platforms is the questions in namespace apple, asked from
// below.
//
// AVFAudio directly rather than through AVFoundation's re-export: this file
// needs the engine and the source node. Verified against the SDK headers rather
// than remembered - the render block puts frameCount before the buffer list,
// initWithRealtimeSafeRenderBlock needs iOS 27 (we floor at 17), and
// connect:to:fromBus:toBus:format:error: likewise, so this uses the error-less
// connect the 17.0 target allows.
#import <AVFAudio/AVFAudio.h>
#import <Foundation/Foundation.h>

#include "ui/audio_apple.h"
#include "ui/audio_in.h"
#include "ui/cpu_meter.h"
#include "ui/resampler.h"
#include "ui/wav.h"

#include "compat/cli_text.h"

#include <mach/mach_time.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace ui {

// Mach ticks per second, for the cpu_percent/worst_ms the status line reads.
// Resolved once: the timebase does not change under a process.
static double mach_tps()
{
	static double tps = 0.0;
	if (tps == 0.0) {
		mach_timebase_info_data_t tb{};
		mach_timebase_info(&tb);
		tps = 1e9 * double(tb.denom) / double(tb.numer);
	}
	return tps;
}

static void zero_buffers(AudioBufferList *abl)
{
	if (!abl)
		return;
	for (UInt32 i = 0; i < abl->mNumberBuffers; i++)
		if (abl->mBuffers[i].mData)
			std::memset(abl->mBuffers[i].mData, 0, abl->mBuffers[i].mDataByteSize);
}

struct apple_audio_out::impl {
	fill_fn fill = nullptr;

	apple::device_ref dev;      // what the request resolved to
	apple::device_claim claim;  // and whether taking it is what got it

	AVAudioEngine *engine = nil;
	AVAudioSourceNode *src = nil;

	std::vector<s16> scratch;   // fill target at 44100 Hz; grown, never shrunk
	std::vector<s16> *cap = nullptr;   // set_capture()'s buffer, or null

	std::string dev_name;
	std::string want_name;        // what the request asked for; "" means system default
	bool want_exact = false;      // and whether a menu name had to match wholly

	std::atomic<bool> running{false};
	std::atomic<bool> taken{false};   // macOS: exclusive asked for, and the device is ours
	std::atomic<u32> buffer_frames{0};    // what the last block actually was
	u32 granted_frames = 0;               // what the platform granted, pre-roll
	// The connection's layout, copied off the device's own format at start() so
	// the block below can write the buffer it is handed without asking anything.
	// Plain values, not the AVAudioFormat: this is the audio thread.
	bool conn_planar = true;      // one buffer per channel, not interleaved
	bool conn_float = true;       // float32 rather than int16
	bool conn_known = false;      // false when the device wanted something else
	std::atomic<u64> produced{0}, starved{0};
	std::atomic<u64> busy_ticks{0}, worst_ticks{0};
	cpu_meter meter;   // recent load for the display (issue #80, shared helper)

	// When the worst spike happened, and how many there were. A single 40 ms
	// callback is four times a 512-frame buffer, so the question is not whether
	// it cracks but what stalls it: cold pages on first touch of the ROM data,
	// the card_lock shared with the display-link pump, or the compiler. The
	// index and the clock are two stores on a path that already does six.
	std::atomic<u64> worst_index{0};
	std::atomic<double> worst_at{0.0};
	std::atomic<u64> spikes{0};          // callbacks over 20 ms
	std::atomic<u64> started_ticks{0};   // mach ticks, like the stamps below
	std::atomic<u64> callbacks{0};

	// The watchdog's state: what it last saw produced, and when. See watch_loop().
	std::atomic<u64> last_produced{0};
	std::atomic<u64> last_seen_ticks{0};
	std::atomic<u64> recoveries{0};
	std::atomic<bool> watch_run{false};
	std::atomic<bool> recovering{false};  // a recovery is failing; said once
	std::atomic<bool> warned{false};       // the block guard below, said once
	std::thread watchdog;            // joined in stop()
};

apple_audio_out::apple_audio_out()
	: m(new impl)
{
}

apple_audio_out::~apple_audio_out()
{
	stop();
}

bool apple_audio_out::start(const request &r, fill_fn fill, std::string &err)
{
	if (m->running.load(std::memory_order_acquire))
		stop();
	m->fill = std::move(fill);

	// The session comes first: the category and the rate decide what the engine
	// is handed, and on iOS an unconfigured session refuses to start one at all.
	if (!apple::session_open(r.latency_ms, err))
		return false;

	AVAudioEngine *engine = [[AVAudioEngine alloc] init];
	AVAudioOutputNode *out_node = [engine outputNode];

	// The connection is at AUDIO_RATE, not at the device's rate: the engine inserts
	// a sample-rate converter of its own when a connection's format differs from
	// the hardware's, so fill(n) needs no translation. Nothing here reads the
	// device's rate to compute with; the one read is after the pin, and it
	// labels the device.

	// Which device the request means, then its buffer size. Both before the
	// engine runs: macOS writes the device's buffer frame size here, which is
	// what --latency has always meant on that side, and the unit negotiates
	// against it once it opens.
	m->want_name = r.device;   // the ask, not the answer: see recover()
	m->want_exact = r.exact;
	m->dev = apple::resolve_output(r.device, r.exact);
	if (!m->dev.found) {
		err = r.device.empty() ? CLI_T("No audio output found", "音声の出口が見つからない")
		                       : CLI_T("No audio output with that name: ", "その名前の音声の出口が見つからない: ")
		                             + r.device;
		return false;
	}
	m->granted_frames = apple::request_buffer_frames(m->dev, r.latency_ms);

	apple_audio_out::impl *im = m.get();
	AVAudioSourceNode *src = [[AVAudioSourceNode alloc]
		initWithRenderBlock:^OSStatus(BOOL *isSilence, const AudioTimeStamp *ts,
		                              AVAudioFrameCount n, AudioBufferList *abl) {
			(void)ts;
			im->buffer_frames.store(n, std::memory_order_relaxed);
			if (!abl || abl->mNumberBuffers < 1) {
				if (isSilence)
					*isSilence = YES;
				return noErr;
			}
			if (!im->running.load(std::memory_order_acquire) || !im->fill) {
				zero_buffers(abl);
				if (isSilence)
					*isSilence = YES;
				return noErr;
			}
			const u64 t0 = mach_absolute_time();
			// n frames at 44100, because that is what the node is connected at:
			// one fill() covers the callback exactly, no drift and no stash, and
			// n is therefore the machine's own frame count - which is what
			// produced() and the overrun test below are denominated in.
			if (im->scratch.size() < size_t(n) * 2)
				im->scratch.resize(size_t(n) * 2);
			im->fill(im->scratch.data(), u32(n));
			// Into the layout the device asked for, which is the one the block is
			// called with: the connection is the hardware's own format at
			// AUDIO_RATE, so the engine's converter has a rate to do and nothing
			// else to do. Only the rate is ours; the layout is read off the
			// pinned device in start() and copied into conn_planar/conn_float.
			const s16 *sv = im->scratch.data();
			bool ok = false;
			if (im->conn_planar && abl->mNumberBuffers >= 2) {
				AudioBuffer *lb = &abl->mBuffers[0], *rb = &abl->mBuffers[1];
				if (im->conn_float &&
				    lb->mDataByteSize >= n * sizeof(float) &&
				    rb->mDataByteSize >= n * sizeof(float)) {
					float *l = static_cast<float *>(lb->mData);
					float *r = static_cast<float *>(rb->mData);
					for (u32 i = 0; i < n; i++) {
						l[i] = float(sv[i * 2])     * (1.0f / 32768.0f);
						r[i] = float(sv[i * 2 + 1]) * (1.0f / 32768.0f);
					}
					ok = true;
				} else if (!im->conn_float &&
				           lb->mDataByteSize >= n * sizeof(s16) &&
				           rb->mDataByteSize >= n * sizeof(s16)) {
					s16 *l = static_cast<s16 *>(lb->mData);
					s16 *r = static_cast<s16 *>(rb->mData);
					for (u32 i = 0; i < n; i++) {
						l[i] = sv[i * 2];
						r[i] = sv[i * 2 + 1];
					}
					ok = true;
				}
			} else if (!im->conn_planar && abl->mNumberBuffers >= 1) {
				AudioBuffer *b = &abl->mBuffers[0];
				if (im->conn_float && b->mDataByteSize >= n * 2 * sizeof(float)) {
					float *f = static_cast<float *>(b->mData);
					for (u32 i = 0; i < n * 2; i++)
						f[i] = float(sv[i]) * (1.0f / 32768.0f);
					ok = true;
				} else if (!im->conn_float && b->mDataByteSize >= n * 2 * sizeof(s16)) {
					std::memcpy(b->mData, sv, size_t(n) * 2 * sizeof(s16));
					ok = true;
				}
			}
			if (ok) {
				// nothing to undo
			} else {
				// Not reached unless the graph hands us something other than the
				// format we connected with - which is the device's own, and, per
				// AVAudioEngine's own note on a configuration change, what the
				// nodes keep across one. Said once, because a silent zero buffer
				// here is a freeze that looks like a mute, and that is how the
				// last two defects in this file were found.
				if (!im->warned.exchange(true, std::memory_order_relaxed))
					std::fprintf(stderr,
					             "[audio] block does not match the connected format"
					             " (%u buffers, planar=%d, float=%d); silencing it\n",
					             unsigned(abl->mNumberBuffers), int(im->conn_planar),
					             int(im->conn_float));
				zero_buffers(abl);
			}
			const u64 t1 = mach_absolute_time();
			const u64 busy = t1 - t0;
			im->busy_ticks.fetch_add(busy, std::memory_order_relaxed);
			im->meter.add(double(busy) / mach_tps(), double(n) / double(AUDIO_RATE));
			const u64 index = im->callbacks.fetch_add(1, std::memory_order_relaxed) + 1;
			if (const double ms = 1000.0 * double(busy) / mach_tps(); ms > 20.0)
				im->spikes.fetch_add(1, std::memory_order_relaxed);
			u64 worst = im->worst_ticks.load(std::memory_order_relaxed);
			while (busy > worst &&
			       !im->worst_ticks.compare_exchange_weak(worst, busy,
			                                             std::memory_order_relaxed)) {
			}
			// Which callback was the worst, and when: printed on stop, so a
			// one-off (cold pages, first touch of the ROM data) can be told
			// apart from a stall that recurs.
			if (busy >= worst) {
				im->worst_index.store(index, std::memory_order_relaxed);
				const double stamp = double(mach_absolute_time());
				im->worst_at.store(
				    (stamp - double(im->started_ticks.load(std::memory_order_relaxed))) /
				        mach_tps() * 1000.0,
				    std::memory_order_relaxed);
			}
			// The overrun proxy CoreAudio has no better name for: we took longer
			// to make the block than the block is worth. Against AUDIO_RATE,
			// because the block is 44100 frames whatever the device runs at -
			// that is what the overrun has to be measured against, and the
			// device's rate is not ours to know on this path any more.
			if (double(busy) / mach_tps() > double(n) / double(AUDIO_RATE))
				im->starved.fetch_add(1, std::memory_order_relaxed);
			// What the machine made, before any conversion, which is what a
			// capture means on both platforms. The only allocation on this
			// path, and only while --dump-dev asked for it.
			if (im->cap)
				im->cap->insert(im->cap->end(), im->scratch.data(),
				                im->scratch.data() + size_t(n) * 2);
			// n, and it needs no thought: the node is connected at AUDIO_RATE,
			// so n is already the machine's frame count - which is the unit every
			// consumer divides by AUDIO_RATE to get seconds. This used to be the
			// device's frame count, which on a 48 kHz output read 8.9% long.
			im->produced.fetch_add(n, std::memory_order_relaxed);
			if (isSilence)
				*isSilence = NO;
			return noErr;
		}];
	[engine attachNode:src];
	NSError *e = nil;
	// The device is pinned before the connection, because the format is
	// negotiated against whichever device the unit holds - and because the
	// format below is read off that device.
	if (!apple::pin_output([out_node audioUnit], m->dev, err)) {
		[engine detachNode:src];
		return false;
	}

	// The connection is the hardware's own format at AUDIO_RATE, so the rate is the
	// only difference and the engine has only the rate to convert. Asking for
	// anything else has it convert the layout too.
	//
	// Two channels whatever the device has: the machine is stereo, and a device
	// with more gets the graph's own downmix. A sample format that is neither
	// float32 nor int16 gets float32 and the graph's conversion after all.
	AVAudioFormat *hw_fmt = [out_node outputFormatForBus:0];
	const bool hw_int16 = hw_fmt.commonFormat == AVAudioPCMFormatInt16;
	const bool hw_planar = hw_fmt.channelCount < 1 ? true : !hw_fmt.isInterleaved;
	im->conn_float = !hw_int16;
	im->conn_planar = hw_planar;
	AVAudioFormat *fmt = [[AVAudioFormat alloc] initWithCommonFormat:
	    hw_int16 ? AVAudioPCMFormatInt16 : AVAudioPCMFormatFloat32
	                                                       sampleRate:double(AUDIO_RATE)
	                                                         channels:2
	                                                      interleaved:!hw_planar];
	if (!fmt) {
		err = "cannot describe the device's stereo format at 44100";
		return false;
	}
	[engine connect:src to:out_node fromBus:0 toBus:0 format:fmt];
	if (![engine startAndReturnError:&e]) {
		err = std::string("AVAudioEngine start: ") +
		      (e ? [[e localizedDescription] UTF8String] : "?");
		return false;
	}

	m->engine = engine;
	m->src = src;
	// Read here, not before the pin: from here on the unit holds the device, so
	// this is that device's rate rather than whatever was default when this
	// function was entered. It labels the device and says the graph is
	// converting; nothing computes with it.
	const double rate = [[out_node outputFormatForBus:0] sampleRate];
	m->dev_name = apple::output_label(m->dev, rate);   // the device's, not ours
	if (rate > 0.0 && rate != double(AUDIO_RATE))
		std::fprintf(stderr, "[audio] device runs %.0f Hz, the engine converts\n", rate);
	m->running.store(true, std::memory_order_release);
	// The watchdog starts with the engine and stops with it (see watch_loop).
	// Its first sight has to be the count we are about to have, not zero, or the
	// first tick would call a fresh engine a stall.
	m->last_produced.store(0, std::memory_order_relaxed);
	m->last_seen_ticks.store(uint64_t(mach_absolute_time()), std::memory_order_relaxed);
	m->watch_run.store(true, std::memory_order_release);
	m->watchdog = std::thread([this] { watch_loop(); });
	m->callbacks.store(0, std::memory_order_relaxed);
	m->spikes.store(0, std::memory_order_relaxed);
	m->started_ticks.store(uint64_t(mach_absolute_time()), std::memory_order_relaxed);

	// The device is claimed after IO has started, as it always was: claiming
	// first can leave a device that cannot be mixed unopenable, and a refused
	// claim still plays - it surfaces through exclusive(), not a failed start.
	// Taking it changes the device's mixability, so the HAL rebuilds its IO
	// under us and the engine is started again on the new one (macOS only).
	if (r.exclusive) {
		std::string hog_err;
		m->claim = apple::take_output(m->dev, hog_err);
		// held, not took: a device we already held is ours to use, and exclusive()
		// has to say so rather than report that we got nothing.
		m->taken.store(m->claim.held, std::memory_order_relaxed);
		if (m->claim.took) {
			[engine stop];
			NSError *restart_err = nil;
			if (![engine startAndReturnError:&restart_err]) {
				// We took the device and cannot open it. Playing nothing while
				// holding it would be the worst of the three answers, so the
				// start fails and stop() gives the device back.
				err = std::string("Cannot play on the device taken for exclusive use: ") +
				      (restart_err ? [[restart_err localizedDescription] UTF8String] : "?");
				stop();
				return false;
			}
		}
	}
	std::fprintf(stderr, "[audio] %s\n", m->dev_name.c_str());
	return true;
}

void apple_audio_out::restart()
{
	// The session watcher (iOS's AVAudioSession, and the hook macOS's session file
	// answers) calls this: something changed that we should come back from. The
	// recovery is the same one the watchdog does, because it is the same
	// situation - the engine is not running and nobody has noticed.
	recover("the session says the device changed");
}

void apple_audio_out::watch_loop()
{
	while (m->watch_run.load(std::memory_order_acquire)) {
		std::this_thread::sleep_for(std::chrono::milliseconds(250));
		if (!m->running.load(std::memory_order_acquire) || !m->engine)
			continue;
		const u64 seen = uint64_t(mach_absolute_time());
		const u64 produced = m->produced.load(std::memory_order_acquire);
		if (produced != m->last_produced.load(std::memory_order_relaxed)) {
			m->last_produced.store(produced, std::memory_order_relaxed);
			m->last_seen_ticks.store(seen, std::memory_order_relaxed);
			continue;
		}
		if ((seen - m->last_seen_ticks.load(std::memory_order_relaxed)) / mach_tps() < 1.0)
			continue;
		m->last_seen_ticks.store(seen, std::memory_order_relaxed);
		recover("nothing produced for a second");
	}
}

bool apple_audio_out::recover(const char *why)
{
	AVAudioEngine *engine = m->engine;
	if (!engine)
		return false;
	// Safe from this thread, which is why the watchdog has one: the engine's
	// configuration-change callback runs on an internal dispatch queue, and
	// Apple's header warns against tearing the engine down inside it.
	//
	// Re-resolve and re-pin first: the device that went away may be a different
	// one now, and a pin onto a device that is gone is what we are recovering
	// from. The connection stays at AUDIO_RATE, so a rate change needs no rebuild.
	// From the *request*, not from the device we ended up on, or a request for
	// the system default would pin whatever was default at startup.
	apple::device_ref again = apple::resolve_output(m->want_name, m->want_exact);
	if (again.found) {
		m->dev = again;
		std::string err;
		apple::pin_output([engine outputNode].audioUnit, again, err);
	}
	[engine stop];
	NSError *e = nil;
	if ([engine startAndReturnError:&e]) {
		const u64 n = m->recoveries.fetch_add(1, std::memory_order_relaxed) + 1;
		const double rate = [[engine outputNode] outputFormatForBus:0].sampleRate;
		std::fprintf(stderr, "[audio] recovered (%s), %.0f Hz, %llu so far\n", why, rate,
		             (unsigned long long)n);
		m->last_produced.store(m->produced.load(std::memory_order_acquire),
		                       std::memory_order_relaxed);
		m->last_seen_ticks.store(uint64_t(mach_absolute_time()), std::memory_order_relaxed);
		m->recovering.store(false, std::memory_order_relaxed);
		return true;
	}
	// Said once per streak. A recovery that keeps failing is usually a device that
	// has gone to another application, and that is not going to be fixed by
	// saying it again every second until the program ends.
	if (!m->recovering.exchange(true, std::memory_order_relaxed))
		std::fprintf(stderr, "[audio] could not recover (%s): %s\n", why,
		             e ? [[e localizedDescription] UTF8String] : "?");
	return false;
}

void apple_audio_out::stop()
{
	if (!m->running.exchange(false))
		return;
	// The watchdog first, before the engine goes: it must not decide to recover
	// an engine that is on its way out, and its thread holds this impl.
	m->watch_run.store(false, std::memory_order_release);
	if (m->watchdog.joinable())
		m->watchdog.join();
	// Stop first so no new block enters, then release: a block already inside
	// still holds the raw impl pointer, the way a refCon does. The app keeps
	// this object in a static and never destroys it mid-render, so the window
	// is theoretical - but stop-before-release is what keeps it so.
	AVAudioEngine *engine = m->engine;
	m->src = nil;
	m->engine = nil;
	if (engine)
		[engine stop];
	m->fill = nullptr;
	apple::unpin_output();
	apple::release_output(m->claim);
	m->claim = apple::device_claim();
	m->taken.store(false, std::memory_order_relaxed);
	// What the worst spike was, and when: a 40 ms callback is four times a
	// 512-frame buffer, so this line is where the crack gets explained (or not).
	const u64 calls = m->callbacks.load(std::memory_order_relaxed);
	if (calls) {
		const double worst_ms = 1000.0 * double(m->worst_ticks.load(std::memory_order_relaxed)) /
		                        mach_tps();
		std::fprintf(stderr,
		             "[audio] %llu callbacks, worst %.1f ms at #%llu (t=%.1fs), "
		             "%llu over 20 ms, %llu late\n",
		             (unsigned long long)calls, worst_ms,
		             (unsigned long long)m->worst_index.load(std::memory_order_relaxed),
		             m->worst_at.load(std::memory_order_relaxed) / 1000.0,
		             (unsigned long long)m->spikes.load(std::memory_order_relaxed),
		             (unsigned long long)m->starved.load(std::memory_order_relaxed));
	}
}

bool apple_audio_out::running() const
{
	return m->running.load(std::memory_order_relaxed);
}

const std::string &apple_audio_out::device_name() const
{
	return m->dev_name;
}

bool apple_audio_out::exclusive() const
{
	return m->taken.load(std::memory_order_relaxed);
}

void *apple_audio_out::realtime_workgroup()
{
	if (!m->engine || !m->engine.isRunning)
		return nullptr;
	// The device-owned audio workgroup, for the parallel slave thread to join
	// (Apple's parallel real-time threads pattern; the join itself is in
	// compat/realtime.h). AVAudioIONode hands out the unit the engine renders
	// through, and off that unit this is the same property audio_out_mac.cpp
	// read from a unit of its own - AUAudioUnit.osWorkgroup is bridged to it.
	// The group belongs to the device, not to the unit, so there is one
	// whichever way the output was opened: RemoteIO was never a requirement.
	//
	// +0: the C getter's contract, so unretained. The device owns the group,
	// which is what lets this be stored as a bare handle and outlive a
	// stop/start of the engine.
	__unsafe_unretained os_workgroup_t wg = nullptr;
	UInt32 size = sizeof(wg);
	if (AudioUnitGetProperty(m->engine.outputNode.audioUnit,
	                         kAudioOutputUnitProperty_OSWorkgroup,
	                         kAudioUnitScope_Global, 0, &wg, &size) == noErr)
		return (__bridge void *)wg;
	return nullptr;
}

void apple_audio_out::set_capture(const std::string &path)
{
	// Same convention as every backend: the flag says capture was asked for, so
	// the render block knows to append rather than to skip a null check.
	m_cap_path = path;
	if (path.empty()) {
		m_cap.clear();
		m->cap = nullptr;
		return;
	}
	m_cap.clear();
	m->cap = &m_cap;
}

u64 apple_audio_out::capture_frames() const
{
	return m_cap.size() / 2;
}

bool apple_audio_out::write_capture(std::string &err)
{
	if (m_cap_path.empty()) {
		err = CLI_T("No output file was given", "書き出す先が決まっていない");
		return false;
	}
	// The WAV header is ui/wav.h's, the same one live --wav and render write:
	// five copies of the same 44 bytes was one too many.
	return write_wav(m_cap_path, m_cap, err, AUDIO_RATE);
}

u64 apple_audio_out::produced() const
{
	return m->produced.load(std::memory_order_relaxed);
}

u32 apple_audio_out::buffer_frames() const
{
	// What the last block actually was, once there has been one. Before the
	// first callback the granted size is the honest answer, and on iOS it used
	// to read as a 0-frame buffer because nothing had been rendered yet.
	const u32 seen = m->buffer_frames.load(std::memory_order_relaxed);
	return seen ? seen : m->granted_frames;
}

u64 apple_audio_out::starved() const
{
	return m->starved.load(std::memory_order_relaxed);
}

bool apple_audio_out::mmcss() const
{
	// The engine's render thread is real-time by construction - the counterpart
	// of registering with MMCSS on Windows, with nothing to register.
	return m->running.load(std::memory_order_relaxed);
}

double apple_audio_out::cpu_percent() const
{
	const u64 busy = m->busy_ticks.load(std::memory_order_relaxed);
	const u64 prod = m->produced.load(std::memory_order_relaxed);
	if (prod == 0)
		return 0.0;
	// Fraction of one device-rate second spent rendering, as a percent.
	// prod is a count of 44100 frames and busy is ticks to make one, so the
	// device's rate is not in this any more (see the render block).
	return 100.0 * double(busy) / (mach_tps() * (double(prod) / double(AUDIO_RATE)));
}

double apple_audio_out::worst_ms() const
{
	return 1000.0 * double(m->worst_ticks.load(std::memory_order_relaxed)) / mach_tps();
}

double apple_audio_out::cpu_recent() const
{
	return m->meter.value();
}

// One sample out of a tap buffer, whichever layout the device hands us.
// floatChannelData is the right accessor either way, but an interleaved buffer
// keeps every channel in entry 0 while a non-interleaved one points each entry
// at its own channel. A request past the last channel folds back to the first,
// so a mono device feeds both stereo sides and a 5-channel interface feeds the
// first two - the rule the Windows and Linux backends use.
static inline float tap_sample(const float *const *chans, AVAudioFormat *fmt,
                               AVAudioFrameCount i, AVAudioChannelCount want)
{
	const AVAudioChannelCount ch = fmt.channelCount;
	if (!ch || !chans[0])
		return 0.0f;
	const AVAudioChannelCount c = want < ch ? want : AVAudioChannelCount(0);
	return fmt.isInterleaved ? chans[0][size_t(i) * ch + c] : chans[c][i];
}

// ---- The input half ---------------------------------------------------------
//
// Recording is the same shape as playback with the arrow reversed: the engine
// hands us buffers, we convert them to 44100 Hz s16 stereo and push them into a
// ring, and pop() takes them out one pair at a time. The buffers arrive the same
// way on both platforms - tapped off the engine - so the format asked for is
// the engine's own input format rather than one this file decides.

struct apple_audio_in::impl {
	static constexpr u32 RING = 1 << 16, MASK = RING - 1;      // 約 1.5 秒
	static constexpr u32 TARGET_FRAMES = 2205;                  // 50ms
	static constexpr u32 DROP_FRAMES = 8820;                    // 200ms を超えたら捨てる

	AVAudioEngine *engine = nil;

	ui::resampler rs;
	std::vector<s16> staging;    // tap frames as s16 stereo (the resampler's input)
	std::vector<float> conv;
	std::vector<s16> out16;

	std::vector<s16> m_ring = std::vector<s16>(size_t(RING) * 2);
	std::atomic<u32> m_w{0}, m_r{0};
	std::atomic<u64> m_empty{0}, m_dropped{0};
	std::atomic<bool> running{false};
	// Liveness. The tap block counts, and the watchdog below watches that count
	// rather than the ring: the ring only drains when the machine asks for input,
	// and a synth that never does would look dead.
	std::atomic<u64> taps{0};
	std::atomic<u64> last_taps{0};
	std::atomic<u64> last_seen_ticks{0};
	std::atomic<bool> watch_run{false};
	std::atomic<bool> recovering{false};
	std::thread watchdog;

	std::string dev_name, fmt_line;
	double dev_rate = double(AUDIO_RATE);

	// 44100Hz 16bit 2ch を輪に積む。溢れる分は捨てる (the tap outruns the
	// synth's pop when the machine is busy).
	void push(const s16 *frames, u32 n)
	{
		u32 wr = m_w.load(std::memory_order_relaxed);
		for (u32 i = 0; i < n; i++) {
			const u32 rd = m_r.load(std::memory_order_acquire);
			if (((wr + 1) & MASK) == rd)
				break;
			m_ring[wr * 2] = frames[i * 2];
			m_ring[wr * 2 + 1] = frames[i * 2 + 1];
			wr = (wr + 1) & MASK;
			m_w.store(wr, std::memory_order_release);
		}
	}
};

apple_audio_in::apple_audio_in()
	: m(new impl)
{
}

apple_audio_in::~apple_audio_in()
{
	stop();
}

bool apple_audio_in::start(const std::string &device, std::string &err)
{
	stop();
	// Three questions before any engine exists, in the order they have always
	// been asked: which device did the name mean, may we record, does the
	// session take the request. A bad name is a cheaper thing to say than a
	// permission prompt.
	const apple::device_ref dev = apple::resolve_input(device, false);
	if (!dev.found) {
		err = device.empty() ? CLI_T("No recording device", "録音デバイスが無い")
		                     : CLI_T("No recording device with that name", "その名前の録音デバイスは無い");
		return false;
	}
	if (!apple::input_permission(err))
		return false;
	if (!apple::session_open_input(err))
		return false;

	AVAudioEngine *engine = [[AVAudioEngine alloc] init];
	AVAudioInputNode *node = [engine inputNode];
	// The pin comes before the format, for the same reason the output side's
	// does: what the unit is allowed to record is decided by which device it
	// holds.
	if (!apple::pin_input([node audioUnit], dev, err))
		return false;

	// The device's rate, unlike the output side: no converter goes ahead of the
	// input node, so a connection asking for a rate the hardware is not running
	// is accepted, starts, and delivers nothing. Measured, hardware at 96000,
	// 600 ms each: the input node at 44100 gave 0 buffers, at 96000 gave 6. The
	// channel count is not the constraint - 2ch against a 1ch device folded and
	// gave 6 - so ui::resampler below is here for the rate alone.
	//
	// The tap asks for the device's own format. A forced 2ch interleaved one is
	// accepted and starts, and on a mono device the block is then never called.
	// The block below folds the channels instead.
	AVAudioFormat *tap = [node inputFormatForBus:0];
	const double rate = tap.sampleRate;
	if (!(rate > 0.0) || tap.channelCount < 1) {
		err = CLI_T("Cannot read the recording device's format", "録音の形式が読めない");
		return false;
	}
	m->dev_rate = rate;
	m->rs.configure(rate, double(AUDIO_RATE));

	apple_audio_in::impl *im = m.get();
	[node installTapOnBus:0 bufferSize:1024 format:tap
	                block:^(AVAudioPCMBuffer *buf, AVAudioTime *when) {
		                (void)when;
		                im->taps.fetch_add(1, std::memory_order_relaxed);
		                if (!buf || buf.frameLength == 0)
			                return;
		                const UInt32 n = buf.frameLength;
		                const float *const *f = buf.floatChannelData;
		                if (!f || !f[0])
			                return;
		                im->staging.resize(size_t(n) * 2);
		                AVAudioFormat *fmt = buf.format;
		                for (UInt32 i = 0; i < n; i++) {
				                const float l = tap_sample(f, fmt, i, 0);
				                const float r = tap_sample(f, fmt, i, 1);
				                im->staging[size_t(i) * 2] =
				                    s16(std::lround(std::clamp(l, -1.0f, 1.0f) * 32767.0f));
				                im->staging[size_t(i) * 2 + 1] =
				                    s16(std::lround(std::clamp(r, -1.0f, 1.0f) * 32767.0f));
		                }
		                if (im->rs.direct()) {
			                im->push(im->staging.data(), n);
			                return;
		                }
		                // The resampler takes what it can take and gives back what
		                // that produced, so a 1024-frame tap at 48k becomes 941
		                // frames at 44100 with nothing kept back.
		                for (UInt32 at = 0; at < n;) {
			                const UInt32 k = std::min<UInt32>(1024, n - at);
			                im->rs.push(im->staging.data() + size_t(at) * 2, int(k));
			                at += k;
			                const int got = im->rs.output_available();
			                if (got <= 0)
				                continue;
			                im->conv.resize(size_t(got) * 2);
			                im->out16.resize(size_t(got) * 2);
			                im->rs.pull(im->conv.data(), got);
			                for (size_t j = 0; j < im->out16.size(); j++)
				                im->out16[j] = s16(std::lround(
				                    std::clamp(im->conv[j], -1.0f, 1.0f) * 32767.0f));
			                im->push(im->out16.data(), u32(got));
		                }
	                }];
	NSError *e = nil;
	if (![engine startAndReturnError:&e]) {
		[node removeTapOnBus:0];
		err = std::string("AVAudioEngine input start: ") +
		      (e ? [[e localizedDescription] UTF8String] : "?");
		return false;
	}

	m->engine = engine;
	m->dev_name = dev.name;
	m->fmt_line = apple::input_label(dev, rate, 2);
	m->m_w.store(0);
	m->m_r.store(0);
	m->running.store(true, std::memory_order_release);
	// The watchdog starts with the engine and stops with it, and its first sight
	// has to be a count it can tell from zero, or the first tick would call a
	// fresh engine a stall.
	m->last_taps.store(m->taps.load(std::memory_order_acquire), std::memory_order_relaxed);
	m->last_seen_ticks.store(uint64_t(mach_absolute_time()), std::memory_order_relaxed);
	m->recovering.store(false, std::memory_order_relaxed);
	m->watch_run.store(true, std::memory_order_release);
	m->watchdog = std::thread([this] { watch_loop(); });
	std::fprintf(stderr, "[audio] in: %s (%.0f Hz)\n", m->dev_name.c_str(), rate);
	return true;
}

void apple_audio_in::restart()
{
	AVAudioEngine *engine = m->engine;
	if (!engine || !m->running.load(std::memory_order_acquire))
		return;
	[engine stop];
	// Re-read the node's format before starting. Starting an engine that has stopped
	// itself with the format read before it fails - measured, error -10868 -
	// while asking the node for its format again re-opens the IO unit and the
	// start succeeds. This is the iOS path's first line of defence, where the
	// session watcher calls restart() after a route change.
	AVAudioInputNode *node = [engine inputNode];
	AVAudioFormat *now = [node inputFormatForBus:0];
	if (now.sampleRate > 0.0 && now.sampleRate != m->dev_rate) {
		m->dev_rate = now.sampleRate;
		m->rs.configure(now.sampleRate, double(AUDIO_RATE));
	}
	NSError *e = nil;
	if ([engine startAndReturnError:&e]) {
		m->recovering.store(false, std::memory_order_relaxed);
		m->last_taps.store(m->taps.load(std::memory_order_acquire), std::memory_order_relaxed);
		m->last_seen_ticks.store(uint64_t(mach_absolute_time()), std::memory_order_relaxed);
		return;
	}
	// Said once per streak: a recording device that has gone to another
	// application is not going to be fixed by saying so every second.
	if (!m->recovering.exchange(true, std::memory_order_relaxed))
		std::fprintf(stderr, "[audio] in: restart failed: %s\n",
		             e ? [[e localizedDescription] UTF8String] : "?");
}

// The output half's watchdog, for the same reason and with the same excuse: the
// session watchers are iOS's, so on macOS nothing restarts a recording engine
// the clock has stopped. Its own thread because not every front end has a tick.
// The signal is the tap's own count and not the ring, which only drains when the
// machine asks for input.
void apple_audio_in::watch_loop()
{
	while (m->watch_run.load(std::memory_order_acquire)) {
		std::this_thread::sleep_for(std::chrono::milliseconds(250));
		if (!m->running.load(std::memory_order_acquire) || !m->engine)
			continue;
		const u64 taps = m->taps.load(std::memory_order_acquire);
		if (taps != m->last_taps.load(std::memory_order_relaxed)) {
			m->last_taps.store(taps, std::memory_order_relaxed);
			m->last_seen_ticks.store(uint64_t(mach_absolute_time()), std::memory_order_relaxed);
			continue;
		}
		const u64 seen = uint64_t(mach_absolute_time());
		if ((seen - m->last_seen_ticks.load(std::memory_order_relaxed)) / mach_tps() < 1.0)
			continue;
		m->last_seen_ticks.store(seen, std::memory_order_relaxed);
		restart();
	}
}

void apple_audio_in::stop()
{
	if (!m->running.exchange(false))
		return;
	// The watchdog first, before the engine goes: it must not decide to restart
	// an engine that is on its way out, and its thread holds this impl.
	m->watch_run.store(false, std::memory_order_release);
	if (m->watchdog.joinable())
		m->watchdog.join();
	// Stop first so no new tap fires, then release the tap and the engine: a
	// block already inside still holds the raw impl pointer.
	AVAudioEngine *engine = m->engine;
	m->engine = nil;
	if (engine) {
		[[engine inputNode] removeTapOnBus:0];
		[engine stop];
	}
}

bool apple_audio_in::running() const
{
	return m->running.load(std::memory_order_acquire);
}

const std::string &apple_audio_in::device_name() const
{
	return m->dev_name;
}

std::string apple_audio_in::format_line() const
{
	return m->fmt_line;
}

u64 apple_audio_in::empty_count() const
{
	return m->m_empty.load(std::memory_order_relaxed);
}

u64 apple_audio_in::dropped_count() const
{
	return m->m_dropped.load(std::memory_order_relaxed);
}

void apple_audio_in::pop(s32 &l, s32 &r)
{
	apple_audio_in::impl &im = *m;
	u32 rd = im.m_r.load(std::memory_order_relaxed);
	const u32 wr = im.m_w.load(std::memory_order_acquire);
	u32 level = (wr - rd) & apple_audio_in::impl::MASK;
	if (level > apple_audio_in::impl::DROP_FRAMES) {
		// Too far behind to catch up frame by frame: jump to a sane distance and
		// say so, which is what the counter is for.
		rd = (wr - apple_audio_in::impl::TARGET_FRAMES) & apple_audio_in::impl::MASK;
		level = apple_audio_in::impl::TARGET_FRAMES;
		im.m_dropped.fetch_add(1, std::memory_order_relaxed);
	}
	if (!level) {
		l = r = 0;
		if (running())
			im.m_empty.fetch_add(1, std::memory_order_relaxed);
		return;
	}
	l = im.m_ring[rd * 2];
	r = im.m_ring[rd * 2 + 1];
	im.m_r.store((rd + 1) & apple_audio_in::impl::MASK, std::memory_order_relaxed);
}


// ---- audio_out: the class, shared by both platforms ------------------------
//
// Every method is one line, because the work behind it is above: the render
// path, the engine, the nodes, the counters. Which platform owns a given
// question is answered in namespace apple, which is all the platform files
// contain.

struct audio_out::impl {
	std::unique_ptr<apple_audio_out> core = std::make_unique<apple_audio_out>();
};

audio_out::audio_out()
	: m_impl(std::make_unique<impl>())
{
}

audio_out::~audio_out()
{
	stop();
}

std::vector<std::string> audio_out::list()
{
	return apple::output_list();
}

bool audio_out::start(int latency_ms, fill_fn fill, std::string &err, bool exclusive,
                      const std::string &device, bool raw, bool exact)
{
	// raw bypasses the system mixer, and there is nothing to bypass on either
	// platform: the system does the format conversion rather than a driver
	// mixer. It is in the signature only so both take the same call.
	(void)raw;
	apple_audio_out::request r;
	r.latency_ms = latency_ms;
	r.device = device;
	r.exact = exact;
	// exclusive asks for the device outright. macOS has hog mode and the core
	// takes it through this file's take_output(); iOS has one route that is
	// always mixed and its answer says so, so the request is simply ignored
	// there rather than failing the open - which is why this is one line here
	// and the difference lives in the answers.
	r.exclusive = exclusive;
	if (!m_impl->core->start(r, std::move(fill), err))
		return false;
	// Watch the session while we are running (see watch_output_session): the
	// engine stops itself when headphones appear or a call arrives.
	apple::watch_output_session([core = m_impl->core.get()] { core->restart(); });
	return true;
}

void audio_out::stop()
{
	apple::watch_output_session(nullptr);
	m_impl->core->stop();
}

std::string audio_out::device_name() const
{
	return m_impl->core->device_name();
}

bool audio_out::exclusive() const
{
	return m_impl->core->exclusive();
}

void *audio_out::realtime_workgroup()
{
	return m_impl->core->realtime_workgroup();
}

void audio_out::set_capture(const std::string &path)
{
	// The class carries the flag and the path outside impl on purpose (stop()
	// throws impl away, and what was captured has to outlive it), so both are
	// set here as the Linux backend sets them - and the core keeps its own pair,
	// which is the one the render block reads.
	m_cap_path = path;
	m_capturing = !path.empty();
	m_impl->core->set_capture(path);
}

u64 audio_out::capture_frames() const
{
	return m_impl->core->capture_frames();
}

bool audio_out::write_capture(std::string &err)
{
	return m_impl->core->write_capture(err);
}

u64 audio_out::produced() const
{
	return m_impl->core->produced();
}

u32 audio_out::buffer_frames() const
{
	return m_impl->core->buffer_frames();
}

u64 audio_out::starved() const
{
	return m_impl->core->starved();
}

bool audio_out::mmcss() const
{
	return m_impl->core->mmcss();
}

double audio_out::cpu_percent() const
{
	return m_impl->core->cpu_percent();
}

double audio_out::worst_ms() const
{
	return m_impl->core->worst_ms();
}

double audio_out::cpu_recent() const
{
	return m_impl->core->cpu_recent();
}


// ---- audio_in: the class, shared by both platforms ------------------------
//
// One line per method, for the same reason audio_out's are: the tap, the ring,
// the resampler and the counters are above.

struct audio_in::impl {
	std::unique_ptr<apple_audio_in> core = std::make_unique<apple_audio_in>();
};

audio_in::audio_in()
	: m_impl(std::make_unique<impl>())
{
}

audio_in::~audio_in()
{
	stop();
}

std::vector<std::string> audio_in::list()
{
	return apple::input_list();
}

bool audio_in::start(const std::string &device, std::string &err)
{
	if (!m_impl->core->start(device, err))
		return false;
	// A route change stops the input engine too (mic unplugged, category
	// flipped), so it is watched the same way output is.
	apple::watch_input_session([core = m_impl->core.get()] { core->restart(); });
	return true;
}

void audio_in::stop()
{
	apple::watch_input_session(nullptr);
	m_impl->core->stop();
}

bool audio_in::running() const
{
	return m_impl->core->running();
}

void audio_in::pop(s32 &l, s32 &r)
{
	m_impl->core->pop(l, r);
}

std::string audio_in::device_name() const
{
	return m_impl->core->device_name();
}

std::string audio_in::format_line() const
{
	return m_impl->core->format_line();
}

u64 audio_in::empty_count() const
{
	return m_impl->core->empty_count();
}

u64 audio_in::dropped_count() const
{
	return m_impl->core->dropped_count();
}

} // namespace ui
