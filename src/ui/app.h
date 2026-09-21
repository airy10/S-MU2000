// license:BSD-3-Clause
//
// The GUI application both graphical front ends are.
//
// gui.exe and the Mac GUI keep the same state (bridge, panel, player,
// button bar, remembered ports) and paint the same picture; only the event
// pump, the window system and the dialogs differ. That shared half lives
// here so a feature added on one side cannot be missed on the other. Each
// front end keeps its window class (WndProc / ui::mac_app) and forwards to
// these from thin per-platform shells.
//
// Slice 1: state + panel paint. Input dispatch, menu actions and lifecycle
// follow in later slices.

#ifndef S_MU2000_UI_APP_H
#define S_MU2000_UI_APP_H

#pragma once

#include <atomic>
#include <cstdio>
#include <string>

#include "ui/audio_out.h"
#include "ui/bridge.h"
#include "ui/engine.h"
#include "ui/keymap.h"
#include "ui/panel.h"
#include "ui/player.h"
#include "ui/snapshot.h"
#include "ui/status.h"
#include "ui/toolbar.h"

namespace ui {

struct engine;
class midi_in;
class midi_out;
class audio_in;

class app
{
public:
	app(bridge &b, midi_in *mi, midi_out &tha, midi_out &thb, midi_out &muo)
	    : br(b), midi(mi), thru_a(tha), thru_b(thb), mu_out(muo) {}

	// ---- shared state (both windows keep the same)

	bridge   &br;
	midi_in  *midi;                  // MIDI IN A-D (mu2000::MIDI_PORTS of them)
	midi_out &thru_a, &thru_b, &mu_out; // THRU A, THRU B, the machine's own OUT

	panel  panel;
	player play;
	toolbar bar;                     // the window button bar (not on --lcd)

	struct engine *eng = nullptr;    // set once the ROMs are loaded
	std::atomic<int> *state = nullptr; // the engine's, so menus can grey out
	bool lcd_only = false;           // --lcd: the LCD on its own
	std::string layout_path;

	// The remembered ports, by name (empty = default/unused). *_keep is the
	// name to fall back on when a port is not there (yet). Four entries,
	// like ui::SET_IN_KEYS (both front ends run 4 MIDI ports)
	std::string in_name[4];
	std::string in_keep[4];
	std::string out_name, out_name_b, out_name_mu;
	std::string out_keep, out_keep_b, out_keep_mu;
	std::string audio_name;          // the audio device, by name
	std::string ain_name;            // the recording device, by name
	std::string ain_keep;
	std::string card_path;           // the SmartMedia in the slot, by path
	bool keep_settings = false;      // --nomidi: leave the remembered alone

	audio_out *out = nullptr;        // set once the audio device is open
	audio_in  *ain = nullptr;        // set once the recording device is picked

	// ---- shared paint (the whole panel picture, status line included)

	void paint_into(HDC dc, int w)
	{
		snapshot s;
		br.read(s);
		const u64 pressed = br.buttons();
		char status[320] = {};
		if (out && out->produced()) {
			// starved() counts what Windows calls late(). output_ms (待ち)
			// is WASAPI-only, so only the drop count is shared (ui/status.h)
			char middle[32];
			std::snprintf(middle, sizeof(middle), "遅れ %llu",
			              (unsigned long long)out->starved());
			format_status_line(status, sizeof(status),
			                   s.voices_master + s.voices_slave,
			                   out->cpu_percent(), out->worst_ms(),
			                   middle,
			                   in_name[0].empty() ? "なし" : in_name[0].c_str(),
			                   out_name.empty() ? "なし" : out_name.c_str());
		}
		else
			std::snprintf(status, sizeof(status), "起動中...");
		panel.set_volume(br.gain());
		panel.paint(dc, s, pressed, status);
		// The bar paints after the panel (the panel fills everything)
		bar.paint(dc, w);
	}

	// ---- shared input decisions (both windows act the same way)

	// What a mouse press means. bar_window is a BAR_* id to open; menu asks
	// for the context menu at the point (each side picks which one); neither
	// set means press the panel. The strip order, the jack spots and the LCD
	// guard are the same on both, so this is decided once.
	struct mouse_hit {
		bool handled = false;
		bool menu = false;
		int bar_window = -1;
	};

	mouse_hit hit_test(int x, int y, bool right) const
	{
		mouse_hit h;
		if (lcd_only)
			return h;
		// A secondary click opens the port picker wherever it lands
		if (right) {
			h.handled = true;
			h.menu = true;
			return h;
		}
		// The strip first: it is not the panel, so nothing reaches the machine
		const int id = bar.hit(x, y);
		if (id >= 0) {
			h.handled = true;
			h.bar_window = id;
			return h;
		}
		if (y < toolbar::HEIGHT) {
			h.handled = true;            // the strip's gaps
			return h;
		}
		// The jacks and the card slot are pressed, not clicked: they open a
		// menu instead of moving a panel control
		if (panel.on_midi_jack(x, y) || panel.on_ad_input(x, y) ||
		    panel.on_card_slot(x, y) || panel.on_phones(x, y)) {
			h.handled = true;
			h.menu = true;
			return h;
		}
		return h;
	}

	// A character key (both sides extract these from their key codes).
	// True when eaten: the meaning of a letter lives in ui/keymap.h.
	bool handle_panel_key(int ch, bool down)
	{
		mu2000::button b = mu2000::button::count;
		if (!button_for_char(ch, b))
			return false;
		br.press(b, down);
		return true;
	}

	// The F4 native-engine toggle both sides offer (key and menu)
	void toggle_engine()
	{
		if (eng)
			eng->want_native_engine.store(eng->native_engine.load() ? 0 : 1);
	}

	void release_keys() { br.release_all(); }

	// ---- per-platform acts (thin shells implement these)

	// Open a PC window by BAR_* id (F2/F3, the strip, the menus)
	virtual void open_window_by_kind(int kind) = 0;
};

} // namespace ui

#endif // S_MU2000_UI_APP_H
