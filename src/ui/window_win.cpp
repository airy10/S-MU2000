// license:BSD-3-Clause
//
// The Windows event translation for gui: the message switch, painting,
// popups, drops and buffering. gui.cpp keeps the app class instance and
// main(); everything Win32 here mirrors ui/window_mac.mm on the Mac side.

#include "window_win.h"

#include "ui/keymap_win.h"
#include "ui/pc_host.h"

#include <windowsx.h>
#include <shellapi.h>

namespace ui {

win_app *g_win = nullptr;

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

void play_dropped_file(const std::string &path)
{
	// Outside a menu command, so a failure shows straight away rather than
	// through last_error at the end of WM_COMMAND
	if (!g_win->play_song(path) && !g_win->last_error.empty()) {
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
	ui::win_open_window(hwnd, *ui::window_for_kind(kind, list, pc, fx, shapes, master));
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
			ui::win_error(hwnd, g_win->last_error);
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
			g_win->reload_layout();
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

} // namespace ui
