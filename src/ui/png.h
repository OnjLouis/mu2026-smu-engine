// license:BSD-3-Clause

#ifndef S_MU2000_UI_PNG_H
#define S_MU2000_UI_PNG_H

#pragma once

#include "compat/mamecompat.h"

#include <string>
#include <vector>

namespace ui {

// 32bit BGRA を PNG に書き出す。圧縮はしない
bool write_png(const std::string &path, const u8 *bgra, int w, int h, int stride);

// PNG を読む。8bit の グレー / グレー＋α / RGB / RGBA、インターレースなし
// （パネルの絵に使う分だけ）。out は 1 画素 0xAARRGGBB、α はかけていない値
bool read_png(const std::string &path, int &w, int &h, std::vector<u32> &out);

} // namespace ui

#endif // S_MU2000_UI_PNG_H
