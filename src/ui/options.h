// license:BSD-3-Clause
//
// Engine flags shared by every tool's command line (gui, live, render, ...).
//
// Each main() delegates these first, so a new engine flag needs no per-tool
// edits: add the flag here and every tool accepts and applies it the same
// way. Tool-specific flags stay in each main. Needs mu2000.h (every tool
// already includes it).

#ifndef S_MU2000_UI_OPTIONS_H
#define S_MU2000_UI_OPTIONS_H

#pragma once

#include <cstring>

#include "mu2000.h"

namespace ui {

// Engine flags shared by all tools: fast MIDI serial pacing, the
// lightweight C++ effects (0 off, 1 added alongside the MEG, 2 replacing
// it), and the native ports (0 off, 1 on). The effects flags apply before
// loading; the native ports only after boot, at a point each tool picks
// itself, so only their parsing is shared.
struct engine_options {
	bool fast_midi = false;
	int  native_fx = 0;
	int  native_engine = 0;
};

// Takes a single argv entry. True when it was a shared engine flag.
inline bool consume_engine_option(const char *arg, engine_options &o)
{
	if (!std::strcmp(arg, "--fast-midi")) { o.fast_midi = true; return true; }
	if (!std::strcmp(arg, "--native-fx")) { o.native_fx = 1; return true; }
	if (!std::strcmp(arg, "--native-fx-full")) { o.native_fx = 2; return true; }
	if (!std::strcmp(arg, "--native-engine")) { o.native_engine = 1; return true; }
	return false;
}

// Applies the flags, as every tool does before loading.
inline void apply_engine_options(mu2000 &mu, const engine_options &o)
{
	mu.set_fast_midi(o.fast_midi);
	if (o.native_fx)
		mu.set_native_fx(o.native_fx);
}

} // namespace ui

#endif // S_MU2000_UI_OPTIONS_H
