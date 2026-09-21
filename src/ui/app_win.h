// license:BSD-3-Clause
//
// The Windows front end's app: ui::app answering the Win32 window (ui/
// window_win.cpp). gui.cpp keeps main(); the Mac's twin is ui/app_mac.h.
// Unlike the Mac, the Win32 headers tolerate the GDI shim, so this class
// can be a plain C++ header.

#ifndef S_MU2000_UI_APP_WIN_H
#define S_MU2000_UI_APP_WIN_H

#pragma once

#include <cstddef>
#include <cstdio>
#include <string>

#include <windows.h>

#include "app.h"
#include "menu.h"
#include "menu_win.h"
#include "pc_window.h"
#include "text.h"
#include "window_win.h"

namespace ui {

// gui.ini lives under %LOCALAPPDATA%
std::string settings_file_path();

class win_app : public app
{
public:
	win_app(bridge &b, midi_in *mi,
	        midi_out &tha, midi_out &thb, midi_out &muo)
	    : app(b, mi, tha, thb, muo) {}

	HWND hwnd = nullptr;             // set at WM_CREATE, for message boxes
	// Double buffering: repainting straight into the window would flicker
	HDC     mem_dc = nullptr;
	HBITMAP mem_bmp = nullptr;
	int     mem_w = 0, mem_h = 0;
	// Choosing from a menu failed: shown at the end of the command
	std::string last_error;

	void open_window_by_kind(int kind) { app::open_window_by_kind(kind); }

	// Opens an existing PC window, warning when it cannot be done
	void open_pc_window(pc_window &w) override
	{
		win_open_window(hwnd, w);
	}

	// The wait/drop fragment is what WASAPI measures (ui/status.h);
	// output_ms and late() are Windows-only (CoreAudio has no wait number)
	void format_middle(char *dst, std::size_t n) override
	{
		std::snprintf(dst, n, "待ち %.0f ms  遅れ %llu",
		              out->output_ms(), (unsigned long long)out->late());
	}

	void print_audio_details() override
	{
		std::printf("%s\n%s\n", out->format_line().c_str(), out->latency_line().c_str());
	}

	// ui::app hooks: file dialogs, confirmations and error display are
	// Win32's business (ui/window_win.h), everything they decide is shared
	std::string settings_path() const override { return settings_file_path(); }
	void menu_error(const std::string &text) override { last_error = text; }
	void menu_note(const std::string &text) override
	{
		win_note(hwnd, text);
	}
	std::string ask_card_open_path() override
	{
		return win_open_file(hwnd, L"差す SmartMedia",
		                         L"SmartMedia の中身 (*.img)\0*.img\0すべて (*.*)\0*.*\0", L"img");
	}
	std::string ask_card_save_path() override
	{
		return win_save_file(hwnd, L"新しい SmartMedia の保存先",
		                         L"SmartMedia の中身 (*.img)\0*.img\0すべて (*.*)\0*.*\0",
		                         L"img", L"smartmedia.img");
	}
	std::string ask_midi_file_path() override
	{
		return win_open_file(hwnd, L"流す MIDI ファイル",
		                         L"MIDI ファイル (*.mid;*.midi)\0*.mid;*.midi\0すべて (*.*)\0*.*\0",
		                         nullptr);
	}
	bool confirm_factory_reset() override
	{
		return win_confirm(hwnd,
		                       "MU2000 を工場出荷状態に戻して、電源を入れ直します。\n"
		                       "ユーティリティの設定や、覚えている音量・音色の設定はすべて消えます。");
	}

	// ---- the window-system shell (ui::app::run drives these)

	bool open_main_window(const char *title, int w, int h) override
	{
		return make_window(title, w, h);
	}
	void pump_window(const char *, int, int) override
	{
		MSG msg;
		while (GetMessageA(&msg, nullptr, 0, 0) > 0) {
			TranslateMessage(&msg);
			DispatchMessageA(&msg);
		}
	}
	// The devices are objects here (WASAPI needs the input side wired into
	// the engine before boot); opening the device itself waits for the
	// firmware (start_audio, on the boot thread)
	void make_audio() override
	{
		static audio_out dev_out;
		static audio_in  dev_in;
		out = &dev_out;
		ain = &dev_in;
		if (eng)
			eng->ain = &dev_in;
	}
	void say_audio_opened(bool) override
	{
		std::printf("音声の出口: %s\n%s\n", out->device_name().c_str(),
		            out->format_line().c_str());
	}
	void say_audio_running() override
	{
		std::printf("鳴らしている（待ち時間 %.1f ms、MMCSS %s）\n",
		            1000.0 * out->buffer_frames() / AUDIO_RATE,
		            out->mmcss() ? "登録できた" : "登録できない（途切れやすい）");
	}
	u64 audio_drops() override { return out->late(); }
};

extern win_app *g_win;

void play_dropped_file(const std::string &path);

} // namespace ui

#endif // S_MU2000_UI_APP_WIN_H
