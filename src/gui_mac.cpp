// license:BSD-3-Clause
//
// Run the MU2000 behind a front panel that looks like the real machine (macOS).
//
//   gui <rom directory> [--midi n] [--midi-b n] [--midi-c n] [--midi-d n]
//       [--midiout n] [--midiout-b n] [--midiout-mu n]
//       [--latency ms] [--exclusive] [--audio <name>] [--factory] [--host-midi] [--fast-midi]
//   gui --list                             list the MIDI ports and audio devices
//   gui <rom directory> --shot image.png   write the picture without a window
//
// The Windows version of this is gui.cpp, and this is the same program: the
// same panel, the same engine, the same arguments. The differences are the
// ones the platform forces.
//
//   * the window is AppKit (src/ui/window_mac.mm) rather than Win32, which is
//     a separate file because the Cocoa headers and compat/gdi.h cannot both
//     be visible at once
//   * the port picker is an NSMenu and choosing a MIDI file is an NSOpenPanel,
//     so both are asked for through ui::mac_app instead of built here
//   * drawing goes into the view's CGContext through the GDI shim, so panel.cpp
//     is literally the same code that paints the Windows window
//   * settings live in ~/Library/Application Support/S-MU2000/gui.ini
//
// Audio is produced the same way as in live: **it keeps no clock of its own**
// (doc/design.md).
//
// The mouse wheel drives the dial. The real machine has a rotary encoder in
// that spot too, and it does the same job as the VALUE -/+ buttons.

#include "compat/console.h"
#include "compat/gdi.h"
#include "compat/paths.h"
#include "bootcache.h"
#include "mu2000.h"
#include "nvram.h"
#include "smartmedia.h"
#include "smf.h"
#include "voicecache.h"
#include "ui/audio_out.h"
#include "ui/bridge.h"
#include "ui/engine.h"
#include "ui/fx_editor.h"
#include "ui/layout.h"
#include "ui/midi_in.h"
#include "ui/midi_out.h"
#include "ui/overview.h"
#include "ui/panel.h"
#include "ui/master_editor.h"
#include "ui/part_shapes.h"
#include "ui/pc_editor.h"
#include "ui/pc_host.h"
#include "ui/pc_window.h"
#include "ui/player.h"
#include "ui/png.h"
#include "ui/shot.h"
#include "ui/app.h"
#include "ui/toolbar.h"
#include "ui/tool_args.h"
#include "ui/keymap.h"
#include "ui/options.h"
#include "ui/settings.h"
#include "ui/status.h"
#include "ui/window_mac.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

// Menu lines and builders are shared with gui.cpp in ui/menu.h; the names
// below are unqualified for the choice dispatch (menu_chosen).
using namespace ui;

constexpr u32 RATE = ui::AUDIO_RATE;

// Menu command numbers, labels and builders are shared with gui.cpp
// in ui/menu.h (Windows is the reference), so a menu added on one
// side cannot be missed on the other. Only rendering (window_mac.mm)
// and acting on the choice (menu_chosen below) stay here.

// ---- Remember the chosen ports
//
// They are remembered by **name**, not by number. Replugging a USB device
// shifts the numbers, so a remembered number would connect to a different
// device the next time the window is opened.

std::string settings_file_path()
{
	const std::string dir = smu2000::ensure_config_dir();
	return dir.empty() ? std::string() : dir + "gui.ini";
}

// gui.ini keys live in ui/settings.h as ui::SET_* (shared with gui.cpp).
// Menu wording lives in ui/menu.h as ui::IN_LABELS.

// ---- Things handed to the window

// ---- The screen. Paints the panel, feeds it input, builds the menus

class gui_app : public ui::mac_app, public ui::app
{
public:
	// mi is MIDI IN A-D, mu2000::MIDI_PORTS of them
	gui_app(ui::bridge &b, ui::midi_in *mi,
	    ui::midi_out &tha, ui::midi_out &thb, ui::midi_out &muo)
	    : ui::app(b, mi, tha, thb, muo) {}

	// ---- mac_app

	void draw(void *cg, int w, int h) override
	{
		// The window's timer work is shared (ui::app::poll: panel tick,
		// CPU/engine publishing, card flush, drop reports); only driving
		// the PC editor windows stays here (different window types)
		poll();
		// the PC editor windows, where the Windows side has its WM_TIMER
		ui::pc_frame_all(list, pc, fx, shapes, master, panel.xg(), panel.ram(), br,
		                 [&](ui::pc_window &w) { open_editor_window(w); });

		// The view's context is already top-left, y down, so the shared
		// painter (ui::app::paint_into) takes it as it stands.
		// starved() counts what Windows calls late(); output_ms is
		// WASAPI-only, so only the drop count crosses over (ui/status.h)
		char middle[32] = {};
		if (out && out->produced())
			std::snprintf(middle, sizeof(middle), "遅れ %llu",
			              (unsigned long long)out->starved());
		HDC dc = static_cast<HDC>(smu_gdi_wrap_view_context(cg, w, h));
		paint_into(dc, w, middle);
		DeleteDC(dc);
	}

	void resized(int w, int h) override
	{
		panel.resize(w, h);
	}

	bool mouse_down(int x, int y, bool right) override
	{
		// Decided and acted in the base; the window only reports whether
		// a popup follows (its infra then asks context_menu for the items)
		return press_at(x, y, right);
	}

	void mouse_drag(int x, int y) override
	{
		do_mouse_drag(x, y);
	}

	void mouse_up() override
	{
		do_mouse_up();
	}

	void wheel(int x, int y, int steps) override
	{
		do_wheel(x, y, steps);
	}

	void key(int code, bool down) override
	{
		if (lcd_only && down)
			return;
		if (code == ui::MAC_KEY_FUNCTION_BASE + 0x60) {      // F5
			if (down)
				reload_layout();
			return;
		}
		if (down && code == ui::MAC_KEY_FUNCTION_BASE + 0x78) {   // F2
			open_window_by_kind(ui::BAR_EDITOR);
			return;
		}
		if (down && code == ui::MAC_KEY_FUNCTION_BASE + 0x63) {   // F3
			open_window_by_kind(ui::BAR_LIST);
			return;
		}
		if (down && code == ui::MAC_KEY_FUNCTION_BASE + 0x76) {   // F4
			toggle_engine();
			return;
		}
		handle_panel_key(code, down);
	}

	void focus_lost() override
	{
		release_keys();
	}

	bool hand_cursor(int x, int y) override
	{
		if (lcd_only)
			return false;
		return hand_at(x, y);
	}

	std::vector<ui::menu_group> context_menu(int x, int y) override
	{
		if (lcd_only)
			return {};
		return menu_groups_for(x, y);
	}

	// mac_app asks through here; the dispatch is shared (ui::app)
	void menu_chosen(int id) override { ui::app::menu_chosen(id); }

	// ui::app hooks: file dialogs, confirmations and error display are
	// AppKit's business, everything they decide is shared
	std::string settings_path() const override { return settings_file_path(); }
	void menu_error(const std::string &text) override
	{
		ui::alert_modal("S-MU2000", text.c_str());
	}
	void menu_note(const std::string &text) override
	{
		ui::alert_modal("S-MU2000", text.c_str());
	}
	std::string ask_card_open_path() override
	{
		return ui::open_file_panel("差す SmartMedia", "img");
	}
	std::string ask_card_save_path() override
	{
		return ui::save_file_panel("新しい SmartMedia の保存先", "smartmedia.img", "img");
	}
	std::string ask_midi_file_path() override
	{
		return ui::open_midi_file_panel();
	}
	bool confirm_factory_reset() override
	{
		return ui::confirm_modal("S-MU2000",
		                         "MU2000 を工場出荷状態に戻して、電源を入れ直します。\n"
		                         "ユーティリティの設定や、覚えている音量・音色の設定はすべて消えます。",
		                         "戻す");
	}

	void reload_layout() override { ui::app::reload_layout(); }

	// ---- the rest

	// An editor window comes up, or says why it could not. Windows' open_window
	// with its MessageBoxW, in AppKit clothing
	void open_editor_window(ui::pc_window &w)
	{
		std::string err;
		if (!w.show(err))
			ui::alert_modal("S-MU2000", ("開けない: " + err).c_str());
	}

	void open_window_by_kind(int kind) override
	{
		open_editor_window(*ui::window_for_kind(kind, list, pc, fx, shapes, master));
	}

	void print_audio_details() override
	{
		std::printf("独り占め: %s\n", out->exclusive() ? "取れた" : "取れなかった");
	}

	// A file dropped on the window is played, which is what gui.cpp's
	// WM_DROPFILES handler does with one. The window only hands the path over:
	// what a drop means is the app's business
	void file_dropped(const std::string &path) override
	{
		play_song(path);
	}

private:
};


// --shot renders through the shared painter (ui/shot.h)

} // namespace


// A MIDI file dropped on **any** window -- the panel's, or one of the editor
// windows' -- is played. Windows' play_dropped_file (gui.cpp), in UTF-8
gui_app *g_gui = nullptr;                  // set once main has made the app

void play_dropped_file(const std::string &path)
{
	if (g_gui)
		g_gui->play_song(path);
}

int main(int argc, char **argv)
{
	smu2000::init_console_utf8();

	ui::tool_args a;
	a.latency = 30;
	ui::window_options win_opts;     // --editor/--lcd etc., shared (ui/options.h)
	ui::engine_options eng_opts;     // --fast-midi/--native-fx*, shared (ui/options.h)
	ui::output_options out_opts;

	// The flags are shared (ui/tool_args.h); only a.latency above stays per side
	const int parsed = ui::parse_tool_args(argc, argv, a, eng_opts, out_opts, win_opts);
	if (parsed >= 0)
		return parsed;

	static ui::bridge br;
	static ui::midi_in  midi_ports[mu2000::MIDI_PORTS];
	static ui::midi_out mout, mout_b, mout_mu;
	static gui_app gui(br, midi_ports, mout, mout_b, mout_mu);

	// Picture only. An empty screen can be drawn even without any ROMs.
	if (!a.shot_path.empty() && (a.dir.empty() || !a.boot_for_shot)) {
		ui::snapshot s;
		std::snprintf(s.message, sizeof(s.message), "S-MU2000");
		br.publish(s);
		return ui::write_shot(a.shot_path, a.win_w, a.win_h, br, a.grid, win_opts.lcd_only, a.layout_path);
	}

	if (a.dir.empty()) {
		std::fprintf(stderr,
			"使い方: gui <rom ディレクトリ> [--midi 番号] [--midi-b 番号] [--midi-c 番号] [--midi-d 番号]"
			" [--midiout 番号] [--midiout-b 番号] [--midiout-mu 番号]"
			" [--latency ミリ秒] [--exclusive] [--layout panel.txt] [--play 曲.mid] [--host-midi] [--fast-midi] [--lcd]\n"
			"        [--factory]   覚えている設定を捨てて工場出荷状態で起動する\n"
			"        [--editor]    PC エディタも開く（窓では F2 か右クリック）\n"
			"        [--list-window] 一覧の窓も開く（窓では F3 か右クリック）\n"
			"        [--fx-window] インサーションの設定の窓も開く（一覧でインサーションの欄をダブルクリック）\n"
			"        [--shapes-window] パートの音色の窓も開く（一覧で VIB などの絵をダブルクリック）\n"
			"        [--master-window] マスターの窓も開く（一覧でマスターの行をダブルクリック）\n"
			"        gui --dump-layout panel.txt   いまの配置を書き出す\n"
			"        gui --list\n"
			"        gui [<rom ディレクトリ> --boot] --shot 絵.png [--size 1000x400]\n");
		return 1;
	}

	static ui::engine eng(br, midi_ports[0]);
	gui.wire_engine(eng, eng_opts);
	gui.eng = &eng;
	gui.state = &eng.state;
	if (!gui.load_machine(eng, a))
		return 1;
	const int shot = gui.run_boot_shot(eng, br, a, win_opts);
	if (shot >= 0)
		return shot;

	// ---- Put the window up

	g_gui = &gui;
	// a MIDI file dropped on any window plays (the panel, the editor, the overview)
	ui::pc_window::set_drop_handler(play_dropped_file);
	gui.keep_settings = a.nomidi;
	gui.setup_for_window(a, win_opts, out_opts.factory);
	gui.open_remembered_ports(a, out_opts);

	// Give the panel something to read before the boot thread says anything, so
	// the window comes up showing the boot message rather than a blank LCD
	eng.publish();

	// Boot on a separate thread, and start the audio once it is done
	static ui::audio_out out;
	gui.out = &out;
	std::thread boot_thread([&] {
		if (!eng.boot()) {
			eng.state.store(2);
			eng.publish();
			return;
		}
		// After boot, as in gui.cpp: starting needs the firmware
		gui.apply_native_engine(eng, eng_opts);
		eng.state.store(1);
		eng.publish();

		if (!gui.start_audio(a.latency, out_opts.exclusive))
			return;
		std::printf("音声の出口: %s\n", out.device_name().c_str());
		// A/D INPUT: open the recording device that was picked last time
		gui.start_ad();
		// Hog mode is a request, not a guarantee: something else may hold it
		if (out_opts.exclusive)
			std::printf("独り占め: %s\n", out.exclusive() ? "取れた" : "取れなかった");
		// With --play, start streaming as soon as it begins to sound
		if (!a.play_path.empty())
			gui.play_song(a.play_path);
		std::printf("鳴らしている（待ち時間 %.1f ms、%s）\n",
		            1000.0 * out.buffer_frames() / RATE,
		            out.mmcss() ? "CoreAudio の実時間スレッド"
		                        : "実時間スレッドを取れていない（途切れやすい）");
		std::fflush(stdout);
	});

	// with --editor and friends, open those windows with the panel (same order as gui.cpp)
	gui.open_startup_windows(win_opts);

	ui::run_window(gui, "S-MU2000", a.win_w, a.win_h);

	if (boot_thread.joinable())
		boot_thread.join();
	gui.shutdown();
	gui.print_exit_stats(out.starved());
	return 0;
}
