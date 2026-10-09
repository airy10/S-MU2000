// license:BSD-3-Clause
//
// The macOS half of Apple audio: the questions the engine cannot answer, and
// nothing else.
//
// The render path - the render callback, the meters, the workgroup, the WAV
// writer - is audio_apple.mm, which iOS uses too. What macOS alone can do:
// enumerate devices by HAL property query, resolve a name to one (whole-name
// first for a menu selection, then a substring, case-insensitively), resize that
// device's buffer, and take it for ourselves in hog mode. Every one of those
// goes through properties on the very AudioUnit AVAudioEngine hands out, which
// is why the two halves meet at ui/audio_apple.h.
//
// The rule from doc/design.md carries over unchanged: **we own no clock**.
// CoreAudio asks for N frames and we make exactly those N.

#include "audio_out.h"
#include "audio_apple.h"
#include "compat/cli_text.h"
#include "hal_mac.h"

#include <algorithm>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace ui {


// ---- The macOS answers to the shared core (see ui/audio_apple.h) ------------
//
// One function per question ui/audio_apple.mm asks, and no branch anywhere else
// on the platform.

namespace apple {

// No session on macOS: there is no category to pick, nothing to activate and no
// permission to ask - the system default device is chosen by the system, and a
// device that goes away is answered by the HAL rather than by a notification.
// So the honest answer is yes, and the shared core takes the engine from here.
bool session_open(int, std::string &)
{
	return true;
}

std::vector<std::string> output_list()
{
	return hal::names(hal::direction::output);
}

// Which device a name means, and what to call it. Empty means the system
// default. A device that has gone leaves found false, so the caller can say so
// instead of opening whatever is left.
device_ref resolve_output(const std::string &name, bool exact)
{
	device_ref dev;
	dev.id = hal::find_device(name, exact);
	dev.found = dev.id != kAudioObjectUnknown;
	if (dev.found)
		dev.name = hal::name_of(dev.id);
	return dev;
}

// Best effort, as it always was: ask for a buffer matching the requested latency
// and report what the driver took. Zero means the write failed and the core
// falls back to whatever the first block turns out to be.
u32 request_buffer_frames(const device_ref &dev, int latency_ms)
{
	if (dev.id == kAudioObjectUnknown)
		return 0;
	return hal::set_buffer_frames(dev.id, latency_ms);
}

// The same property this file set on a unit of its own, now set on the unit the
// engine hands out. Note what it means, unchanged: the unit is pinned even for
// the system default, so a later change of the system default does not move us.
bool pin_output(AudioUnit unit, const device_ref &dev, std::string &err)
{
	if (unit == nullptr || dev.id == kAudioObjectUnknown)
		return true;
	if (AudioUnitSetProperty(unit, kAudioOutputUnitProperty_CurrentDevice,
	                         kAudioUnitScope_Global, 0, &dev.id, sizeof(dev.id)) != noErr) {
		err = CLI_T("Cannot select the audio output", "音声の出口を選べない");
		return false;
	}
	return true;
}

void unpin_output()
{
	// Nothing is remembered here: the pin is a property of the unit, and the
	// unit dies with the engine.
}

// Hog mode, with the same two rules as before. It is claimed after IO has
// started, because claiming first can leave a device that cannot be mixed
// unopenable. And it is given back only when taking it is what got it: a device
// some other process holds is not ours to release.
device_claim take_output(const device_ref &dev, std::string &err)
{
	device_claim claim;
	if (dev.id == kAudioObjectUnknown) {
		err = CLI_T("No audio output found", "音声の出口が見つからない");
		return claim;
	}
	claim.id = dev.id;
	bool took = false;
	if (!hal::take_hog(dev.id, took)) {
		std::fprintf(stderr, "[mac] hog refused: %s\n", dev.name.c_str());
		return claim;
	}
	claim.took = took;
	return claim;
}

void release_output(const device_claim &claim)
{
	if (claim.took && claim.id != kAudioObjectUnknown)
		hal::release_hog(claim.id);
}

// The device's own name, which is what the status line and the menu compare
// against. A device with no name (should not happen) falls back to the rate.
std::string output_label(const device_ref &dev, double rate)
{
	if (dev.id != kAudioObjectUnknown && !dev.name.empty())
		return dev.name;
	char name[128] = {};
	std::snprintf(name, sizeof(name), "%.0f Hz", rate);
	return name;
}

} // namespace apple

} // namespace ui
