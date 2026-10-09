// license:BSD-3-Clause
//
// The AudioHardware HAL queries the macOS half of Apple audio needs, in one
// place.
//
// These are the questions AVAudioEngine cannot answer: which devices exist,
// what they are called, which one a remembered name means, how big their
// buffer may be, and whether we can have one to ourselves. Playback and
// recording ask the same questions of the same HAL, so the queries take a
// direction rather than being written twice - two copies of name_of() and
// lowered() are two places for the two sides to drift apart.
//
// macOS only: iOS ships no public HAL (AudioObject* appears in no header), so
// nothing here compiles there - which is exactly why the answers to these
// questions live behind ui/audio_apple.h's hooks instead.

#ifndef S_MU2000_UI_HAL_MAC_H
#define S_MU2000_UI_HAL_MAC_H

#include "ui/audio_out.h"   // AUDIO_RATE and the u32/u8 spellings

#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <unistd.h>          // getpid(), for hog mode

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <vector>

namespace ui::hal {

// Which way round a device question is being asked. The HAL is one list of
// devices with a scope per direction, so every query below takes this.
enum class direction { output, input };

// The system's default device for that direction, or kAudioObjectUnknown.
inline AudioDeviceID default_device(direction dir)
{
	AudioObjectPropertyAddress addr = {
		dir == direction::output ? kAudioHardwarePropertyDefaultOutputDevice
		                         : kAudioHardwarePropertyDefaultInputDevice,
		kAudioObjectPropertyScopeGlobal,
		kAudioObjectPropertyElementMain
	};
	AudioDeviceID dev = kAudioObjectUnknown;
	UInt32 size = sizeof(dev);
	if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0, nullptr,
	                               &size, &dev) != noErr)
		return kAudioObjectUnknown;
	return dev;
}

// Does this device have anything at all in that direction? The device list
// holds output-only and input-only devices alike, and offering the wrong one
// would be a lie.
inline bool has_stream(AudioDeviceID dev, direction dir)
{
	AudioObjectPropertyAddress addr = {
		kAudioDevicePropertyStreams,
		dir == direction::output ? kAudioObjectPropertyScopeOutput
		                         : kAudioObjectPropertyScopeInput,
		kAudioObjectPropertyElementMain
	};
	UInt32 size = 0;
	if (AudioObjectGetPropertyDataSize(dev, &addr, 0, nullptr, &size) != noErr)
		return false;
	return size >= sizeof(AudioStreamID);
}

// Every device that can do that direction.
inline std::vector<AudioDeviceID> devices(direction dir)
{
	AudioObjectPropertyAddress addr = {
		kAudioHardwarePropertyDevices,
		kAudioObjectPropertyScopeGlobal,
		kAudioObjectPropertyElementMain
	};
	UInt32 size = 0;
	if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &addr, 0, nullptr, &size) != noErr)
		return {};
	std::vector<AudioDeviceID> devs(size / sizeof(AudioDeviceID));
	if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0, nullptr, &size,
	                               devs.data()) != noErr)
		return {};
	devs.erase(std::remove_if(devs.begin(), devs.end(),
	                          [dir](AudioDeviceID d) { return !has_stream(d, dir); }),
	           devs.end());
	return devs;
}

// Named rather than called device_name(), which would collide with the member
// function of the same name wherever one is in scope.
inline std::string name_of(AudioDeviceID dev)
{
	AudioObjectPropertyAddress addr = {
		kAudioObjectPropertyName,
		kAudioObjectPropertyScopeGlobal,
		kAudioObjectPropertyElementMain
	};
	CFStringRef name = nullptr;
	UInt32 size = sizeof(name);
	if (AudioObjectGetPropertyData(dev, &addr, 0, nullptr, &size, &name) != noErr || !name)
		return {};
	char buf[256] = {};
	const bool ok = CFStringGetCString(name, buf, sizeof(buf), kCFStringEncodingUTF8);
	CFRelease(name);
	return ok ? std::string(buf) : std::string();
}

// The names, for the menu.
inline std::vector<std::string> names(direction dir)
{
	std::vector<std::string> out;
	for (AudioDeviceID d : devices(dir)) {
		const std::string n = name_of(d);
		if (!n.empty())
			out.push_back(n);
	}
	return out;
}

inline std::string lowered(const std::string &s)
{
	std::string out;
	out.reserve(s.size());
	for (char c : s)
		out.push_back(char(std::tolower((unsigned char)c)));
	return out;
}

// Which device a remembered name means. Empty means the system default. The
// rule has not moved: a whole-name match first, then a substring one, both
// case-insensitively - except for a menu selection, which passes exact and so
// accepts nothing but a whole name. That is what stops a device that has gone
// from quietly becoming another one whose name happens to contain the old one.
inline AudioDeviceID find_device(const std::string &want, bool exact,
                                 direction dir = direction::output)
{
	if (want.empty())
		return default_device(dir);
	const std::string needle = lowered(want);
	const std::vector<AudioDeviceID> devs = devices(dir);
	for (int pass = 0; pass < (exact ? 1 : 2); pass++) {
		for (AudioDeviceID d : devs) {
			const std::string n = lowered(name_of(d));
			if (n == needle || (!exact && pass == 1 && n.find(needle) != std::string::npos))
				return d;
		}
	}
	return kAudioObjectUnknown;
}

// How many channels a device records, read from its stream configuration (a
// list of buffers, one per stream). 0 on any failure.
inline u32 input_channels(AudioDeviceID dev)
{
	AudioObjectPropertyAddress addr = {
		kAudioDevicePropertyStreamConfiguration,
		kAudioObjectPropertyScopeInput,
		kAudioObjectPropertyElementMain
	};
	UInt32 size = 0;
	if (AudioObjectGetPropertyDataSize(dev, &addr, 0, nullptr, &size) != noErr || !size)
		return 0;
	std::vector<uint8_t> room(size);
	auto *list = reinterpret_cast<AudioBufferList *>(room.data());
	if (AudioObjectGetPropertyData(dev, &addr, 0, nullptr, &size, list) != noErr)
		return 0;
	u32 ch = 0;
	for (UInt32 i = 0; i < list->mNumberBuffers; i++)
		ch += list->mBuffers[i].mNumberChannels;
	return ch;
}

// The device's own rate. A hand-written backend asks for exactly this and lets
// the HAL convert; an engine negotiates the format instead and reports what it
// got, so this is for the label and for the fallback.
inline double nominal_rate(AudioDeviceID dev)
{
	AudioObjectPropertyAddress addr = {
		kAudioDevicePropertyNominalSampleRate,
		kAudioObjectPropertyScopeGlobal,
		kAudioObjectPropertyElementMain
	};
	Float64 rate = 0.0;
	UInt32 size = sizeof(rate);
	if (AudioObjectGetPropertyData(dev, &addr, 0, nullptr, &size, &rate) != noErr)
		return 0.0;
	return double(rate);
}

inline u32 buffer_frames(AudioDeviceID dev)
{
	AudioObjectPropertyAddress addr = {
		kAudioDevicePropertyBufferFrameSize,
		kAudioObjectPropertyScopeGlobal,
		kAudioObjectPropertyElementMain
	};
	UInt32 frames = 0;
	UInt32 size = sizeof(frames);
	if (AudioObjectGetPropertyData(dev, &addr, 0, nullptr, &size, &frames) != noErr)
		return 0;
	return frames;
}

// Best effort: ask a device for a buffer matching the requested latency, and
// report what it took. Zero means the write failed, and the caller falls back
// to whatever the first block turns out to be.
inline u32 set_buffer_frames(AudioDeviceID dev, int latency_ms)
{
	u32 wanted = u32(AUDIO_RATE) * u32(std::max(latency_ms, 0)) / 1000;
	if (wanted < 32)
		wanted = 32;
	// Never outside what the driver says it can do
	AudioObjectPropertyAddress range = {
		kAudioDevicePropertyBufferFrameSizeRange,
		kAudioObjectPropertyScopeGlobal,
		kAudioObjectPropertyElementMain
	};
	AudioValueRange r{};
	UInt32 rsize = sizeof(r);
	if (AudioObjectGetPropertyData(dev, &range, 0, nullptr, &rsize, &r) == noErr) {
		if (wanted < r.mMinimum)
			wanted = r.mMinimum;
		if (wanted > r.mMaximum)
			wanted = r.mMaximum;
	}
	AudioObjectPropertyAddress addr = {
		kAudioDevicePropertyBufferFrameSize,
		kAudioObjectPropertyScopeGlobal,
		kAudioObjectPropertyElementMain
	};
	UInt32 size = sizeof(wanted);
	if (AudioObjectSetPropertyData(dev, &addr, 0, nullptr, size, &wanted) != noErr)
		return 0;
	// What it actually took, not what we asked for
	UInt32 got = 0;
	size = sizeof(got);
	if (AudioObjectGetPropertyData(dev, &addr, 0, nullptr, &size, &got) != noErr)
		return wanted;
	return got;
}

// ---- Hog mode ---------------------------------------------------------------
//
// Taking the device for ourselves, so nothing else can play through it. This is
// the whole of exclusive() on macOS, and it is a *device* property: it does not
// care which unit drives the device, which is why it survives the move to
// AVAudioEngine unchanged.

inline AudioObjectPropertyAddress hog_address()
{
	return {kAudioDevicePropertyHogMode, kAudioObjectPropertyScopeGlobal,
	        kAudioObjectPropertyElementMain};
}

// The holder is a pid: -1 is nobody, our own pid means we already hold it, and
// anyone else's means the answer is no. took_it says whether writing our pid is
// what got us the device - a device we merely found already hogged by us must
// not be released on the way out.
inline bool take_hog(AudioDeviceID dev, bool &took_it)
{
	if (dev == kAudioObjectUnknown)
		return false;
	AudioObjectPropertyAddress addr = hog_address();
	pid_t holder = 0;
	UInt32 size = sizeof(holder);
	if (AudioObjectGetPropertyData(dev, &addr, 0, nullptr, &size, &holder) != noErr)
		return false;
	const pid_t me = getpid();
	if (holder == me)
		return true;                       // already ours; took_it stays false
	if (holder > 0)
		return false;                      // somebody else has it: do not write
	holder = me;
	size = sizeof(holder);
	if (AudioObjectSetPropertyData(dev, &addr, 0, nullptr, size, &holder) != noErr)
		return false;
	// Verify: a device that will not stay hogged must not be counted as hogged
	size = sizeof(holder);
	holder = 0;
	if (AudioObjectGetPropertyData(dev, &addr, 0, nullptr, &size, &holder) != noErr ||
	    holder != me)
		return false;
	took_it = true;
	return true;
}

inline void release_hog(AudioDeviceID dev)
{
	if (dev == kAudioObjectUnknown)
		return;
	AudioObjectPropertyAddress addr = hog_address();
	pid_t none = -1;
	UInt32 size = sizeof(none);
	AudioObjectSetPropertyData(dev, &addr, 0, nullptr, size, &none);
}

} // namespace ui::hal

#endif // S_MU2000_UI_HAL_MAC_H
