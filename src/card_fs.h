// license:BSD-3-Clause
//
// SmartMedia の生の並び（smartmedia::raw、カードの画像ファイル）から、FAT のファイルを読む。
// サンプリングの窓の「カード」が、本体に差さずに中身を見るのに使う（doc/sampling.md）。
//
// 並びは smartmedia.h のとおり（1 ページ 512 + 予備 16、32 ページで 1 ブロック）。予備の 6-7 バイトにある
// 論理ブロックの番号（0001 0bbb bbbb bbbp）で物理と論理を結び、論理セクターの並びを作る。
// 1024 ブロックごとのゾーンに論理 0-999。その上は MBR → ブートセクター → FAT12/16 → ルート（smartmedia::format）。
// 読むだけ。書くのは firmware（本体の SAVE）か smartmedia::format。

#ifndef S_MU2000_CARD_FS_H
#define S_MU2000_CARD_FS_H

#pragma once

#include "compat/mamecompat.h"

#include <string>
#include <vector>

namespace smu2000::cardfs {

struct entry {
	std::string path;   // "/" で区切る（ルートのファイルは名前だけ）。8.3 の大文字
	u32 size = 0;
	u32 cluster = 0;
};

// カードのファイルを全部（下のディレクトリも）並べる。読めなければ false（理由は err）
bool list(const std::vector<u8> &raw, std::vector<entry> &out, std::string &err);
// path のファイルを読む
bool read(const std::vector<u8> &raw, const std::string &path, std::vector<u8> &out, std::string &err);

} // namespace smu2000::cardfs

#endif // S_MU2000_CARD_FS_H
