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
#include <thread>

#include "ui/audio_in.h"
#include "ui/audio_out.h"
#include "ui/bridge.h"
#include "ui/engine.h"
#include "ui/keymap.h"
#include "ui/menu.h"
#include "ui/panel.h"
#include "ui/player.h"
#include "ui/settings.h"
#include "ui/snapshot.h"
#include "ui/status.h"
#include "ui/toolbar.h"

#include "smartmedia.h"

#include <mutex>

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

	// Device indices being opened (-1 unused). Names above outlive them:
	// unplugging USB shifts numbers, so reconnects look the names up again
	int in_dev[4] = { -1, -1, -1, -1 };
	int out_dev = -1, out_dev_b = -1, out_dev_mu = -1;
	int ain_dev = -1;
	u64 reported_drops = 0;          // MIDI drops the UI thread last reported

	std::thread reboot;              // the factory-reset reboot, while it runs

	void join_reboot()
	{
		if (reboot.joinable())
			reboot.join();
	}

	// ---- shared paint (the whole panel picture, status line included)

	void paint_into(HDC dc, int w, const char *middle)
	{
		snapshot s;
		br.read(s);
		const u64 pressed = br.buttons();
		char status[320] = {};
		if (out && out->produced()) {
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

	void release_keys()
	{
		pressed = false;
		br.release_all();
	}

	// Panel layout from a file (F5 reads it back). Same file both sides
	void apply_layout(const std::string &path, bool quiet)
	{
		panel.lay() = layout();
		std::string err;
		if (!path.empty() && panel.lay().load(path, err)) {
			if (!quiet)
				std::printf("配置: %s\n", path.c_str());
		} else if (!path.empty() && !quiet) {
			std::printf("配置: %s を開けない。組み込みの配置を使う\n", path.c_str());
		}
		if (!err.empty())
			std::fprintf(stderr, "%s", err.c_str());
		std::fflush(stdout);
		panel.resize(panel.width(), panel.height());
	}

	void reload_layout() { apply_layout(layout_path, false); }

	// Which popup the point asks for. The card slot, PHONES and A/D INPUT
	// have their own; everywhere else gets the port picker
	std::vector<menu_group> menu_groups_for(int x, int y)
	{
		if (panel.on_card_slot(x, y))
			return menu_card(menu_snapshot());
		if (panel.on_phones(x, y))
			return menu_phones(eng && eng->analog.load());
		if (panel.on_ad_input(x, y))
			return menu_ain_only(audio_in::list(), ain_name);
		return menu_ports(menu_snapshot());
	}

	// What a mouse press does. bar_window opens through open_window_by_kind
	// and show_menu wants the popup; panel_pressed means the panel took it
	// (the side repaints). A press is remembered for drag/up
	struct mouse_out {
		bool panel_pressed = false;
		bool opened_window = false;
		bool show_menu = false;
	};

	mouse_out do_mouse_down(int x, int y, bool right)
	{
		mouse_out o;
		const mouse_hit h = hit_test(x, y, right);
		if (h.bar_window >= 0) {
			bar.set_down(h.bar_window);
			open_window_by_kind(h.bar_window);
			o.opened_window = true;
			pressed = true;
			return o;
		}
		if (h.handled) {
			o.show_menu = h.menu;
			return o;
		}
		pressed = true;
		o.panel_pressed = true;
		panel.press(x, y, br);
		return o;
	}

	bool do_mouse_drag(int x, int y)
	{
		if (lcd_only || !pressed)
			return false;
		return panel.drag(x, y, br);
	}

	void do_mouse_up()
	{
		if (!pressed)
			return;
		pressed = false;
		bar.set_down(-1);
		panel.release(br);
	}

	// A press all the way through, for windows whose infra asks only
	// whether a popup follows (the press state still feeds drag/up).
	// True when a popup follows
	bool press_at(int x, int y, bool right)
	{
		if (lcd_only)
			return false;
		const mouse_out o = do_mouse_down(x, y, right);
		return o.show_menu || o.opened_window;
	}

	bool do_wheel(int x, int y, int steps)
	{
		if (lcd_only || !steps)
			return false;
		return panel.wheel_at(x, y, steps, br);
	}

	bool hand_at(int x, int y) const
	{
		return panel.on_midi_jack(x, y) || panel.on_ad_input(x, y) ||
		       panel.on_card_slot(x, y) || panel.on_phones(x, y);
	}

	// ---- per-platform acts (thin shells implement these)

	// Open a PC window by BAR_* id (F2/F3, the strip, the menus)
	virtual void open_window_by_kind(int kind) = 0;

	// ---- remembered settings (gui.ini)

	// Where the file lives differs per platform (registry side vs Library)
	virtual std::string settings_path() const = 0;

	void save_settings()
	{
		if (keep_settings)                   // --nomidi: keep the ports
			return;
		const std::string path = settings_path();
		if (path.empty())
			return;
		remembered r;
		for (int p = 0; p < 4; p++)
			r.in[p] = in_name[p].empty() ? in_keep[p] : in_name[p];
		r.out       = out_name.empty()    ? out_keep    : out_name;
		r.out_b     = out_name_b.empty()  ? out_keep_b  : out_name_b;
		r.out_mu    = out_name_mu.empty() ? out_keep_mu : out_name_mu;
		r.audio_out = audio_name;
		r.audio_in  = ain_name.empty() ? ain_keep : ain_name;
		r.card      = card_path;
		r.volume    = br.gain();
		r.fold34    = play.fold_extra_ports();
		r.analog    = eng && eng->analog.load();
		write_settings_file(path, collect_settings(r));
	}

	static remembered load_remembered(const std::string &path)
	{
		remembered r;
		if (path.empty())
			return r;
		settings_map kv;
		if (!read_settings_file(path, kv))
			return r;
		apply_settings(kv, r);
		return r;
	}

	// ---- ports (the menus pick these)

	// Open what the menu picked, falling back to "unused". keep is true
	// only while starting up: the asked-for name is then kept even if the
	// port is not there yet. Failures print to stderr always and reach
	// menu_error only for menu picks (never boot-time).
	bool choose_in(int port, int dev, bool keep = false)
	{
		if (port < 0 || port >= 4)
			return false;
		if (!keep)
			in_keep[port].clear();
		std::string err;
		if (!midi[port].open(dev, err)) {
			std::fprintf(stderr, "%s: %s\n", IN_LABELS[port], err.c_str());
			if (!keep)
				menu_error(err);
			midi[port].open(-1, err);
			dev = -1;
		}
		in_dev[port]  = midi[port].is_open() ? dev : -1;
		in_name[port] = midi[port].device_name();
		save_settings();
		return dev >= 0;
	}

	bool choose_out(int dev, bool keep = false)
	{
		return open_out(thru_a, out_dev, out_name, out_keep,
		                "MIDI 出力", dev, keep);
	}

	bool choose_out_b(int dev, bool keep = false)
	{
		return open_out(thru_b, out_dev_b, out_name_b, out_keep_b,
		                "MIDI 出力 B", dev, keep);
	}

	bool choose_out_mu(int dev, bool keep = false)
	{
		return open_out(mu_out, out_dev_mu, out_name_mu, out_keep_mu,
		                "MIDI 出力（本体の OUT）", dev, keep);
	}

	bool choose_ain(int dev, bool keep = false)
	{
		if (!keep)
			ain_keep.clear();
		if (!ain)
			return false;
		ain->stop();
		if (dev < 0) {
			ain_name.clear();
		} else {
			const auto names = audio_in::list();
			if (dev < int(names.size())) {
				std::string err;
				if (!ain->start(names[size_t(dev)], err)) {
					std::fprintf(stderr, "A/D INPUT: %s\n", err.c_str());
					if (!keep)
						menu_error(err);
				} else {
					std::printf("A/D INPUT: %s（%s）\n",
					            ain->device_name().c_str(),
					            ain->format_line().c_str());
					std::fflush(stdout);
				}
				ain_name = names[size_t(dev)];
			}
		}
		save_settings();
		return true;
	}

	// ---- SmartMedia (the card slot)

	// Written-back blocks go to the file; only snapshotting stops the
	// audio thread. Called from the timers, and on eject/close/save
	void flush_card()
	{
		if (!eng || card_path.empty())
			return;
		std::vector<smu2000::smartmedia::block> blocks;
		{
			const std::lock_guard<std::mutex> hold(eng->card_lock);
			eng->mu.card().take_dirty_blocks(blocks);
		}
		if (blocks.empty())
			return;
		std::string err;
		if (!smu2000::smartmedia::write_blocks(card_path, blocks, err))
			std::fprintf(stderr, "SmartMedia: %s\n", err.c_str());
	}

	void card_tick()
	{
		const u64 now = smu2000::perf_ticks() * 1000 / smu2000::perf_freq();
		if (now - last_flush < 2000)
			return;
		last_flush = now;
		flush_card();
	}

	// A MIDI loop (THRU fed back into an IN) overflows the guards. Said out
	// loud once a second, from the window's timer rather than the paint
	void report_drops()
	{
		if (!eng)
			return;
		const u64 now = smu2000::perf_ticks() * 1000 / smu2000::perf_freq();
		if (now - last_drop_report < 1000)
			return;
		last_drop_report = now;
		const u64 drops = eng->guard_a.dropped() + eng->guard_b.dropped() +
		                  eng->mu.midi_dropped();
		if (drops == reported_drops)
			return;
		reported_drops = drops;
		std::fprintf(stderr,
		             "MIDI が多すぎるので捨てた: THRU A %llu / THRU B %llu / 受信 %llu バイト"
		             "（MIDI の輪ができていないか確かめる）\n",
		             (unsigned long long)eng->guard_a.dropped(),
		             (unsigned long long)eng->guard_b.dropped(),
		             (unsigned long long)eng->mu.midi_dropped());
	}

	// The window's timer work, both sides: feed the panel, publish CPU and
	// engine state for the PC windows, flush the card file, report drops.
	// Opening/drawing the PC windows stays per side (different window types)
	void poll()
	{
		panel.tick(br);
		if (out && out->produced())
			br.set_cpu(float(out->cpu_percent()));
		br.set_engine(eng ? eng->native_engine.load() : -1);
		card_tick();
		report_drops();
	}

	void eject_card()
	{
		if (!eng)
			return;
		flush_card();
		{
			const std::lock_guard<std::mutex> hold(eng->card_lock);
			eng->mu.card().eject();
		}
		if (!card_path.empty())
			std::printf("SmartMedia を抜いた: %s\n", card_path.c_str());
		std::fflush(stdout);
		card_path.clear();
		save_settings();
	}

	// Load it first, so a file that cannot be read does not take the slot
	// away from the card that is already in it. quiet is for boot, where a
	// missing file must stay silent
	bool insert_card(const std::string &path, bool quiet = false)
	{
		if (!eng)
			return false;
		smu2000::smartmedia card;
		std::string err;
		if (!card.load(path, err)) {
			std::fprintf(stderr, "SmartMedia: %s\n", err.c_str());
			if (!quiet)
				menu_error(err);
			return false;
		}
		eject_card();
		{
			const std::lock_guard<std::mutex> hold(eng->card_lock);
			eng->mu.card() = std::move(card);
		}
		card_path = path;
		std::printf("SmartMedia を差した: %s（%uMB）\n",
		            path.c_str(), eng->mu.card().megabytes());
		std::fflush(stdout);
		save_settings();
		return true;
	}

	// An empty card, in the physical layout a new one comes in. It has to
	// be formatted by the machine (UTIL -> CARD -> Format) before it holds
	// anything
	void new_card(u32 megabytes)
	{
		const std::string path = ask_card_save_path();
		if (path.empty())
			return;
		smu2000::smartmedia card;
		if (!card.create(megabytes)) {
			std::fprintf(stderr, "SmartMedia を作れない\n");
			return;
		}
		std::string err;
		if (!card.save(path, err)) {
			std::fprintf(stderr, "SmartMedia: %s\n", err.c_str());
			menu_error(err);
			return;
		}
		if (insert_card(path))
			menu_note("空の SmartMedia を差しました。\n"
			          "使う前に、本体の UTIL → CARD → Format で書式化してください。");
	}

	void do_card_open()
	{
		const std::string path = ask_card_open_path();
		if (!path.empty() && insert_card(path))
			save_settings();
	}

	// ---- MIDI file playback

	// Plays the file picked from a menu or dropped on a window. Restarts it
	// when it is already playing
	bool play_song(const std::string &path)
	{
		std::string err;
		if (!play.start(path, br, err)) {
			std::fprintf(stderr, "開けない: %s\n", err.c_str());
			menu_error("開けない: " + err);
			return false;
		}
		std::printf("再生: %s（%.1f 秒）\n", path.c_str(), play.length());
		// The machine has two ports, so a four-port file is either folded
		// onto them or has its extra parts dropped
		if (play.ports_used() > 2)
			std::printf("  この曲は %d 口ぶん。C・D は未対応なので、口 3 以降は%s\n",
			            play.ports_used(),
			            play.fold_extra_ports() ? " A・B に重ねて鳴らす" : "鳴らさない");
		std::fflush(stdout);
		return true;
	}

	void do_midi_file()
	{
		const std::string path = ask_midi_file_path();
		if (!path.empty())
			play_song(path);
	}

	// ---- the rest of the menu

	// Throwing the settings away means rebooting the machine, which takes
	// tens of seconds, so it runs on its own thread (joined first: two
	// boots at once would both be writing the machine)
	void do_factory_reset()
	{
		if (!eng || !state || state->load() != 1)
			return;
		if (!confirm_factory_reset())
			return;
		play.stop();
		join_reboot();
		reboot = std::thread([this] { eng->factory_reset(); });
	}

	void set_fold34(bool on)
	{
		play.set_fold_extra_ports(on);
		save_settings();
	}

	void set_analog(bool on)
	{
		if (!eng)
			return;
		// Digital matches S/PDIF (some DPCM samples keep their DC, as on
		// the hardware); analog cuts DC like LINE OUT and PHONES do
		eng->analog.store(on);
		std::printf("音の出口: %s\n", on ? "アナログ（直流を切る）" : "デジタル");
		std::fflush(stdout);
		save_settings();
	}

	void toggle_fx()
	{
		if (eng)
			eng->want_native_fx.store(eng->native_fx.load() ? 0 : 2);
	}

	// What the shared menu builders (ui/menu.h) show, from this window's state
	menu_state menu_snapshot()
	{
		menu_state s;
		s.midi_ins = midi_in::list();
		s.midi_outs = midi_out::list();
		s.audio_ins = audio_in::list();
		for (int p = 0; p < 4; p++)
			s.in_dev[p] = in_dev[p];
		s.out_dev = out_dev;
		s.out_dev_b = out_dev_b;
		s.out_dev_mu = out_dev_mu;
		s.ain_name = ain_name;
		s.card_path = card_path;
		s.playing = play.playing();
		s.play_name = play.name();
		s.fold34 = play.fold_extra_ports();
		s.ready = eng && state && state->load() == 1;
		s.native_fx = eng && eng->native_fx.load();
		s.native_engine = eng && eng->native_engine.load();
		return s;
	}

	// The shared dispatch for the 26 menu IDs both front ends render
	// (ui/menu.h). Only the dialogs and the error display are per-platform
	// (the hooks below); everything else is the same calls in the same order
	void menu_chosen(int id)
	{
		for (int p = 0; p < 4; p++) {
			const int none = ID_IN_NONE + p * ID_IN_STRIDE, base = ID_IN_BASE + p * ID_IN_STRIDE;
			if (id == none)                    { choose_in(p, -1); return; }
			if (id >= base && id < base + 256) { choose_in(p, id - base); return; }
		}
		if (id == ID_OUT_NONE)                                        choose_out(-1);
		else if (id >= ID_OUT_BASE && id < ID_OUT_BASE + 256)         choose_out(id - ID_OUT_BASE);
		else if (id == ID_OUTMU_NONE)                                 choose_out_mu(-1);
		else if (id >= ID_OUTMU_BASE && id < ID_OUTMU_BASE + 256)     choose_out_mu(id - ID_OUTMU_BASE);
		else if (id == ID_OUTB_NONE)                                  choose_out_b(-1);
		else if (id >= ID_OUTB_BASE && id < ID_OUTB_BASE + 256)       choose_out_b(id - ID_OUTB_BASE);
		else if (id == ID_AIN_NONE)                                   choose_ain(-1);
		else if (id >= ID_AIN_BASE && id < ID_AIN_BASE + 256)         choose_ain(id - ID_AIN_BASE);
		else if (id == ID_CARD_OPEN)                                  do_card_open();
		else if (id == ID_CARD_EJECT)                                 { eject_card(); save_settings(); }
		else if (id >= ID_CARD_NEW16 && id <= ID_CARD_NEW128)         new_card(16u << (id - ID_CARD_NEW16));
		else if (id == ID_PLAY_FILE)                                  do_midi_file();
		else if (id == ID_STOP_FILE)                                  play.stop();
		else if (id == ID_PORTS34_FOLD)                               set_fold34(true);
		else if (id == ID_PORTS34_DROP)                               set_fold34(false);
		else if (id == ID_NATIVE_FX)                                  toggle_fx();
		else if (id == ID_NATIVE_ENGINE)                              toggle_engine();
		else if (id == ID_FACTORY)                                    do_factory_reset();
		else if (id == ID_PC_EDITOR)                                  open_window_by_kind(BAR_EDITOR);
		else if (id == ID_OVERVIEW)                                   open_window_by_kind(BAR_LIST);
		else if (id == ID_OUTPUT_DIGITAL || id == ID_OUTPUT_ANALOG)   set_analog(id == ID_OUTPUT_ANALOG);
	}

	// ---- per-platform acts (thin shells implement these)

	// Something in a menu failed. Windows remembers it for the end of the
	// command; macOS tells the user straight away
	virtual void menu_error(const std::string &text) = 0;
	// Something worth saying that is not a failure (fresh card needs Format)
	virtual void menu_note(const std::string &text) = 0;
	// File dialogs ("" means cancelled)
	virtual std::string ask_card_open_path() = 0;
	virtual std::string ask_card_save_path() = 0;
	virtual std::string ask_midi_file_path() = 0;
	// Factory reset confirmation (false keeps everything)
	virtual bool confirm_factory_reset() = 0;

protected:
	// One MIDI OUT opener for the three (A/B/MU): same calls, different
	// slots and labels. Failures print always and reach menu_error for
	// menu picks (never boot-time, which passes keep)
	bool open_out(midi_out &port, int &dev_slot, std::string &name_slot,
	              std::string &keep_slot, const char *label, int dev, bool keep)
	{
		if (!keep)
			keep_slot.clear();
		std::string err;
		if (!port.open(dev, err)) {
			std::fprintf(stderr, "%s: %s\n", label, err.c_str());
			if (!keep)
				menu_error(err);
			port.open(-1, err);
			dev = -1;
		}
		dev_slot  = port.is_open() ? dev : -1;
		name_slot = port.device_name();
		save_settings();
		return dev >= 0;
	}

	u64 last_flush = 0;                // card file last written back
	u64 last_drop_report = 0;          // MIDI drops last said out loud
	bool pressed = false;            // a panel press is in flight (drag/up)
};

} // namespace ui

#endif // S_MU2000_UI_APP_H
