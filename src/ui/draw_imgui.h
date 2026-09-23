// license:BSD-3-Clause
//
// The panel drawing surface on Dear ImGui: the windows paint through the
// same ImGui renderer the PC editor windows use on every platform
// (DX11 on Windows, Metal on macOS, SDL_Renderer on Linux).
//
// Panel palette first (the same names and packing panel.txt uses), then the
// primitives one by one, painting into an ImDrawList. What this mapping
// taught:
//
// - COLORREF is 0x00BBGGRR while IM_COL32 wants RGBA: extract the bytes
//   with GetRValue/GetGValue/GetBValue (compat/gdi.h, cross-platform),
//   never bit-cast.
// - The three font slots (label/small/tiny) ride along with their px
//   sizes because ImGui rasterizes per size.
// - DT_WORDBREAK becomes a wrap width; DT_SINGLELINE + CENTER/RIGHT become
//   CalcTextSize + cursor math (the bools below).
// - Arcs and ellipses become sampled polylines (ImGui arcs are circular;
//   the panel dials use axis-aligned ellipses). Fills only do convex
//   outlines; the panel art does not rely on holes.

#ifndef S_MU2000_UI_DRAW_IMGUI_H
#define S_MU2000_UI_DRAW_IMGUI_H

#pragma once

#include "compat/gdi.h"   // COLORREF + GetRValue/GetGValue/GetBValue only

#include "imgui.h"

#include <cmath>

namespace ui {
// ---- 色

inline constexpr COLORREF BODY      = RGB(28, 30, 34);
inline constexpr COLORREF BODY_TOP  = RGB(44, 47, 53);
inline constexpr COLORREF BEZEL     = RGB(12, 12, 12);
inline constexpr COLORREF LCD_BACK  = RGB(150, 205, 45);
inline constexpr COLORREF LCD_GHOST = RGB(140, 194, 44);   // 消えている点。実物もうっすら見える
inline constexpr COLORREF LCD_DOT   = RGB(18, 22, 14);
inline constexpr COLORREF LED_OFF   = RGB(20, 28, 10);
inline constexpr COLORREF LED_ON    = RGB(178, 255, 51);
inline constexpr COLORREF BTN_FACE  = RGB(58, 62, 68);
inline constexpr COLORREF BTN_EDGE  = RGB(92, 97, 104);
inline constexpr COLORREF BTN_DOWN  = RGB(126, 170, 70);
inline constexpr COLORREF TEXT      = RGB(226, 229, 233);
inline constexpr COLORREF TEXT_DIM  = RGB(150, 155, 162);
inline constexpr COLORREF WHEEL     = RGB(46, 49, 54);
inline constexpr COLORREF WHEEL_EDGE= RGB(96, 101, 108);
inline constexpr COLORREF ACCENT    = RGB(126, 200, 90);

namespace im {



inline ImU32 col(COLORREF c, unsigned char a = 255)
{
	return IM_COL32(GetRValue(c), GetGValue(c), GetBValue(c), a);
}

// The three panel slots (label/small/tiny, re-rasterized at every resize
// in panel.cpp). px rides along because ImGui rasterizes per size.
struct fonts {
	ImFont *label = nullptr, *small = nullptr, *tiny = nullptr;
	float label_px = 13.0f, small_px = 8.5f, tiny_px = 6.5f;
};

inline ImVec2 pos_of(const RECT &r) { return ImVec2(float(r.left), float(r.top)); }
inline ImVec2 size_of(const RECT &r)
{
	return ImVec2(float(r.right - r.left), float(r.bottom - r.top));
}

inline void fill(ImDrawList *dl, ImVec2 pos, ImVec2 size, COLORREF c)
{
	dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), col(c));
}

inline void fill(ImDrawList *dl, const RECT &r, COLORREF c)
{
	fill(dl, pos_of(r), size_of(r), c);
}

// 角を落とした四角。ボタンはこれで描く
inline void round_box(ImDrawList *dl, const RECT &r, COLORREF face,
                      COLORREF edge, float radius)
{
	dl->AddRectFilled(pos_of(r), ImVec2(float(r.right), float(r.bottom)),
	                  col(face), radius);
	dl->AddRect(pos_of(r), ImVec2(float(r.right), float(r.bottom)),
	            col(edge), radius, 0, 1.0f);
}

inline void disc(ImDrawList *dl, ImVec2 center, float r, COLORREF face,
                 COLORREF edge, float pen = 2.0f)
{
	dl->AddCircleFilled(center, r, col(face));
	dl->AddCircle(center, r, col(edge), 0, pen);
}

inline void disc(ImDrawList *dl, int cx, int cy, int r, COLORREF face,
                 COLORREF edge, int pen = 2)
{
	disc(dl, ImVec2(float(cx), float(cy)), float(r), face, edge, float(pen));
}

inline void line(ImDrawList *dl, ImVec2 a, ImVec2 b, COLORREF c, float width)
{
	dl->AddLine(a, b, col(c), width);
}

inline void line(ImDrawList *dl, int x1, int y1, int x2, int y2, COLORREF c,
                 int width)
{
	line(dl, ImVec2(float(x1), float(y1)), ImVec2(float(x2), float(y2)), c,
	     float(width));
}

// 凸多角形の塗りと輪郭 (panel.cpp の Polygon / svg.cpp の PolyPolygon の 1 区画ぶん)
inline void poly(ImDrawList *dl, const ImVec2 *pts, int n, COLORREF fill_c,
                 COLORREF edge_c, float pen = 1.0f, bool closed = true)
{
	dl->AddConvexPolyFilled(const_cast<ImVec2 *>(pts), n, col(fill_c));
	if (closed)
		dl->AddPolyline(const_cast<ImVec2 *>(pts), n, col(edge_c),
		                ImDrawFlags_Closed, pen);
	else
		dl->AddPolyline(const_cast<ImVec2 *>(pts), n, col(edge_c), 0, pen);
}

// 目盛りの弧 (panel.cpp の Arc)。a0/a1 はラジアン
inline void arc(ImDrawList *dl, ImVec2 center, float rx, float ry, float a0,
                float a1, COLORREF c, float width)
{
	// ImGui arcs are circular; the panel dials use axis-aligned ellipses,
	// so sample the ellipse into a polyline (segments look identical).
	const int segs = 48;
	dl->PathClear();
	for (int i = 0; i <= segs; i++) {
		const float a = a0 + (a1 - a0) * float(i) / float(segs);
		dl->PathLineTo(ImVec2(center.x + rx * cosf(a), center.y + ry * cosf(a) * 0 + ry * sinf(a)));
	}
	dl->PathStroke(col(c), 0, width);
}

inline void ellipse(ImDrawList *dl, ImVec2 center, float rx, float ry,
                    COLORREF c, float width)
{
	// Same elliptical sampling as arc(), full turn.
	arc(dl, center, rx, ry, 0.0f, 2.0f * 3.14159265f, c, width);
}

// 字は UTF-8 のまま渡す (ImGui wants UTF-8)。font + px は label/small/tiny
// のどれか（未指定なら既定の字）。
inline void text_in(ImDrawList *dl, ImVec2 pos, ImVec2 size, const char *s,
                    COLORREF c, ImFont *font = nullptr, float px = 0.0f,
                    bool center_x = false, bool center_y = false,
                    bool wrap = false)
{
	if (!s || !s[0])
		return;
	const ImVec2 ts = font ? font->CalcTextSizeA(px, FLT_MAX, 0.0f, s)
	                       : ImGui::CalcTextSize(s);
	ImVec2 at = pos;
	if (center_x)
		at.x = pos.x + (size.x - ts.x) * 0.5f;
	if (center_y)
		at.y = pos.y + (size.y - ts.y) * 0.5f;
	if (wrap)
		dl->PushClipRect(pos, ImVec2(pos.x + size.x, pos.y + size.y), true);
	if (font)
		dl->AddText(font, px, at, col(c), s, nullptr, wrap ? size.x : 0.0f);
	else
		dl->AddText(at, col(c), s);
	if (wrap)
		dl->PopClipRect();
}

} // namespace im
} // namespace ui

#endif // S_MU2000_UI_DRAW_IMGUI_H
