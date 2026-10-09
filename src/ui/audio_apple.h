// license:BSD-3-Clause
//
// One audio render path for both Apple front ends.
//
// macOS and iOS differ in what surrounds the audio, not in the audio itself.
// The session - category, permission, ports, route and interruption observers -
// is iOS only. Device enumeration and hog mode are macOS only. The part in
// between is one implementation, here: the engine, the source node, the render
// block, the resampler, the meters, the capture and the workgroup read.
//
// The engine is AVAudioEngine on both systems, and its input and output nodes
// hand out the underlying AudioUnit (AVAudioIONode.audioUnit). So pinning a
// device, asking for a buffer size and reading the workgroup all go through the
// AudioUnit properties on both platforms, and RemoteIO is not involved.
//
// What stays per platform is the short list of questions the engine cannot
// answer by itself: declared at the bottom of this file, answered in
// audio_out_mac.cpp and audio_in_mac.cpp (the HAL) and audio_out_ios.mm and
// audio_in_ios.mm (the session), with audio_out's and audio_in's own methods
// here beside the code they forward to.

#ifndef S_MU2000_UI_AUDIO_APPLE_H
#define S_MU2000_UI_AUDIO_APPLE_H

#include "ui/audio_out.h"

#include <AudioToolbox/AudioToolbox.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace ui {

// The output half: an AVAudioSourceNode whose render block calls fill (44100 Hz
// s16 stereo interleaved, the contract everywhere) and converts to the float
// the engine pulls. It also owns the device workgroup handle, so the parallel
// slave thread joins the same group on both platforms.
class apple_audio_out {
public:
	using fill_fn = audio_out::fill_fn;

	// What was asked for. An empty device means "whatever the platform routes
	// to": the whole of what iOS can offer, and the first entry of the macOS
	// device menu.
	struct request {
		int       latency_ms = 0;
		std::string device;
		bool      exact = false;     // a menu name must match a whole name
		bool      exclusive = false; // macOS: take the device for ourselves
	};

	apple_audio_out();
	~apple_audio_out();

	bool start(const request &r, fill_fn fill, std::string &err);
	void stop();

	// A route change or an interruption stops the engine and nothing resumes
	// it; the platform's observer calls this. The nodes and the render block
	// survive, so this is a restart and not a rebuild.
	void restart();

	bool running() const;
	const std::string &device_name() const;
	bool exclusive() const;
	void *realtime_workgroup();

	// What we hand the unit, written as a WAV: what the machine made, before
	// any format conversion, which is what a capture on either platform means.
	void set_capture(const std::string &path);
	u64 capture_frames() const;
	bool write_capture(std::string &err);

	u64 produced() const;
	u32 buffer_frames() const;
	u64 starved() const;
	bool mmcss() const;
	double cpu_percent() const;
	double worst_ms() const;
	double cpu_recent() const;

	apple_audio_out(const apple_audio_out &) = delete;
	apple_audio_out &operator=(const apple_audio_out &) = delete;

private:
	struct impl;
	std::unique_ptr<impl> m;
	// Outside impl so the capture survives stop(), the way audio_out's does.
	std::vector<s16> m_cap;
	std::string m_cap_path;
};

// The input half: the machine's A/D INPUT. The engine's input node with a tap,
// into the same ring and resampler as everywhere else (44100 Hz s16 stereo, a
// 50 ms target, dropped past 200 ms). The two clocks drift - the device runs at
// its own rate and the machine at 44100 - so the ring is the elastic part between
// them, exactly as audio_in.h describes.
//
// A second engine, on purpose: stopping output must not stop recording, and the
// reverse. The one thing both halves share is the session underneath them.
class apple_audio_in {
public:
	apple_audio_in();
	~apple_audio_in();

	// An empty name means the first device available, the default rule of every
	// backend here. macOS takes the remembered name whole; iOS's is a port the
	// session offers.
	bool start(const std::string &device, std::string &err);
	void stop();

	// A route change or an interruption stops the input engine too.
	void restart();

	bool running() const;
	const std::string &device_name() const;
	std::string format_line() const;
	u64 empty_count() const;
	u64 dropped_count() const;

	// The machine's next sample pair. Silent when there is nothing queued, which
	// is counted: a synth that never asks for input must not look starved.
	void pop(s32 &l, s32 &r);

	apple_audio_in(const apple_audio_in &) = delete;
	apple_audio_in &operator=(const apple_audio_in &) = delete;

private:
	struct impl;
	std::unique_ptr<impl> m;
};

// ---- The platform half ------------------------------------------------------
//
// One function per question the engine cannot answer, or per action only that
// system has a way to take. Implemented in audio_out_ios.mm and audio_out_mac.cpp;
// nothing above this line branches on the platform.
namespace apple {

// A device a request means. Resolved once and then passed around, so the buffer
// size, the pin, the hog and the label all describe the same device rather than
// four independent guesses at what "AirPods" means a second later.
struct device_ref {
	// The platform's own handle for it: an AudioDeviceID on macOS, and always
	// 0 on iOS, which has no HAL to ask (AudioDeviceID itself is not even
	// declared there - that is the whole reason this file exists). The id is
	// carried as the plain UInt32 both are.
	UInt32      id = 0;          // 0 is kAudioObjectUnknown
	std::string name;            // what to show for it
	bool        found = true;    // false when a name was given and nothing matches
};

// What a device claim is: which device, and whether taking it is what got
// it (in which case it has to be given back).
struct device_claim {
	UInt32 id = 0;
	bool   took = false;
};

// The session. iOS sets the category, asks for a rate and an IO buffer
// duration, and activates; macOS has no session, so it says yes and does
// nothing. There is deliberately no session_close(): deactivating belongs to
// whoever else shares the session (the input half does), and a half-answer
// would only invite the bug.
bool session_open(int latency_ms, std::string &err);

// Devices that can be played through, by name, for the device menu. macOS:
// every device with an output stream. iOS: the session's current route, which
// is the only choice the system offers.
std::vector<std::string> output_list();

// Which device a name means. macOS: the HAL lookup this file has always done -
// whole-name first when exact, then a substring, case-insensitively - with an
// empty name meaning the system default. iOS: the route, whatever it is called.
device_ref resolve_output(const std::string &name, bool exact);

// Ask for a buffer of about latency_ms and report what was granted. macOS
// writes the device's buffer frame size, the same call this file made on a unit
// of its own, and reads back what the driver accepted. iOS asked the session
// already and answers 0, which the core reads as "the platform decides".
u32 request_buffer_frames(const device_ref &dev, int latency_ms);

// Point the output at that device, on the unit the engine handed us. macOS sets
// kAudioOutputUnitProperty_CurrentDevice, which is the property a hand-written
// backend sets on a unit of its own; iOS has nothing to do, the session chose.
bool pin_output(AudioUnit unit, const device_ref &dev, std::string &err);
void unpin_output();

// Take the device for ourselves, so nothing else can play through it. macOS hog
// mode, with the same pid semantics and the same "claim after IO has started"
// rule as before. iOS has no counterpart: nothing can share the route through
// us, and the system mixer is not ours to take over.
device_claim take_output(const device_ref &dev, std::string &err);
void release_output(const device_claim &claim);

// The name to show for what was opened: the device on macOS, the route on iOS.
std::string output_label(const device_ref &dev, double rate);

// ---- The session watchers --------------------------------------------------
//
// The engine stops itself when the route changes or a call arrives, and nothing
// restarts it, so each half asks to be told and calls its own restart() from the
// callback. The platform owns the observers and calls back only while the
// function is set - pass an empty one to stop being called, which is what
// stop() does. It is a function rather than a token the caller holds because the
// session is a process-wide object: keeping the tokens per device would mean a
// callback outliving the core it points at.
//
// iOS watches AVAudioSession. macOS has no session and nothing that stops an
// engine behind our back, so both are no-ops there.
void watch_output_session(const std::function<void()> &on_change);
void watch_input_session(const std::function<void()> &on_change);

// ---- The input side, same questions ----------------------------------------

// Devices that can be recorded from, for the menu. macOS: every device with an
// input stream. iOS: the session's available inputs, which is hardware
// capability rather than the current route (the route's inputs are empty until
// a session category asks for them).
std::vector<std::string> input_list();

// May we record? iOS asks the microphone permission here and only here, so a
// launch that never records never prompts; an undetermined answer fails this
// pick with "pick again", a refusal says where to re-allow. macOS: yes.
bool input_permission(std::string &err);

// The session, for recording. iOS: PlayAndRecord with DefaultToSpeaker (without
// that the speaker goes quiet and sound moves to the earpiece the moment
// recording starts), then activate. macOS: yes.
bool session_open_input(std::string &err);

// Which device a name means, the same rule as the output side.
device_ref resolve_input(const std::string &name, bool exact);

// Point recording at that device. macOS: enable the unit's input bus, disable
// its output bus, and set kAudioOutputUnitProperty_CurrentDevice - the property
// a hand-written backend sets on a unit of its own, now set on the unit the
// engine hands out, which is what makes a menu selection take effect. iOS: ask
// the session for that port, since there is no HAL there to ask.
bool pin_input(AudioUnit unit, const device_ref &dev, std::string &err);

// The line the front ends print under the input device: each platform names
// its own path, and neither has to be told what the other does.
std::string input_label(const device_ref &dev, double rate, u32 channels);

} // namespace apple

} // namespace ui

#endif // S_MU2000_UI_AUDIO_APPLE_H