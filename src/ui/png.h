// license:BSD-3-Clause

#ifndef S_MU2000_UI_PNG_H
#define S_MU2000_UI_PNG_H

#pragma once

#include "compat/mamecompat.h"

#include <string>

namespace ui {

// 32bit BGRA を PNG に書き出す。圧縮はしない
bool write_png(const std::string &path, const u8 *bgra, int w, int h, int stride);

} // namespace ui

#endif // S_MU2000_UI_PNG_H
