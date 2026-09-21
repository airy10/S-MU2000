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
#include "ui/pc_window_mac.h"
#include "ui/player.h"
#include "ui/png.h"
#include "ui/shot.h"
#include "ui/app.h"
#include "ui/toolbar.h"
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

	// The PC editor windows. Same contents as on Windows; only the window is
	// AppKit + Metal (ui/pc_window_mac.mm). F2 / F3 or right-click opens them
	ui::pc_window pc{ std::make_unique<ui::pc_editor>() };    // PC editor (F2 or right-click)
	ui::pc_window list{ std::make_unique<ui::overview>() };   // overview (F3 or right-click)
	ui::pc_window fx{ std::make_unique<ui::fx_editor>() };    // insertion settings (double-click in the overview)
	ui::pc_window shapes{ std::make_unique<ui::part_shapes>() };  // part voice (double-click a VIB/FILTER/EG/EQ cell in the overview)
	ui::pc_window master{ std::make_unique<ui::master_editor>() }; // master (double-click the MASTER row in the overview)

	std::string layout_path;

	// ---- mac_app

	void draw(void *cg, int w, int h) override
	{
		// The window's timer is where this has to happen: it touches the bridge,
		// so it must not run on the audio thread (same as gui.cpp's WM_TIMER)
		panel.tick(br);
		// the CPU load for the PC windows (the overview's top strip)
		if (out && out->produced())
			br.set_cpu(float(out->cpu_percent()));
		// いまどちらの口で鳴らしているか（F4 で切り替わる）を一覧の帯へ。
		// Windows 側は gui.cpp の WM_TIMER で同じことを書く
		br.set_engine(eng ? eng->native_engine.load() : -1);
		// the PC editor windows, where the Windows side has its WM_TIMER
		ui::pc_frame_all(list, pc, fx, shapes, master, panel.xg(), panel.ram(), br,
		                 [&](ui::pc_window &w) { open_editor_window(w); });
		card_tick();
		report_drops();

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
		if (lcd_only)
			return false;
		// What the press means is shared (ui::app::hit_test); only acting
		// on it is the window's business
		const mouse_hit h = hit_test(x, y, right);
		if (h.bar_window >= 0) {
			bar.set_down(h.bar_window);
			open_window_by_kind(h.bar_window);
			m_pressed = true;   // mouse_up clears the pressed look
			return true;
		}
		if (h.handled)
			return true;            // the strip's gaps, or a menu spot
		m_pressed = true;
		panel.press(x, y, br);
		return false;
	}

	void mouse_drag(int x, int y) override
	{
		if (lcd_only)
			return;
		if (m_pressed)
			panel.drag(x, y, br);
	}

	void mouse_up() override
	{
		if (!m_pressed)
			return;
		m_pressed = false;
		bar.set_down(-1);
		panel.release(br);
	}

	void wheel(int x, int y, int steps) override
	{
		if (lcd_only)
			return;
		if (steps)
			panel.wheel_at(x, y, steps, br);
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
		m_pressed = false;
		release_keys();
	}

	bool hand_cursor(int x, int y) override
	{
		if (lcd_only)
			return false;
		return panel.on_midi_jack(x, y) || panel.on_ad_input(x, y) ||
		       panel.on_card_slot(x, y) || panel.on_phones(x, y);
	}

	std::vector<ui::menu_group> context_menu(int x, int y) override
	{
		if (lcd_only)
			return {};
		if (panel.on_card_slot(x, y))
			return ui::menu_card(menu_snapshot());
		// The PHONES jack is about the output, as in gui.cpp
		if (panel.on_phones(x, y))
			return ui::menu_phones(eng && eng->analog.load());
		// The A/D INPUT jack offers just its recording devices, as in gui.cpp
		if (panel.on_ad_input(x, y))
			return ui::menu_ain_only(ui::audio_in::list(), ain_name);
		return ui::menu_ports(menu_snapshot());
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

	void reload_layout() override
	{
		apply_layout(layout_path, false);
	}

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
		if (kind == ui::BAR_LIST)         open_editor_window(list);
		else if (kind == ui::BAR_EDITOR)  open_editor_window(pc);
		else if (kind == ui::BAR_SHAPES)  open_editor_window(shapes);
		else if (kind == ui::BAR_FX)      open_editor_window(fx);
		else if (kind == ui::BAR_MASTER)  open_editor_window(master);
	}

	void set_layout(const std::string &path)
	{
		layout_path = path;
		panel.lay() = ui::layout();
		std::string err;
		if (!path.empty() && !panel.lay().load(path, err))
			std::printf("配置: %s を開けない。組み込みの配置を使う\n", path.c_str());
		if (!err.empty())
			std::fprintf(stderr, "%s", err.c_str());
		panel.resize(panel.width(), panel.height());
	}

	void apply_layout(const std::string &path, bool quiet)
	{
		panel.lay() = ui::layout();
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

	// A file dropped on the window is played, which is what gui.cpp's
	// WM_DROPFILES handler does with one. The window only hands the path over:
	// what a drop means is the app's business
	void file_dropped(const std::string &path) override
	{
		play_song(path);
	}

	// A MIDI loop (THRU fed back into an IN) overflows the guards. gui.cpp says
	// so once a second rather than once a block; the same here, from the window's
	// timer rather than from the paint
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

private:
	bool m_pressed = false;
	u64 last_drop_report = 0;          // when the MIDI drops were last said out loud
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

	std::string dir, shot_path, dump_layout, play_path;
	std::string layout_path;
	ui::window_options win_opts;     // --editor/--lcd etc., shared (ui/options.h)
	// MIDI IN A-D. -2 unset (use the remembered one) / -1 unused
	int in_dev[mu2000::MIDI_PORTS] = { -2, -2, -2, -2 };
	// Start as the machine does with HOST SELECT = USB, which is what makes ports
	// C and D usable. --host-midi turns it off (the DIN ports A and B only)
	bool usb_host = true;
	ui::engine_options eng_opts;     // --fast-midi/--native-fx*, shared (ui/options.h)
	int mout_dev = -2;
	int moutb_dev = -2;
	int moutmu_dev = -2;               // the machine's own MIDI OUT
	int latency = 30;
	ui::output_options out_opts;
	int win_w = 1000, win_h = 400;
	bool size_given = false;
	bool grid = false;
	bool boot_for_shot = false;
	bool nomidi = false;               // --nomidi: open and remember no MIDI port
	std::string shot_mid;
	double shot_secs = 0.0;

	for (int i = 1; i < argc; i++) {
		if (!std::strcmp(argv[i], "--list")) {
			const auto ins = ui::midi_in::list();
			std::printf("MIDI 入力（--midi 番号 / 画面からも選べる）:\n");
			for (size_t k = 0; k < ins.size(); k++)
				std::printf("  %zu: %s\n", k, ins[k].c_str());
			if (ins.empty())
				std::printf("  （なし）\n");
			const auto outs = ui::midi_out::list();
			std::printf("MIDI 出力（--midiout 番号 / 受けたものをそのまま外へ）:\n");
			for (size_t k = 0; k < outs.size(); k++)
				std::printf("  %zu: %s\n", k, outs[k].c_str());
			if (outs.empty())
				std::printf("  （なし）\n");
			// Same as gui.cpp: the names --audio takes are matched as substrings
			const auto aouts = ui::audio_out::list();
			std::printf("音声の出口（--audio に名前の一部）:\n");
			for (size_t k = 0; k < aouts.size(); k++)
				std::printf("  %zu: %s\n", k, aouts[k].c_str());
			const auto ains = ui::audio_in::list();
			std::printf("A/D INPUT（録音デバイス。画面から選ぶ）:\n");
			for (size_t k = 0; k < ains.size(); k++)
				std::printf("  %zu: %s\n", k, ains[k].c_str());
			if (ains.empty())
				std::printf("  （なし）\n");
			return 0;
		}
		else if (!std::strcmp(argv[i], "--midi") && i + 1 < argc) in_dev[0] = std::atoi(argv[++i]);
		else if (!std::strcmp(argv[i], "--midi-b") && i + 1 < argc) in_dev[1] = std::atoi(argv[++i]);
		else if (!std::strcmp(argv[i], "--midi-c") && i + 1 < argc) in_dev[2] = std::atoi(argv[++i]);
		else if (!std::strcmp(argv[i], "--midi-d") && i + 1 < argc) in_dev[3] = std::atoi(argv[++i]);
		else if (!std::strcmp(argv[i], "--usb")) usb_host = true;
		else if (!std::strcmp(argv[i], "--host-midi")) usb_host = false;
		else if (!std::strcmp(argv[i], "--midiout") && i + 1 < argc) mout_dev = std::atoi(argv[++i]);
		else if (!std::strcmp(argv[i], "--midiout-b") && i + 1 < argc) moutb_dev = std::atoi(argv[++i]);
		else if (!std::strcmp(argv[i], "--midiout-mu") && i + 1 < argc) moutmu_dev = std::atoi(argv[++i]);
		else if (ui::consume_engine_option(argv[i], eng_opts)) {}
		else if (!std::strcmp(argv[i], "--nomidi")) {
			// Nothing is opened and nothing is remembered: this is for tests,
			// which must leave the real settings file the way they found it.
			// The app itself is made further down, so the flag is carried there
			for (int &d : in_dev) d = -1;
			mout_dev = moutb_dev = moutmu_dev = -1;
			nomidi = true;
		}
		else if (!std::strcmp(argv[i], "--latency") && i + 1 < argc) latency = std::atoi(argv[++i]);
		else if (ui::consume_output_option(argv, argc, i, out_opts)) {}
		else if (ui::consume_window_option(argv[i], win_opts)) {}
		else if (!std::strcmp(argv[i], "--shot") && i + 1 < argc) shot_path = argv[++i];
		else if (!std::strcmp(argv[i], "--boot")) boot_for_shot = true;
		else if (!std::strcmp(argv[i], "--grid")) grid = true;
		else if (!std::strcmp(argv[i], "--layout") && i + 1 < argc) layout_path = argv[++i];
		else if (!std::strcmp(argv[i], "--play") && i + 1 < argc) play_path = argv[++i];
		else if (!std::strcmp(argv[i], "--dump-layout") && i + 1 < argc) dump_layout = argv[++i];
		else if (!std::strcmp(argv[i], "--mid") && i + 2 < argc) {
			shot_mid = argv[++i];
			shot_secs = std::atof(argv[++i]);
			boot_for_shot = true;
		}
		else if (!std::strcmp(argv[i], "--size") && i + 1 < argc) {
			if (std::sscanf(argv[++i], "%dx%d", &win_w, &win_h) != 2) { win_w = 1000; win_h = 400; }
			size_given = true;
		}
		else if (dir.empty()) dir = argv[i];
	}
	if (win_opts.lcd_only && !size_given) {
		win_w = 898;
		win_h = 290;
	}

	// Without --layout, look through the usual places in order
	if (layout_path.empty())
		layout_path = ui::layout::find_default();

	if (!dump_layout.empty()) {
		ui::layout l;
		std::string lerr;
		if (!layout_path.empty())
			l.load(layout_path, lerr);
		if (!l.save(dump_layout)) {
			std::fprintf(stderr, "%s に書けない\n", dump_layout.c_str());
			return 1;
		}
		std::printf("いまの配置を書き出した: %s\n", dump_layout.c_str());
		std::printf("直したら --layout で渡すか、窓で F5 を押す\n");
		return 0;
	}

	static ui::bridge br;
	static ui::midi_in  midi_ports[mu2000::MIDI_PORTS];
	static ui::midi_out mout, mout_b, mout_mu;

	// Picture only. An empty screen can be drawn even without any ROMs.
	if (!shot_path.empty() && (dir.empty() || !boot_for_shot)) {
		ui::snapshot s;
		std::snprintf(s.message, sizeof(s.message), "S-MU2000");
		br.publish(s);
		return ui::write_shot(shot_path, win_w, win_h, br, grid, win_opts.lcd_only, layout_path);
	}

	if (dir.empty()) {
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
	if (std::getenv("SMU2000_VOICECACHE"))
		eng_opts.voicecache = 1;
	ui::apply_engine_options(eng.mu, eng_opts);
	eng.native_fx.store(eng_opts.native_fx);
	for (int p = 1; p < mu2000::MIDI_PORTS; p++)
		eng.midi_p[p] = &midi_ports[p];
	eng.mout_b = &mout_b;
	eng.mout_mu = &mout_mu;
	eng.mout = &mout;
	if (!eng.load(dir)) {
		std::fprintf(stderr, "%s\n", eng.message.c_str());
		return 1;
	}
	// the overview reads voice names and instrument icons from the user's ROM (xg/voices.h)
	ui::xgui::set_voice_rom(eng.mu.program_rom());

	// **USB by default**, the way the machine is set up when it is connected to a
	// computer. The firmware passes ports C and D only when HOST SELECT is USB,
	// and then A and B arrive over USB as well. --host-midi gives the DIN ports A
	// and B only.
	//
	// Decided before any boot: reset() keys the host-present message on this,
	// and the --shot boot below returns early. Same move as gui.cpp.
	eng.mu.set_usb_host(usb_host);
	std::printf(usb_host ? "MIDI は USB の口（A-D の 64 パート）\n"
	                     : "--host-midi: DIN の口 A・B だけ（パート 1-32）\n");

	// Picture only, but taken after boot so the LCD has something on it
	if (!shot_path.empty()) {
		if (!eng.boot()) { std::fprintf(stderr, "%s\n", eng.message.c_str()); return 1; }
		eng.state.store(1);

		// The display is still settling right after boot. Idle a little to calm it.
		{
			s32 l, r;
			for (size_t i = 0; i < size_t(2.0 * RATE); i++)
				eng.mu.run_sample(l, r);
		}

		// The level meters need signal, so stream MIDI first when one was given
		if (!shot_mid.empty()) {
			std::vector<smf::event> evs;
			std::string err;
			if (!smf::load(shot_mid, evs, err)) {
				std::fprintf(stderr, "%s\n", err.c_str());
			} else {
				std::printf("MIDI %zu 件を %.1f 秒ぶん流す\n", evs.size(), shot_secs);
				size_t at = 0;
				s32 l, r;
				for (size_t i = 0; i < size_t(shot_secs * RATE); i++) {
					const double now = double(i) / RATE;
					while (at < evs.size() && evs[at].time <= now) {
						for (u8 b : evs[at].bytes)
							eng.mu.midi_in(b);
						at++;
					}
					eng.mu.run_sample(l, r);
				}
			}
		}

		eng.publish();
		return ui::write_shot(shot_path, win_w, win_h, br, grid, win_opts.lcd_only, layout_path);
	}

	// ---- Put the window up

	static gui_app gui(br, midi_ports, mout, mout_b, mout_mu);
	g_gui = &gui;
	// a MIDI file dropped on any window plays (the panel, the editor, the overview)
	ui::pc_window::set_drop_handler(play_dropped_file);
	gui.keep_settings = nomidi;
	gui.eng = &eng;
	gui.state = &eng.state;
	gui.lcd_only = win_opts.lcd_only;
	gui.panel.set_lcd_only(win_opts.lcd_only);
	if (!win_opts.lcd_only) {
		gui.bar.set_items(ui::window_bar_items());
		gui.panel.set_top_inset(ui::toolbar::HEIGHT);
	}
	gui.panel.resize(win_w, win_h);
	gui.set_layout(layout_path);
	gui.panel.resize(win_w, win_h);

	// Only the window uses the remembered settings: --shot has to give the same
	// picture every time
	eng.use_nvram = !out_opts.factory;
	if (out_opts.factory)
		std::printf("工場出荷状態で起動する（覚えていた設定は終わるときに上書きされる）\n");

	// Look up the previously chosen ports by name. --midi / --midiout win.
	//
	// Opening the ports here rather than on the boot thread keeps the names
	// settled before the window starts reading them for the status line
	// The machine's A/D INPUT. Declared here so the boot thread below can start
	// it; the engine only samples it through the pointer
	static ui::audio_in ain;
	gui.ain = &ain;
	eng.ain = &ain;

	{
		const ui::remembered want = ui::app::load_remembered(settings_file_path());
		br.set_gain(want.volume);
		// set before set_fold34, which writes the settings back through save_settings()
		eng.analog.store(want.analog);
		if (want.analog)
			std::printf("音の出口: アナログ（直流を切る）\n");
		gui.set_fold34(want.fold34);
		// --audio wins; otherwise the port that was opened last time
		gui.audio_name = out_opts.audio_dev ? std::string(out_opts.audio_dev) : want.audio_out;
		// A/D INPUT is remembered by name too. It is opened in the boot thread,
		// once the machine is up
		gui.ain_name = want.audio_in;
		gui.ain_keep = want.audio_in;
		if (!want.card.empty())
			gui.insert_card(want.card, true);
		for (int p = 0; p < mu2000::MIDI_PORTS; p++)
			if (in_dev[p] == -2)
				in_dev[p] = find_device(ui::midi_in::list(), want.in[p]);
		if (mout_dev == -2)  mout_dev   = find_device(ui::midi_out::list(), want.out);
		if (moutb_dev == -2)  moutb_dev  = find_device(ui::midi_out::list(), want.out_b);
		if (moutmu_dev == -2) moutmu_dev = find_device(ui::midi_out::list(), want.out_mu);

		// A port that is not there yet keeps its name in the settings
		for (int p = 0; p < mu2000::MIDI_PORTS; p++)
			gui.in_keep[p] = want.in[p];
		gui.out_keep    = want.out;
		gui.out_keep_b  = want.out_b;
		gui.out_keep_mu = want.out_mu;
		for (int p = 0; p < mu2000::MIDI_PORTS; p++)
			gui.choose_in(p, in_dev[p], true);
		gui.choose_out(mout_dev, true);
		gui.choose_out_b(moutb_dev, true);
		gui.choose_out_mu(moutmu_dev, true);
		// Show the name that was remembered when the port could not be opened,
		// so it is visible that the choice was not lost
		auto show = [](const char *label, const std::string &now, const std::string &keep) {
			if (!now.empty())
				std::printf("%s: %s\n", label, now.c_str());
			else if (!keep.empty())
				std::printf("%s: なし（「%s」が見つからないか開けない。覚えたままにしてある）\n",
				            label, keep.c_str());
			else
				std::printf("%s: なし\n", label);
		};
		for (int p = 0; p < mu2000::MIDI_PORTS; p++)
			show(ui::IN_LABELS[p], gui.in_name[p], gui.in_keep[p]);
		show("MIDI OUT",   gui.out_name_mu, gui.out_keep_mu);
		show("MIDI THRU A", gui.out_name,    gui.out_keep);
		show("MIDI THRU B", gui.out_name_b,  gui.out_keep_b);
		std::fflush(stdout);
	}

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
		if (eng_opts.native_engine) {
			eng.mu.set_native_engine(eng_opts.native_engine);
			if (eng_opts.voicecache)
				smu2000::voicecache::load(eng.mu, smu2000::voicecache::key(eng.mu));
		}
		eng.state.store(1);
		eng.publish();

		std::string err;
		if (!out.start(latency, [](s16 *o, u32 n) { eng.fill(o, n); }, err, out_opts.exclusive,
		               gui.audio_name)) {
			std::fprintf(stderr, "音声: %s\n", err.c_str());
			eng.message = "音声デバイスを開けない";
			eng.state.store(2);
			eng.publish();
			return;
		}
		// Remember the port that was actually opened, by name. **After** the MIDI
		// ports were settled above, or the settings written here would carry an
		// empty MIDI name and the next start would come up with no ports
		gui.audio_name = out.device_name();
		std::printf("音声の出口: %s\n", out.device_name().c_str());
		// A/D INPUT: open the recording device that was picked last time. A
		// device that cannot be opened now keeps its name in the settings, the
		// same as a MIDI port (gui.cpp does this here too)
		if (!gui.ain_name.empty()) {
			const auto names = ui::audio_in::list();
			const int dev = find_device(names, gui.ain_name);
			std::string aerr;
			if (dev >= 0 && ain.start(names[size_t(dev)], aerr)) {
				gui.ain_dev = dev;
				std::printf("A/D INPUT: %s（%s）\n", ain.device_name().c_str(), ain.format_line().c_str());
			} else
				std::printf("A/D INPUT: なし（%s）\n",
				            dev < 0 ? "デバイスが見つからない" : aerr.c_str());
		}
		// Hog mode is a request, not a guarantee: something else may hold it
		if (out_opts.exclusive)
			std::printf("独り占め: %s\n", out.exclusive() ? "取れた" : "取れなかった");
		gui.save_settings();
		// With --play, start streaming as soon as it begins to sound
		if (!play_path.empty())
			gui.play_song(play_path);
		std::printf("鳴らしている（待ち時間 %.1f ms、%s）\n",
		            1000.0 * out.buffer_frames() / RATE,
		            out.mmcss() ? "CoreAudio の実時間スレッド"
		                        : "実時間スレッドを取れていない（途切れやすい）");
		std::fflush(stdout);
	});

	// with --editor and friends, open those windows with the panel (same order as gui.cpp)
	if (win_opts.open_editor && !win_opts.lcd_only)
		gui.open_editor_window(gui.pc);
	if (win_opts.open_fx && !win_opts.lcd_only)
		gui.open_editor_window(gui.fx);
	if (win_opts.open_list && !win_opts.lcd_only)
		gui.open_editor_window(gui.list);
	if (win_opts.open_shapes && !win_opts.lcd_only)
		gui.open_editor_window(gui.shapes);
	if (win_opts.open_master && !win_opts.lcd_only)
		gui.open_editor_window(gui.master);

	ui::run_window(gui, "S-MU2000", win_w, win_h);

	// tell the editor windows we are closing (unmute the overview, restore its
	// receive channels, ...). The audio thread drains what we sent, so pause
	// a moment before stopping it
	ui::pc_shutdown_all(gui.list, gui.pc, gui.fx, gui.shapes, gui.master, br);
	std::this_thread::sleep_for(std::chrono::milliseconds(100));

	out.stop();
	ain.stop();
	// Leaving the THRU ports open with notes still held would leave them stuck
	// on whatever is listening, so all sound off and all notes off go out first
	for (ui::midi_out *thru : { &mout, &mout_b }) {
		if (!thru->is_open())
			continue;
		for (int ch = 0; ch < 16; ch++) {
			for (u8 v : { u8(0xb0 | ch), u8(120), u8(0), u8(0xb0 | ch), u8(123), u8(0) })
				thru->send(v);
		}
	}
	if (boot_thread.joinable())
		boot_thread.join();
	gui.join_reboot();
	gui.flush_card();        // the sound has stopped; keep what was written to the card
	gui.save_settings();          // the audio port, the A/D input and the VOLUME knob's position
	// The sound has stopped by now. Keep the machine's settings only if it came up
	eng.settle_for_save();
	if (eng.state.load() == 1 && !smu2000::nvram::save(eng.mu))
		std::fprintf(stderr, "設定を残せなかった: %s\n", smu2000::nvram::path(eng.mu).c_str());
	// Prepare the snapshot for the settings just saved (src/bootcache.h).
	// Without it, the first boot after changing settings is slow
	// (same as gui.cpp)
	if (eng.state.load() == 1) {
		if (smu2000::bootcache::refresh(eng.mu))
			std::printf("次の起動ぶんの写しを作った\n");
		smu2000::bootcache::prune();
	}
	gui.play.stop();
	for (ui::midi_in &m : midi_ports)
		m.close();
	mout.close();
	mout_b.close();
	mout_mu.close();
	return 0;
}
