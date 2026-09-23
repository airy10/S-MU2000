// license:BSD-3-Clause
//
// Geometry and colour types in Windows's packing, shared by the panel code
// on every platform: RECT/POINT for bounds and spots, COLORREF (red low,
// blue in bits 16-23) for the palette, DT_* for layout.txt alignment bits.
//
// This used to be the GDI drawing surface (handles, device contexts, the
// drawing calls, filled in per platform); the panel now paints through
// Dear ImGui (ui/draw_imgui.h, ui/imgui_shell.h) and none of that remains.
//
// The colour packing is Windows's, with red in the low byte and blue in bits
// 16-23. panel.txt writes colours as #rrggbb, so this must not be changed.

#ifndef S_MU2000_COMPAT_GDI_H
#define S_MU2000_COMPAT_GDI_H

#pragma once

#if defined(_WIN32)

// ---- Windows: use the real thing ----------------------------------------

#include <windows.h>

#else

// ---- Everyone else: the slice still in use --------------------------------

#include <cstddef>
#include <cstdint>

// ---- Base types

using BYTE  = uint8_t;
using WORD  = uint16_t;
using DWORD = uint32_t;
using UINT  = uint32_t;
using INT   = int32_t;
// bool, agreeing with objc/objc.h's BOOL, so this header can share a
// translation unit with Cocoa.
using BOOL  = bool;
using UINT_PTR = uintptr_t;

// LONG is `long`, as it is in the Windows headers, rather than a fixed
// 32-bit type. The drawing code writes things like std::max(1L, ...) with
// a coordinate, and those only deduce if the two are the same type.
using LONG = long;

#ifndef TRUE
#define TRUE  1
#define FALSE 0
#endif

// ---- Colour, packed exactly as Windows packs it: red low, blue in bits 16-23

using COLORREF = DWORD;

inline constexpr COLORREF RGB(int r, int g, int b)
{
	return COLORREF((DWORD(r) & 0xff) | ((DWORD(g) & 0xff) << 8) | ((DWORD(b) & 0xff) << 16));
}

inline constexpr BYTE GetRValue(COLORREF c) { return BYTE(c & 0xff); }
inline constexpr BYTE GetGValue(COLORREF c) { return BYTE((c >> 8) & 0xff); }
inline constexpr BYTE GetBValue(COLORREF c) { return BYTE((c >> 16) & 0xff); }

// ---- Coordinates

struct RECT  { LONG left, top, right, bottom; };
struct POINT { LONG x, y; };
struct SIZE  { LONG cx, cy; };

// Text placement, as layout.txt names it. Note that DT_LEFT and DT_TOP are
// 0: being left- or top-aligned is the default, so you cannot test for it,
// only for the others.
enum { DT_TOP = 0x0000, DT_LEFT = 0x0000, DT_CENTER = 0x0001, DT_RIGHT = 0x0002,
       DT_VCENTER = 0x0004, DT_BOTTOM = 0x0008, DT_WORDBREAK = 0x0010,
       DT_SINGLELINE = 0x0020, DT_NOCLIP = 0x0100, DT_END_ELLIPSIS = 0x8000 };

#endif // _WIN32

#endif // S_MU2000_COMPAT_GDI_H
