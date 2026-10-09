// license:BSD-3-Clause
//
// The iOS half of Apple audio, playback: the questions the engine cannot
// answer, and nothing else.
//
// The render path - engine, source node, render block, resampler, meters,
// capture, workgroup - is audio_apple.mm, shared with macOS. What is left is
// what only iOS can answer, and the answer to most of it is short: the route is
// the system's, one route at a time, described by AVAudioSession.currentRoute,
// so there is no device to enumerate (list() reports the route, which is what
// the picker shows), none to pin, none to hog and no buffer size to write - a
// hand-written AudioUnit would have to get the last three right itself, and
// getting one wrong means silence. The session itself lives in
// ui/session_ios.{h,mm}, which the recording half uses too; its counterpart on
// the macOS side is ui/session_mac.cpp, which has no session to watch.
//
// iOS ships no public AudioHardware HAL: AudioObject* appears in no header,
// only in CoreAudio.tbd, so the macOS half does not compile there at all.
//
// The engine's nodes hand out the very AudioUnit a hand-written backend would
// own, so device, buffer size, stream format and workgroup are the same
// properties on both systems.

#import <AVFAudio/AVFAudio.h>
#import <Foundation/Foundation.h>

#include "ui/session_ios.h"
#include "ui/audio_apple.h"
#include "ui/audio_out.h"

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace ui {

// ---- The answers, and nothing else -----------------------------------------
//
// audio_out's and audio_in's own methods are in audio_apple.mm, beside the code
// they forward to. What this file holds is the answers to the questions in
// ui/audio_apple.h that only this platform can answer.

// ---- The iOS answers to the shared core (see ui/audio_apple.h) --------------

namespace apple {

bool session_open(int latency_ms, std::string &err)
{
	return ios::session_open_output(latency_ms, err);
}

// The one route, by its port name ("iPhone Speaker", "AirPods", ...). Empty
// when nothing is attached, which the picker shows as empty rather than lying.
std::vector<std::string> output_list()
{
	return ios::output_port_names();
}

// Which device a name means: there is only ever the one route, so any name
// resolves to it and is found. A remembered name goes stale the moment AirPods
// connect, so refusing one here would turn a cosmetic mismatch into a silent
// app - the route is not ours to refuse.
device_ref resolve_output(const std::string &name, bool)
{
	device_ref dev;
	dev.id = 0;   // no HAL to name a device with
	dev.name = name;
	dev.found = true;
	return dev;
}

// Nothing to pin: the session chose the route and the unit follows it.
bool pin_output(AudioUnit, const device_ref &, std::string &)
{
	return true;
}

void unpin_output()
{
}

// No hog mode on iOS: nothing else can share the route through us, and the
// system mixer is not ours to take over. So nothing is ever given back either.
device_claim take_output(const device_ref &, std::string &)
{
	return device_claim();
}

void release_output(const device_claim &)
{
}

// The IO buffer duration was asked of the session in session_open(), and the
// device under the unit is not ours to resize. Zero says so.
u32 request_buffer_frames(const device_ref &, int)
{
	return 0;
}

std::string output_label(const device_ref &, double rate)
{
	char name[128] = {};
	std::snprintf(name, sizeof(name), "iOS %.0f Hz", rate);
	return name;
}

} // namespace apple

} // namespace ui
