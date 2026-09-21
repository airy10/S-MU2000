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
#include "ui/tool_args.h"
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
// ---- 口を選ぶ品書き

// --shot renders through the shared painter (ui/shot.h), like the window

} // namespace


int main(int argc, char **argv)
{
	SetConsoleOutputCP(CP_UTF8);

	ui::tool_args a;
	a.latency = 20;        // 溜める目標 (per-backend default; the shared parser keeps it)
	ui::output_options out_opts;
	ui::window_options win_opts;
	ui::engine_options eng_opts;

	// The flags are shared (ui/tool_args.h); only latency above stays per side
	const int parsed = ui::parse_tool_args(argc, argv, a, eng_opts, out_opts, win_opts);
	if (parsed >= 0)
		return parsed;

	static ui::bridge br;
	static ui::midi_in  midi_ports[mu2000::MIDI_PORTS];
	static ui::midi_out mout, mout_b, mout_mu;
	static win_app gui(br, midi_ports, mout, mout_b, mout_mu);
	g_win = &gui;
	gui.keep_settings = a.nomidi;

	// 絵だけ欲しい場合。ROM が無くても中身が空の画面は出せる
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
	gui.wire_engine(eng, eng_opts);
	gui.eng = &eng;
	if (!gui.load_machine(eng, a))
		return 1;
	const int shot = gui.run_boot_shot(eng, br, a, win_opts);
	if (shot >= 0)
		return shot;
	// ---- 窓を出す

	const HINSTANCE inst = GetModuleHandleA(nullptr);
	WNDCLASSA wc{};
	wc.lpfnWndProc   = wnd_proc;
	wc.hInstance     = inst;
	wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
	wc.lpszClassName = "SMU2000Panel";
	wc.hbrBackground = nullptr;
	RegisterClassA(&wc);

	RECT want{ 0, 0, a.win_w, a.win_h };
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

	gui.eng  = &eng;
	gui.setup_for_window(a, win_opts, out_opts.factory);

	eng.publish();
	ShowWindow(hwnd, SW_SHOW);
	gui.open_startup_windows(win_opts);
	UpdateWindow(hwnd);

	// 起動は別スレッド。終わったら音を出し始める
	static ui::audio_out out;
	gui.out = &out;
	static ui::audio_in ain;
	gui.ain = &ain;
	eng.ain = &ain;
	std::thread boot_thread([&] {
		if (!eng.boot()) {
			eng.state.store(2);
			eng.publish();
			return;
		}
		// 起動が終わってから入れる（起動には firmware が要る）
		gui.apply_native_engine(eng, eng_opts);
		eng.state.store(1);
		eng.publish();

		// 前に選んだ口を名前で探す。--midi / --midiout があればそちらが勝つ
		gui.open_remembered_ports(a, out_opts);

		std::string err;

		if (!gui.start_audio(a.latency, out_opts.exclusive))
			return;
		std::printf("音声の出口: %s\n%s\n", out.device_name().c_str(),
		            out.format_line().c_str());
		// A/D INPUT。前に選んだ録音デバイスがあれば開く（開けなくても名前は覚えておく）
		gui.start_ad();
		// --play が付いていれば、鳴り始めたところで流し出す
		if (!a.play_path.empty())
			gui.play_song(a.play_path);
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

	if (boot_thread.joinable())
		boot_thread.join();
	gui.shutdown();
	gui.print_exit_stats(out.late());
	return 0;
}
