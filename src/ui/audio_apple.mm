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
#include <cstring>
#include <string>
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

	ui::resampler resamp;
	std::vector<s16> scratch;   // fill target at 44100 Hz; grown, never shrunk
	std::vector<s16> *cap = nullptr;   // set_capture()'s buffer, or null

	std::string dev_name;

	std::atomic<bool> running{false};
	std::atomic<bool> taken{false};   // macOS: the device is ours alone
	std::atomic<u32> buffer_frames{0};    // what the last block actually was
	u32 granted_frames = 0;               // what the platform granted, pre-roll
	double dev_rate = double(AUDIO_RATE);
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

	// Asked, not promised: the hardware may be at 48k, and the resampler
	// below covers the difference rather than playing at the wrong speed.
	double rate = [[out_node outputFormatForBus:0] sampleRate];
	if (!(rate > 0.0))
		rate = double(AUDIO_RATE);
	// The conversion is ours, on purpose, and it could be the engine's.
	//
	// AVAudioEngine will insert a sample-rate converter of its own if the
	// connection format's rate differs from the hardware's - Apple's header
	// names it, among the things a graph may contain ("any sample rate
	// conversion"), and its converter is at least as good as this one and quite
	// possibly better. Connecting at 44100 instead of at the device's rate would
	// hand the job over: the block would then be asked for n frames at 44100,
	// fill(n) would need no translation at all, and nothing but the s16-to-float
	// conversion would be left on the audio thread.
	//
	// We keep ui::resampler because it is the same code the Windows output and
	// the Linux input already use (Linux output sidesteps the question by
	// refusing a device that cannot do 44100), so this is one resampler in the
	// tree rather than two paths to reason about - and because our converter's
	// cost is a few percent of a core, which the spikes we are chasing are not.
	// Worth revisiting if the render block ever needs the room: connect the
	// source node at AUDIO_RATE, let the engine convert, and keep ui::resampler
	// for the input side.
	m->dev_rate = rate;
	m->resamp.configure(double(AUDIO_RATE), rate);
	if (!m->resamp.direct())
		std::fprintf(stderr, "[audio] resampling %.0f -> %.0f Hz\n",
		             double(AUDIO_RATE), rate);

	// Which device the request means, then its buffer size. Both before the
	// engine runs: macOS writes the device's buffer frame size here, which is
	// what --latency has always meant on that side, and the unit negotiates
	// against it once it opens.
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
			// The device wants n frames at its own rate; the machine makes them
			// at 44100. input_needed() says how many machine frames that takes,
			// so one fill() covers the callback exactly - no drift, no stash.
			const int want = im->resamp.direct() ? int(n) : im->resamp.input_needed(int(n));
			if (want > 0) {
				if (im->scratch.size() < size_t(want) * 2)
					im->scratch.resize(size_t(want) * 2);
				im->fill(im->scratch.data(), u32(want));
			}
			float *f = static_cast<float *>(abl->mBuffers[0].mData);
			const u32 floats = abl->mBuffers[0].mDataByteSize / sizeof(float);
			if (f && floats >= n * 2) {
				if (im->resamp.direct()) {
					const s16 *sv = im->scratch.data();
					for (u32 i = 0; i < n * 2; i++)
						f[i] = float(sv[i]) * (1.0f / 32768.0f);
				} else {
					im->resamp.push(im->scratch.data(), want);
					im->resamp.pull(f, int(n));
				}
			} else {
				zero_buffers(abl);
			}
			const u64 t1 = mach_absolute_time();
			const u64 busy = t1 - t0;
			im->busy_ticks.fetch_add(busy, std::memory_order_relaxed);
			im->meter.add(double(busy) / mach_tps(), double(n) / im->dev_rate);
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
			// to make the block than the block is worth. Counted on both
			// platforms now, where before only the AudioUnit callback had it.
			if (double(busy) / mach_tps() > double(n) / im->dev_rate)
				im->starved.fetch_add(1, std::memory_order_relaxed);
			// What the machine made, before any conversion, which is what a
			// capture means on both platforms. The only allocation on this
			// path, and only while --dump-dev asked for it.
			if (im->cap && want > 0)
				im->cap->insert(im->cap->end(), im->scratch.data(),
				                im->scratch.data() + size_t(want) * 2);
			// want, not n: produced() is a count of 44100 Hz machine frames, which
		// is what every consumer divides by AUDIO_RATE to get seconds (live.cpp
		// does, in its progress and its CPU-per-second at the end). n is the
		// device's frame count, so counting it says 48 kHz frames and reads 8.9%
		// long on a 48 kHz output - which is what every iPhone runs at. The
		// machine frames are already worked out above, one fill() per callback
		// with no drift, so this is the same number the audio left the machine in.
		im->produced.fetch_add(u64(want > 0 ? want : 0), std::memory_order_relaxed);
			if (isSilence)
				*isSilence = NO;
			return noErr;
		}];
	// Interleaved float32 stereo: the block above writes one buffer of LRLR, so
	// the connection format says so rather than converting behind our back.
	AVAudioFormat *fmt = [[AVAudioFormat alloc] initWithCommonFormat:AVAudioPCMFormatFloat32
	                                                      sampleRate:rate
	                                                        channels:2
	                                                     interleaved:YES];
	if (!fmt) {
		err = "cannot describe float32 stereo";
		return false;
	}
	[engine attachNode:src];
	NSError *e = nil;
	// The device is pinned before the connection, because the format is
	// negotiated against whichever device the unit holds.
	if (!apple::pin_output([out_node audioUnit], m->dev, err)) {
		[engine detachNode:src];
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
	m->dev_name = apple::output_label(m->dev, rate);
	m->running.store(true, std::memory_order_release);
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
		m->taken.store(m->claim.took, std::memory_order_relaxed);
		if (m->claim.took) {
			[engine stop];
			NSError *restart_err = nil;
			if (![engine startAndReturnError:&restart_err])
				std::fprintf(stderr, "[audio] restart after taking the device: %s\n",
				             restart_err ? [[restart_err localizedDescription] UTF8String] : "?");
		}
	}
	std::fprintf(stderr, "[audio] %s\n", m->dev_name.c_str());
	return true;
}

void apple_audio_out::restart()
{
	AVAudioEngine *engine = m->engine;
	if (!engine || !m->running.load(std::memory_order_acquire))
		return;
	[engine stop];
	NSError *e = nil;
	if (![engine startAndReturnError:&e])
		std::fprintf(stderr, "[audio] restart failed: %s\n",
		             e ? [[e localizedDescription] UTF8String] : "?");
}

void apple_audio_out::stop()
{
	if (!m->running.exchange(false))
		return;
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
	return 100.0 * double(busy) / (mach_tps() * double(prod) / m->dev_rate);
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

	// The tap asks for the device's own format, whatever that is. Asking for
	// 2ch interleaved instead looks harmless - the tap is accepted and the
	// engine starts - but on a mono device the block is then never called at
	// all, so the machine hears silence and nothing says why. Measured on
	// macOS with a bare engine: a 1ch device delivered 48000 frames in 10
	// callbacks on its own format and zero on a forced 2ch one. The block
	// below folds the channels instead.
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
	std::fprintf(stderr, "[audio] in: %s (%.0f Hz)\n", m->dev_name.c_str(), rate);
	return true;
}

void apple_audio_in::restart()
{
	AVAudioEngine *engine = m->engine;
	if (!engine || !m->running.load(std::memory_order_acquire))
		return;
	[engine stop];
	NSError *e = nil;
	if (![engine startAndReturnError:&e])
		std::fprintf(stderr, "[audio] in: restart failed: %s\n",
		             e ? [[e localizedDescription] UTF8String] : "?");
}

void apple_audio_in::stop()
{
	if (!m->running.exchange(false))
		return;
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
