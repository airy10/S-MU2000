// license:BSD-3-Clause
//
// 実機のフロントパネル風の画面で MU2000 を動かす。
//
//   gui <rom ディレクトリ> [--midi 番号] [--midi-b 番号] [--fast-midi]
//       [--midiout 番号] [--midiout-b 番号] [--midiout-mu 番号] [--latency ミリ秒]
//   gui --list                             MIDI の入口と出口の一覧
//   gui <rom ディレクトリ> --shot 絵.png    窓を出さずに絵だけ書き出す（見た目の確認用）
//
// 入口と出口は**動かしたまま画面から選べる**。パネルの MIDI IN A の
// ジャックを押すか、どこでも右クリックすると品書きが出る。選んだものは
// %LOCALAPPDATA%\S-MU2000\gui.ini に覚えておいて、次から使う。
//
// MU2000 の設定（ワーク RAM）は終わるときに残し、次の起動で使う（src/nvram.h）。
// --factory か右クリックの「工場出荷状態に戻す」で捨てられる。
//
// 音の作り方は live.exe と同じ。**時計を自分で持たない**（doc/design.md）。
// 画面は別スレッドで、音源とは ui::bridge 越しにしか触れ合わない。
//
// マウスホイールはダイヤルに割り当ててある。実機にもロータリー
// エンコーダがあり、VALUE -/+ のボタンと同じ働きをする。

#include "mu2000.h"
#include "bootcache.h"
#include "nvram.h"
#include "voicecache.h"
#include "smf.h"
#include "ui/audio_out.h"
#include "ui/audio_in.h"
#include "ui/app.h"
#include "ui/bridge.h"
#include "ui/driver.h"
#include "ui/engine.h"
#include "ui/midi_in.h"
#include "ui/midi_guard.h"
#include "ui/midi_out.h"
#include "ui/layout.h"
#include "ui/panel.h"
#include "ui/fx_editor.h"
#include "ui/overview.h"
#include "ui/master_editor.h"
#include "ui/menu.h"
#include "ui/menu_win.h"
#include "ui/keymap.h"
#include "ui/keymap_win.h"
#include "ui/options.h"
#include "ui/settings.h"
#include "ui/status.h"
#include "ui/part_shapes.h"
#include "ui/toolbar.h"
#include "ui/window_win.h"
#include "ui/pc_editor.h"
#include "ui/pc_host.h"
#include "ui/pc_window.h"
#include "ui/player.h"
#include "ui/text.h"
#include "ui/png.h"
#include "ui/shot.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

#include <windows.h>
#include <windowsx.h>
#include <commdlg.h>
#include <shellapi.h>

namespace {

constexpr u32 RATE = ui::AUDIO_RATE;

// ---- 音源側
//
// The engine itself now lives in ui/engine.h: the macOS front end needs the
// same boot sequence and, more to the point, the same MIDI routing, and one
// copy is the only way to keep the two from drifting. Only the name is pulled
// in here, so the rest of this file reads as it always did.

using ui::engine;

// Menu lines and builders are shared with gui_mac.cpp in ui/menu.h; the
// names below are unqualified for the choice dispatch (WM_COMMAND).
using namespace ui;


// ---- 窓

// gui.ini lives under %LOCALAPPDATA% (defined below, before main)
std::string settings_file_path();

// The Windows front end: shared ui::app state and logic plus the Win32
// window (double buffering, message translation). g_win is a pointer
// because ui::app needs its bridge and MIDI ports at construction, which
// main() owns (same as the Mac side's g_gui).
class win_app : public ui::app
{
public:
	win_app(ui::bridge &b, ui::midi_in *mi,
	        ui::midi_out &tha, ui::midi_out &thb, ui::midi_out &muo)
	    : ui::app(b, mi, tha, thb, muo) {}

	HWND hwnd = nullptr;             // set at WM_CREATE, for message boxes
	ui::pc_window pc{ std::make_unique<ui::pc_editor>() };
	ui::pc_window list{ std::make_unique<ui::overview>() };
	ui::pc_window fx{ std::make_unique<ui::fx_editor>() };
	ui::pc_window shapes{ std::make_unique<ui::part_shapes>() };
	ui::pc_window master{ std::make_unique<ui::master_editor>() };
	// Double buffering: repainting straight into the window would flicker
	HDC     mem_dc = nullptr;
	HBITMAP mem_bmp = nullptr;
	int     mem_w = 0, mem_h = 0;
	// Choosing from a menu failed: shown at the end of the command
	std::string last_error;

	void open_window_by_kind(int kind) override;

	// ui::app hooks: file dialogs, confirmations and error display are
	// Win32's business (ui/window_win.h), everything they decide is shared
	std::string settings_path() const override { return settings_file_path(); }
	void menu_error(const std::string &text) override { last_error = text; }
	void menu_note(const std::string &text) override
	{
		ui::win_note(hwnd, text);
	}
	std::string ask_card_open_path() override
	{
		return ui::win_open_file(hwnd, L"差す SmartMedia",
		                         L"SmartMedia の中身 (*.img)\0*.img\0すべて (*.*)\0*.*\0", L"img");
	}
	std::string ask_card_save_path() override
	{
		return ui::win_save_file(hwnd, L"新しい SmartMedia の保存先",
		                         L"SmartMedia の中身 (*.img)\0*.img\0すべて (*.*)\0*.*\0",
		                         L"img", L"smartmedia.img");
	}
	std::string ask_midi_file_path() override
	{
		return ui::win_open_file(hwnd, L"流す MIDI ファイル",
		                         L"MIDI ファイル (*.mid;*.midi)\0*.mid;*.midi\0すべて (*.*)\0*.*\0",
		                         nullptr);
	}
	bool confirm_factory_reset() override
	{
		return ui::win_confirm(hwnd,
		                       "MU2000 を工場出荷状態に戻して、電源を入れ直します。\n"
		                       "ユーティリティの設定や、覚えている音量・音色の設定はすべて消えます。");
	}
};

win_app *g_win = nullptr;

// ---- パネルの配置。作り直さずに文字ファイルで直せる（doc/panel-editing.md）

void apply_layout(const std::string &path, bool quiet)
{
	g_win->panel.lay() = ui::layout();          // まず既定値に戻す
	std::string err;
	if (!path.empty() && g_win->panel.lay().load(path, err)) {
		if (!quiet)
			std::printf("配置: %s\n", path.c_str());
	} else if (!path.empty() && !quiet) {
		std::printf("配置: %s を開けない。組み込みの配置を使う\n", path.c_str());
	}
	if (!err.empty())
		std::fprintf(stderr, "%s", err.c_str());
	std::fflush(stdout);
	g_win->panel.resize(g_win->panel.width(), g_win->panel.height());
}

// ---- 選んだ口を覚えておく
//
// 番号ではなく**名前**で覚える。USB の機器を挿し直すと番号がずれるので、
// 番号で覚えると次に開いたとき別の機器に繋がってしまう。

std::string settings_file_path()
{
	const char *base = std::getenv("LOCALAPPDATA");
	if (!base || !*base)
		return {};
	std::string dir = std::string(base) + "\\S-MU2000";
	CreateDirectoryA(dir.c_str(), nullptr);
	return dir + "\\gui.ini";
}

// ---- 口を選ぶ品書き

// Menu command numbers, labels and builders are shared with gui_mac.cpp
// in ui/menu.h (Windows is the reference), so a menu added on one side
// cannot be missed on the other. Which popup a point asks for is shared
// too (ui::app::menu_groups_for); only rendering it through ui/menu_win.h
// stays here.

void track_menu_at(HWND hwnd, int mx, int my)
{
	POINT pt{ mx, my };
	ClientToScreen(hwnd, &pt);
	ui::win_track_menu(hwnd, pt, g_win->menu_groups_for(mx, my));
}

void play_dropped_file(const std::wstring &path)
{
	// Outside a menu command, so a failure shows straight away rather than
	// through last_error at the end of WM_COMMAND
	if (!g_win->play_song(ui::to_utf8(path.c_str())) && !g_win->last_error.empty()) {
		ui::win_error(GetForegroundWindow(), g_win->last_error);
		g_win->last_error.clear();
	}
}

void ensure_backing(HDC dc, int w, int h)
{
	if (g_win->mem_dc && g_win->mem_w == w && g_win->mem_h == h)
		return;
	if (g_win->mem_bmp) DeleteObject(g_win->mem_bmp);
	if (g_win->mem_dc)  DeleteDC(g_win->mem_dc);
	g_win->mem_dc = CreateCompatibleDC(dc);
	g_win->mem_bmp = CreateCompatibleBitmap(dc, w, h);
	SelectObject(g_win->mem_dc, g_win->mem_bmp);
	g_win->mem_w = w;
	g_win->mem_h = h;
}

void win_app::open_window_by_kind(int kind)
{
	ui::pc_window *w = nullptr;
	if (kind == ui::BAR_LIST)         w = &list;
	else if (kind == ui::BAR_EDITOR)  w = &pc;
	else if (kind == ui::BAR_SHAPES)  w = &shapes;
	else if (kind == ui::BAR_FX)      w = &fx;
	else if (kind == ui::BAR_MASTER)  w = &master;
	if (w)
		ui::win_open_window(hwnd, *w);
}

LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	switch (msg) {
	case WM_CREATE:
		g_win->hwnd = hwnd;
		SetTimer(hwnd, 1, 33, nullptr);        // 30 コマ／秒で描き直す
		return 0;

	case WM_TIMER: {
		// The window's timer work is shared (ui::app::poll); only driving
		// the PC editor windows stays here (different window types)
		g_win->poll();
		ui::pc_frame_all(g_win->list, g_win->pc, g_win->fx, g_win->shapes, g_win->master,
		                 g_win->panel.xg(), g_win->panel.ram(), g_win->br,
		                 [&](ui::pc_window &w) { ui::win_open_window(hwnd, w); });
		InvalidateRect(hwnd, nullptr, FALSE);
		return 0;
	}

	case WM_DROPFILES: {
		// 窓に落とされたファイルの 1 つ目を流す
		const HDROP drop = HDROP(wp);
		wchar_t path[MAX_PATH * 4] = {};
		const bool got = DragQueryFileW(drop, 0, path, UINT(sizeof(path) / sizeof(path[0]))) > 0;
		DragFinish(drop);
		if (got && !g_win->play_song(ui::to_utf8(path)) && !g_win->last_error.empty()) {
			const std::wstring w = ui::to_wide(g_win->last_error);
			MessageBoxW(hwnd, w.c_str(), L"S-MU2000", MB_OK | MB_ICONWARNING);
			g_win->last_error.clear();
		}
		return 0;
	}

	case WM_SIZE:
		g_win->panel.resize(LOWORD(lp), HIWORD(lp));
		InvalidateRect(hwnd, nullptr, FALSE);
		return 0;

	case WM_ERASEBKGND:
		return 1;                               // 全部自分で描く

	case WM_PAINT: {
		PAINTSTRUCT ps;
		HDC dc = BeginPaint(hwnd, &ps);
		RECT cr;
		GetClientRect(hwnd, &cr);
		const int w = cr.right, h = cr.bottom;
		ensure_backing(dc, w, h);

		// The wait/drop fragment is what WASAPI measures (ui/status.h)
		char middle[64] = {};
		if (g_win->out && g_win->out->produced())
			std::snprintf(middle, sizeof(middle), "待ち %.0f ms  遅れ %llu",
			              g_win->out->output_ms(),
			              (unsigned long long)g_win->out->late());
		g_win->paint_into(g_win->mem_dc, w, middle);

		BitBlt(dc, 0, 0, w, h, g_win->mem_dc, 0, 0, SRCCOPY);
		EndPaint(hwnd, &ps);
		return 0;
	}

	case WM_LBUTTONDOWN: {
		if (g_win->lcd_only)
			return 0;
		const int mx = GET_X_LPARAM(lp), my = GET_Y_LPARAM(lp);
		// Decided and mostly acted in the base; the window only shows the
		// popup and repaints
		const ui::app::mouse_out o = g_win->do_mouse_down(mx, my, false);
		if (o.panel_pressed)
			SetCapture(hwnd);
		if (o.opened_window || o.panel_pressed)
			InvalidateRect(hwnd, nullptr, FALSE);
		if (o.opened_window || !o.show_menu)
			return 0;
		track_menu_at(hwnd, mx, my);
		return 0;
	}

	case WM_RBUTTONUP: {
		if (g_win->lcd_only)
			return 0;
		const int mx = GET_X_LPARAM(lp), my = GET_Y_LPARAM(lp);
		track_menu_at(hwnd, mx, my);
		return 0;
	}

	case WM_COMMAND: {
		const UINT id = LOWORD(wp);
		g_win->last_error.clear();
		// The dispatch is shared (ui::app::menu_chosen); failures land in
		// last_error through menu_error and show below
		g_win->menu_chosen(int(id));
		if (!g_win->last_error.empty()) {
			const std::wstring w = ui::to_wide(g_win->last_error);
			MessageBoxW(hwnd, w.c_str(), L"S-MU2000", MB_OK | MB_ICONWARNING);
			g_win->last_error.clear();
		}
		InvalidateRect(hwnd, nullptr, FALSE);
		return 0;
	}

	case WM_SETCURSOR: {
		// Show a hand where something opens (same spots as the Mac side)
		POINT pt;
		GetCursorPos(&pt);
		ScreenToClient(hwnd, &pt);
		if (LOWORD(lp) == HTCLIENT && g_win->hand_at(pt.x, pt.y)) {
			SetCursor(LoadCursor(nullptr, IDC_HAND));
			return TRUE;
		}
		break;
	}

	case WM_MOUSEMOVE:
		if (g_win->lcd_only)
			return 0;
		if (g_win->do_mouse_drag(GET_X_LPARAM(lp), GET_Y_LPARAM(lp)))
			InvalidateRect(hwnd, nullptr, FALSE);
		return 0;

	case WM_LBUTTONUP:
		if (g_win->lcd_only)
			return 0;
		g_win->do_mouse_up();
		ReleaseCapture();
		InvalidateRect(hwnd, nullptr, FALSE);
		return 0;

	case WM_MOUSEWHEEL: {
		if (g_win->lcd_only)
			return 0;
		POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
		ScreenToClient(hwnd, &pt);
		const int delta = GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA;
		if (g_win->do_wheel(pt.x, pt.y, delta))
			InvalidateRect(hwnd, nullptr, FALSE);
		return 0;
	}

	case WM_KEYDOWN: {
		if (g_win->lcd_only)
			return 0;
		if (lp & (1 << 30))                     // 押しっぱなしの繰り返しは無視
			return 0;
		if (wp == VK_F2) {                      // PC エディタ
			g_win->open_window_by_kind(ui::BAR_EDITOR);
			return 0;
		}
		if (wp == VK_F3) {                      // 一覧
			g_win->open_window_by_kind(ui::BAR_LIST);
			return 0;
		}
		if (wp == VK_F4) {                      // firmware を走らせない口の入切
			g_win->toggle_engine();
			return 0;
		}
		if (wp == VK_F5) {                      // 配置を読み直す
			apply_layout(g_win->layout_path, false);
			InvalidateRect(hwnd, nullptr, FALSE);
			return 0;
		}
		g_win->handle_panel_key(ui::key_char_of_vk(int(wp)), true);
		return 0;
	}

	case WM_KEYUP: {
		g_win->handle_panel_key(ui::key_char_of_vk(int(wp)), false);
		return 0;
	}

	case WM_KILLFOCUS:
		g_win->release_keys();                  // 窓から離れたら全部離す
		return 0;

	case WM_DESTROY:
		PostQuitMessage(0);
		return 0;
	}
	return DefWindowProcA(hwnd, msg, wp, lp);
}


// --shot renders through the shared painter (ui/shot.h), like the window

} // namespace


int main(int argc, char **argv)
{
	SetConsoleOutputCP(CP_UTF8);

	std::string dir, shot_path;
	// -2 未指定（覚えているものを使う）/ -1 使わない
	int in_dev[mu2000::MIDI_PORTS] = { -2, -2, -2, -2 };
	bool usb_host = true;              // USB の口で起動する（口 C・D が使える）。--host-midi で切る
	int moutb_dev = -2;                // MIDI OUT B
	int mout_dev = -2;
	int moutmu_dev = -2;               // MIDI OUT（本体）
	int latency = 20;        // 溜める目標
	ui::output_options out_opts;
	ui::window_options win_opts;
	int win_w = 1000, win_h = 400;   // パネルの論理寸法（1000 × 400）と同じ比
	bool size_given = false;
	ui::engine_options eng_opts;
	bool grid = false;
	std::string layout_path, dump_layout, play_path;
	bool boot_for_shot = false;
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
			const auto aouts = ui::audio_out::list();
			std::printf("音声の出口（--audio に名前の一部）:\n");
			for (size_t k = 0; k < aouts.size(); k++)
				std::printf("  %zu: %s\n", k, aouts[k].c_str());
			return 0;
		}
		else if (!std::strcmp(argv[i], "--midi") && i + 1 < argc) in_dev[0] = std::atoi(argv[++i]);
		else if (!std::strcmp(argv[i], "--midi-b") && i + 1 < argc) in_dev[1] = std::atoi(argv[++i]);
		else if (!std::strcmp(argv[i], "--midi-c") && i + 1 < argc) in_dev[2] = std::atoi(argv[++i]);
		else if (!std::strcmp(argv[i], "--midi-d") && i + 1 < argc) in_dev[3] = std::atoi(argv[++i]);
		else if (!std::strcmp(argv[i], "--midiout") && i + 1 < argc) mout_dev = std::atoi(argv[++i]);
		else if (!std::strcmp(argv[i], "--midiout-b") && i + 1 < argc) moutb_dev = std::atoi(argv[++i]);
		else if (!std::strcmp(argv[i], "--midiout-mu") && i + 1 < argc) moutmu_dev = std::atoi(argv[++i]);
		// 入口も出口も開かない。試しに動かすとき、覚えている THRU の先（実機）へ
		// 流れないように。覚えている口は書き換えない
		else if (!std::strcmp(argv[i], "--nomidi")) {
			for (int &d : in_dev) d = -1;
			mout_dev = moutb_dev = moutmu_dev = -1;
			g_win->keep_settings = true;
		}
		else if (!std::strcmp(argv[i], "--latency") && i + 1 < argc) latency = std::atoi(argv[++i]);
		else if (ui::consume_output_option(argv, argc, i, out_opts)) {}
		else if (ui::consume_window_option(argv[i], win_opts)) {}
		else if (ui::consume_engine_option(argv[i], eng_opts)) {}
		else if (!std::strcmp(argv[i], "--usb")) usb_host = true;
		else if (!std::strcmp(argv[i], "--host-midi")) usb_host = false;
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

	// --layout が無ければ、決まった場所を順に探す
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
	static win_app app(br, midi_ports, mout, mout_b, mout_mu);
	g_win = &app;

	// 絵だけ欲しい場合。ROM が無くても中身が空の画面は出せる
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
			" [--latency ミリ秒] [--exclusive] [--layout panel.txt] [--play 曲.mid] [--lcd] [--fast-midi] [--host-midi]\n"
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

	static engine eng(br, midi_ports[0]);
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
	// 一覧の窓で、音色の名前と楽器の絵を利用者の ROM から読む（xg/voices.h）
	ui::xgui::set_voice_rom(eng.mu.program_rom());

	// **既定は USB の口**（実機を PC に繋ぐときと同じ姿）。口 C・D は実機では
	// USB だけの口で、firmware は HOST SELECT が USB のときしか通さない。
	// USB のときは A・B も USB 側を通る（実機で DIN が黙るのと同じ）。
	// --host-midi を付けると DIN の口 A・B だけになる。
	//
	// **起動より前に決めること**。reset() が「ホストが居る」の知らせ
	// （F4 03 01 01 01）を積むかどうかはここで決まる。--shot は下で先に
	// 起動して return するので、この行が後ろにあると絵だけ DIN の姿で
	// 撮れてしまっていた
	eng.mu.set_usb_host(usb_host);
	std::printf(usb_host ? "MIDI は USB の口（A-D の 64 パート）\n"
	                     : "--host-midi: DIN の口 A・B だけ（パート 1-32）\n");

	// 絵だけ、ただし起動後の LCD が欲しい場合
	if (!shot_path.empty()) {
		if (!eng.boot()) { std::fprintf(stderr, "%s\n", eng.message.c_str()); return 1; }
		eng.state.store(1);

		// 起動直後は表示が動いている途中。少し空回しして落ち着かせる
		{
			s32 l, r;
			for (size_t i = 0; i < size_t(2.0 * RATE); i++)
				eng.mu.run_sample(l, r);
		}

		// レベルメータを出したいので、指定があれば MIDI を流しておく
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

	// ---- 窓を出す

	const HINSTANCE inst = GetModuleHandleA(nullptr);
	WNDCLASSA wc{};
	wc.lpfnWndProc   = wnd_proc;
	wc.hInstance     = inst;
	wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
	wc.lpszClassName = "SMU2000Panel";
	wc.hbrBackground = nullptr;
	RegisterClassA(&wc);

	RECT want{ 0, 0, win_w, win_h };
	AdjustWindowRect(&want, WS_OVERLAPPEDWINDOW, FALSE);
	HWND hwnd = CreateWindowA("SMU2000Panel", "S-MU2000", WS_OVERLAPPEDWINDOW,
	                          CW_USEDEFAULT, CW_USEDEFAULT,
	                          want.right - want.left, want.bottom - want.top,
	                          nullptr, nullptr, inst, nullptr);
	if (!hwnd) {
		std::fprintf(stderr, "窓を出せない\n");
		return 1;
	}

	// MIDI ファイルを窓に落とせば流す（本体の窓も、エディタや一覧の窓も）
	DragAcceptFiles(hwnd, TRUE);
	ui::pc_window::set_drop_handler(play_dropped_file);

	g_win->eng  = &eng;
	g_win->lcd_only = win_opts.lcd_only;
	g_win->panel.set_lcd_only(win_opts.lcd_only);
	// 帯は普通の窓だけ。LCD だけの窓には出さない
	if (!win_opts.lcd_only) {
		g_win->bar.set_items(ui::window_bar_items());
		g_win->panel.set_top_inset(ui::toolbar::HEIGHT);
	}
	// 窓を出すときだけ、覚えている設定で起動する（--shot は毎回同じ絵にしたい）
	eng.use_nvram = !out_opts.factory;
	if (out_opts.factory)
		std::printf("工場出荷状態で起動する（覚えていた設定は終わるときに上書きされる）\n");
	g_win->layout_path = layout_path;
	g_win->panel.resize(win_w, win_h);
	apply_layout(layout_path, false);
	g_win->panel.resize(win_w, win_h);
	{
		// VOLUME のつまみは前に閉じたときの位置から
		ui::remembered r = ui::app::load_remembered(settings_file_path());
		br.set_gain(r.volume);
		eng.analog.store(r.analog);
		if (r.analog)
			std::printf("音の出口: アナログ（直流を切る）\n");
		g_win->play.set_fold_extra_ports(r.fold34);
	}

	eng.publish();
	ShowWindow(hwnd, SW_SHOW);
	if (win_opts.open_editor && !win_opts.lcd_only)
		ui::win_open_window(hwnd, g_win->pc);
	if (win_opts.open_fx && !win_opts.lcd_only)
		ui::win_open_window(hwnd, g_win->fx);
	if (win_opts.open_list && !win_opts.lcd_only)
		ui::win_open_window(hwnd, g_win->list);
	if (win_opts.open_shapes && !win_opts.lcd_only)
		ui::win_open_window(hwnd, g_win->shapes);
	if (win_opts.open_master && !win_opts.lcd_only)
		ui::win_open_window(hwnd, g_win->master);
	UpdateWindow(hwnd);

	// 起動は別スレッド。終わったら音を出し始める
	static ui::audio_out out;
	g_win->out = &out;
	static ui::audio_in ain;
	g_win->ain = &ain;
	eng.ain = &ain;
	std::thread boot_thread([&] {
		if (!eng.boot()) {
			eng.state.store(2);
			eng.publish();
			return;
		}
		// 起動が終わってから入れる（起動には firmware が要る）
		if (eng_opts.native_engine) {
			eng.mu.set_native_engine(eng_opts.native_engine);
			eng.native_engine.store(eng_opts.native_engine);
			if (eng_opts.voicecache)
				smu2000::voicecache::load(eng.mu, smu2000::voicecache::key(eng.mu));
		}
		eng.state.store(1);
		eng.publish();

		// 前に選んだ口を名前で探す。--midi / --midiout があればそちらが勝つ
		ui::remembered want = ui::app::load_remembered(settings_file_path());
		g_win->ain_name = want.audio_in;
		// 前に差していた SmartMedia。ファイルが無くなっていたら差さない（覚えている名前も消える）
		if (!want.card.empty())
			g_win->insert_card(want.card, true);
		// --audio があればそちらが勝つ。無ければ前に選んだもの
		g_win->audio_name = out_opts.audio_dev ? std::string(out_opts.audio_dev) : want.audio_out;
		for (int p = 0; p < mu2000::MIDI_PORTS; p++)
			if (in_dev[p] == -2)
				in_dev[p] = find_device(ui::midi_in::list(), want.in[p]);
		if (mout_dev == -2)
			mout_dev = find_device(ui::midi_out::list(), want.out);
		if (moutb_dev == -2)
			moutb_dev = find_device(ui::midi_out::list(), want.out_b);
		if (moutmu_dev == -2)
			moutmu_dev = find_device(ui::midi_out::list(), want.out_mu);

		for (int p = 0; p < mu2000::MIDI_PORTS; p++)
			g_win->in_keep[p] = want.in[p];
		g_win->out_keep    = want.out;
		g_win->out_keep_b  = want.out_b;
		g_win->out_keep_mu = want.out_mu;
		for (int p = 0; p < mu2000::MIDI_PORTS; p++)
			g_win->choose_in(p, in_dev[p], true);
		g_win->choose_out(mout_dev, true);
		g_win->choose_out_b(moutb_dev, true);
		g_win->choose_out_mu(moutmu_dev, true);
		// 開けなかった口は、覚えていた名前も出す（選び直すまで覚えている）
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
			show(ui::IN_LABELS[p], g_win->in_name[p], g_win->in_keep[p]);
		show("MIDI OUT",    g_win->out_name_mu, g_win->out_keep_mu);
		show("MIDI THRU A", g_win->out_name,    g_win->out_keep);
		show("MIDI THRU B", g_win->out_name_b,  g_win->out_keep_b);
		std::fflush(stdout);

		std::string err;

		if (!out.start(latency, [](s16 *o, u32 n) { eng.fill(o, n); }, err, out_opts.exclusive,
		               g_win->audio_name)) {
			std::fprintf(stderr, "音声: %s\n", err.c_str());
			eng.message = "音声デバイスを開けない";
			eng.state.store(2);
			eng.publish();
			return;
		}
		// 開けた出口を覚える。**設定を読んで MIDI の口を開いた後でないと
		// いけない**。前はこれを起動直後にやっていて、まだ空の MIDI の名前で
		// gui.ini を上書きしていた（毎回 MIDI が「なし」に戻っていた）
		g_win->audio_name = out.device_name();
		std::printf("音声の出口: %s\n%s\n", out.device_name().c_str(),
		            out.format_line().c_str());
		// A/D INPUT。前に選んだ録音デバイスがあれば開く（開けなくても名前は覚えておく）
		if (!g_win->ain_name.empty()) {
			std::string aerr;
			if (ain.start(g_win->ain_name, aerr))
				std::printf("A/D INPUT: %s（%s）\n", ain.device_name().c_str(), ain.format_line().c_str());
			else
				std::printf("A/D INPUT: なし（%s）\n", aerr.c_str());
		}
		g_win->save_settings();
		// --play が付いていれば、鳴り始めたところで流し出す
		if (!play_path.empty()) {
			std::string perr;
			if (!g_win->play.start(play_path, br, perr))
				std::fprintf(stderr, "MIDI ファイル: %s\n", perr.c_str());
			else
				std::printf("再生: %s（%.1f 秒）\n", play_path.c_str(),
				            g_win->play.length());
		}
		std::printf("鳴らしている（待ち時間 %.1f ms、MMCSS %s）\n",
		            1000.0 * out.buffer_frames() / RATE,
		            out.mmcss() ? "登録できた" : "登録できない（途切れやすい）");
		std::fflush(stdout);
	});

	MSG msg;
	while (GetMessageA(&msg, nullptr, 0, 0) > 0) {
		TranslateMessage(&msg);
		DispatchMessageA(&msg);
	}

	// PC の窓に閉じたと知らせる（一覧のミュートを外して受信チャンネルを戻すなど）。
	// 送ったものは音声の糸が流すので、少し待ってから止める
	ui::pc_shutdown_all(g_win->list, g_win->pc, g_win->fx, g_win->shapes, g_win->master, g_win->br);
	Sleep(100);

	// **先に MIDI ファイルを止める。** 止めたときのオールノートオフは音声の糸が THRU から
	// 外へ流すので、音を先に止めると外の機器（実機）に届かず鳴りっぱなしになる。
	// 止めてから、音声の糸が流し終えるのを少し待つ
	if (g_win->play.playing()) {
		g_win->play.stop();
		Sleep(150);
	}
	out.stop();
	// 念のため、THRU の先へ直にもオールサウンドオフ・オールノートオフを送る。
	// 音声の糸はもう止まっているので、ここから送っても取り合いにならない
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
	g_win->join_reboot();
	g_win->flush_card();      // 音はもう止まっている。SmartMedia に書いたものを残す
	g_win->save_settings();   // VOLUME のつまみの位置
	// 音はもう止まっている。起動できていたときだけ残す
	eng.settle_for_save();
	if (eng.state.load() == 1 && !smu2000::nvram::save(eng.mu))
		std::fprintf(stderr, "設定を残せなかった: %s\n", smu2000::nvram::path(eng.mu).c_str());
	// 残した設定で起動した写しも用意しておく（src/bootcache.h）。無いと、
	// 設定をいじった次の 1 回だけ起動が遅くなる。1 秒ほどかかるが、
	// 窓はもう閉じているので待たせない。溜まった古い写しはここで間引く
	if (eng.state.load() == 1) {
		if (smu2000::bootcache::refresh(eng.mu))
			std::printf("次の起動ぶんの写しを作った\n");
		smu2000::bootcache::prune();
	}
	g_win->play.stop();
	for (ui::midi_in &m : midi_ports)
		m.close();
	mout.close();
	mout_mu.close();
	mout_b.close();
	ain.stop();

	// 音を出さずに終わったとき（起動に失敗した、音声デバイスを開けなかった）は、どちらも出さない
	if (out.produced()) {
		std::printf("CPU %.1f%%、1 回の最悪 %.2f ms、間に合わなかった %llu 回\n",
		            out.cpu_percent(), out.worst_ms(),
		            (unsigned long long)out.late());
		std::printf("%s\n%s\n", out.format_line().c_str(), out.latency_line().c_str());
	}
	return 0;
}
