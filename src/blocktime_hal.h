// license:BSD-3-Clause
//
// blocktime's silent HAL unit: a started-but-silent output unit, so a real,
// ticking audio workgroup exists for the benchmark threads to join (see
// src/compat/realtime.h). Only the slave joins it: joining the main thread
// too double-counts against the slave through inheritance and traps in
// tsd_cleanup at exit ("Joined count underflowed"). This mirrors
// production, where gui's main thread never joins either. Tear-down stops
// the unit. Apple-only; elsewhere this is a stub that stays down.

#ifndef S_MU2000_BLOCKTIME_HAL_H
#define S_MU2000_BLOCKTIME_HAL_H

#pragma once

#if defined(__APPLE__)
#include <AudioToolbox/AudioToolbox.h>
#include <os/workgroup.h>

#include <cstring>
#endif

#include <string>

struct silent_hal {
#if defined(__APPLE__)
	AudioUnit unit = nullptr;
	os_workgroup_t wg = nullptr;

	// Silence render callback: the HAL pulls, we hand back zeros.
	static OSStatus silence_cb(void *, AudioUnitRenderActionFlags *,
	                           const AudioTimeStamp *, UInt32, UInt32,
	                           AudioBufferList *ioData)
	{
		if (!ioData)
			return noErr;
		for (UInt32 i = 0; i < ioData->mNumberBuffers; i++)
			std::memset(ioData->mBuffers[i].mData, 0,
			            ioData->mBuffers[i].mDataByteSize);
		return noErr;
	}

	bool start()
	{
		AudioComponentDescription desc{};
		desc.componentType = kAudioUnitType_Output;
		desc.componentSubType = kAudioUnitSubType_DefaultOutput;
		desc.componentManufacturer = kAudioUnitManufacturer_Apple;
		AudioComponent comp = AudioComponentFindNext(nullptr, &desc);
		if (!comp)
			return false;
		if (AudioComponentInstanceNew(comp, &unit) != noErr || !unit)
			return false;
		AURenderCallbackStruct cb{};
		cb.inputProc = silence_cb;
		cb.inputProcRefCon = nullptr;
		if (AudioUnitSetProperty(unit, kAudioUnitProperty_SetRenderCallback,
		                         kAudioUnitScope_Input, 0, &cb, sizeof(cb)) != noErr)
			return false;
		if (AudioUnitInitialize(unit) != noErr)
			return false;
		if (__builtin_available(macOS 11.0, *)) {
			UInt32 size = sizeof(wg);
			if (AudioUnitGetProperty(unit, kAudioOutputUnitProperty_OSWorkgroup,
			                         kAudioUnitScope_Global, 0, &wg, &size) != noErr)
				wg = nullptr;
		}
		if (AudioOutputUnitStart(unit) != noErr)
			return false;
		return true;
	}

	~silent_hal()
	{
		if (unit) {
			AudioOutputUnitStop(unit);
			AudioUnitUninitialize(unit);
			AudioComponentInstanceDispose(unit);
		}
	}
#else
	// Nothing to join elsewhere; wg stays null so the hooks below
	// compile unchanged and never fire.
	void *wg = nullptr;
	bool start() { return false; }
#endif
};

#endif // S_MU2000_BLOCKTIME_HAL_H
