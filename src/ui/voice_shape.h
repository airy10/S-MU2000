// license:BSD-3-Clause
//
// 音色の中身（ROM の要素の記録）とパートの値から、**実際の動き**を組んで絵に渡す。
// パートの音色の窓の絵（ピッチ EG など）が使う。
//
// 式は native の口（xg/native_voice.h）と同じもので、どれも実機の firmware が書くレジスタと
// 突き合わせて確かめてある。時間はチップの包絡線の刻み（swp30 の level_step）を平均して出す。
// 描く鍵と強さは決め打ち（鍵 60・強さ 100）で、鍵や強さで動く分は入らない。
//
// Windows にも画面にも依存しない。

#ifndef S_MU2000_UI_VOICE_SHAPE_H
#define S_MU2000_UI_VOICE_SHAPE_H

#pragma once

#include "mame/sound/swp30.h"
#include "xg/native_voice.h"
#include "xg/ram.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace ui {
namespace shape {

constexpr int NOTE = 60;
constexpr int VEL = 100;
constexpr double RATE = 44100.0;

// 包絡線の 1 サンプルあたりの平均の歩幅（チップの刻みを 65536 サンプルぶん数える）
inline double avg_step(int speed)
{
	static std::array<double, 160> cache = [] {
		std::array<double, 160> c{};
		for (int i = 0; i < 160; i++) {
			const int sp = i - 32;
			u64 sum = 0;
			for (u32 sc = 0; sc < 65536; sc++)
				sum += swp30_device::envelope_step(sp, sc);
			c[size_t(i)] = double(sum) / 65536.0;
		}
		return c;
	}();
	return cache[size_t(std::clamp(speed + 32, 0, 159))];
}

// 音程の包絡線の 1 点（時刻 ms、ずれ セント）
struct pt { float ms, cents; };

// 要素 1 つぶんの音程の動き。鍵を離した時刻（ms）と、この鍵・強さで鳴る要素か
struct peg_line {
	std::vector<pt> pts;
	float keyoff_ms = 0;
	bool active = true;
	bool moves = false;       // 素のままでも動く要素か（素の高さが全部 64 でない）
};

// レジスタ 0x10 の値（14bit 符号つき、1 オクターブ 1024）→ セント
inline float units_cents(u16 reg)
{
	int v = int(reg & 0x3fff);
	if (v & 0x2000)
		v -= 0x4000;
	return float(v) * 1200.0f / 1024.0f;
}

// 音色（rec）とパートの塊（part_ram。XG のパート番号の並びに直した写し）から、要素ごとの
// 音程の動きを組む。hold_ms は「行き着いてから離すまで」の間
inline std::vector<peg_line> peg_lines(const u8 *rom, u32 rec, const u8 *part, float hold_ms)
{
	namespace nv = xg::nv;
	std::vector<peg_line> out;
	if (!rom || !rec)
		return out;
	const int n = nv::element_count(rom, rec);
	std::vector<float> at_end(size_t(n), 0.0f), level(size_t(n), 0.0f);
	for (int e = 0; e < n; e++) {
		const u8 *el = nv::element(rom, rec, e);
		peg_line line;
		line.active = nv::element_active(el, NOTE, VEL);
		line.moves = el[30] != 64 || el[31] != 64 || el[32] != 64 || el[33] != 64 || el[34] != 64;
		// 鍵を押した瞬間（build_note と同じ）
		const int part_init = part[0x62], part_atk = part[0x63];
		const int cc_atk = part[0x1a], cc_dec = part[0x1b], cc_rel = part[0x1c];
		const int praw = part_atk == 64 ? int(el[26]) : nv::eg_rate_cc(rom, int(el[26]), part_atk);
		const int prate = nv::peg_rate_reg_raw(rom, el, praw, NOTE, VEL, 64, cc_atk);
		const u16 start = nv::peg_reg(rom, prate == 127 ? nv::peg_cents(el, el[31], VEL)
		                                                : nv::peg_cents(el, el[30], VEL) + nv::part_peg_cents(part_init),
		                              el);
		double ms = 0;
		float cur = units_cents(start);
		line.pts.push_back({ 0, cur });
		// 1 段ぶん。速さのレジスタと行き先（0x10 の値）から、着くまでの時間
		auto go = [&](int rate_reg, u16 target_reg) {
			const float tgt = units_cents(target_reg);
			const double step = avg_step(rate_reg - 16) * 1200.0 / 1024.0;     // セント / サンプル
			const double dist = std::fabs(double(tgt - cur));
			if (dist > 0 && step > 0) {
				ms += dist / step / RATE * 1000.0;
				line.pts.push_back({ float(ms), tgt });
			}
			cur = tgt;
		};
		go(prate, nv::peg_reg(rom, nv::peg_cents(el, el[31], VEL), el));
		// 段 1・2（行き先が前の段と同じなら飛ばす。peg_advance と同じ）
		if (nv::peg_level_of(el, 0) != nv::peg_level_of(el, 1))
			go(nv::peg_rate_reg_stage(rom, el, 1, NOTE, VEL, 64, 64),
			   nv::peg_reg(rom, nv::peg_cents(el, el[32], VEL), el));
		if (nv::peg_level_of(el, 1) != nv::peg_level_of(el, 2))
			go(nv::peg_rate_reg_stage(rom, el, 2, NOTE, VEL, 64, cc_dec),
			   nv::peg_reg(rom, nv::peg_cents(el, el[33], VEL), el));
		at_end[size_t(e)] = float(ms);
		level[size_t(e)] = cur;
		out.push_back(std::move(line));
	}
	// 鍵を離すのは全部の要素で同時。いちばん遅く行き着いた要素から hold_ms 後
	float keyoff = 0;
	for (float t : at_end)
		keyoff = std::max(keyoff, t);
	keyoff += hold_ms;
	for (int e = 0; e < n; e++) {
		const u8 *el = nv::element(rom, rec, e);
		peg_line &line = out[size_t(e)];
		float cur = level[size_t(e)];
		double ms = keyoff;
		line.keyoff_ms = keyoff;
		line.pts.push_back({ keyoff, cur });
		const int cc_rel = part[0x1c];
		auto go = [&](int rate_reg, u16 target_reg) {
			const float tgt = units_cents(target_reg);
			const double step = avg_step(rate_reg - 16) * 1200.0 / 1024.0;
			const double dist = std::fabs(double(tgt - cur));
			if (dist > 0 && step > 0) {
				ms += dist / step / RATE * 1000.0;
				line.pts.push_back({ float(ms), tgt });
			}
			cur = tgt;
		};
		// 離し（peg_release と同じ）
		int raw = int(el[29]);
		const int d = int(part[0x65]) - 64;
		if (d > 0) {
			const int tb = int(rom[nv::PEG_REL_TAB + u32(d)]);
			if (raw > tb)
				raw = tb;
		} else {
			raw -= d >> 1;
			if (raw > 63)
				raw = 63;
		}
		const int lv = std::clamp(int(el[34]) + (int(part[0x64]) - 64), 0, 127);
		go(nv::peg_rate_reg_raw(rom, el, raw, NOTE, VEL, 64, cc_rel),
		   nv::peg_reg(rom, nv::peg_cents(el, lv, VEL), el));
	}
	return out;
}

// ---- 音量の包絡線
//
// チップ（swp30 の envelope_block::step）と同じ動きを、128 サンプルずつまとめて進める。
// 減衰量は 1024 で 6.02 dB（4.10 の浮動小数）。はじめの減衰量は立ち上がりのレジスタの下位、
// 立ち上がりの速さは今の減衰量でも速くなる（`(減衰量 >> 9) << 2` を足す）。減衰 1・2 は
// 下位の行き先へ、離しは下へ。レジスタは native の口の build_note・release_reg そのまま
struct amp_line {
	std::vector<pt> pts;          // ms と dB（cents の欄に dB を入れる）
	float keyoff_ms = 0;
	float attack_ms = 0, decay1_ms = 0, sustain_db = 0;   // つまむ点に使う
	bool active = true;
};

inline float att_db(double level) { return float(-level * 6.0206 / 1024.0); }

// 1 要素ぶんを進める。keyoff_ms が負なら離さない（減衰 2 の終わりまで、上限 limit_ms）
inline amp_line amp_run(const u8 *rom, const u8 *el, const u8 *part, float keyoff_ms, float limit_ms)
{
	namespace nv = xg::nv;
	amp_line line;
	line.active = nv::element_active(el, NOTE, VEL);
	const int cc_atk = part[0x1a], cc_dec = part[0x1b], cc_rel = part[0x1c];
	const nv::slot_regs sr = nv::build_note(rom, el, NOTE, 0, nullptr, nv::defaults(), 0, VEL, cc_atk, cc_dec,
	                                        64, 64, -1, NOTE, false, part[0x62], part[0x63]);
	const u16 atk = sr.v[0x06], dc1 = sr.v[0x07], dc2 = sr.v[0x08];
	const u16 rel = nv::release_reg(rom, el, NOTE, 0, cc_rel);
	constexpr int CHUNK = 128;
	const double chunk_ms = CHUNK / RATE * 1000.0;
	double level = (atk & 0xff) ? double((atk & 0xff) << 6) : 0.0;
	int mode = (atk & 0xff) ? 0 : 1;            // 0 立ち上がり、1 減衰 1、2 減衰 2、3 離し
	double ms = 0;
	line.pts.push_back({ 0, att_db(level) });
	int last_mode = mode;
	int since = 0;
	while (ms < limit_ms) {
		if (keyoff_ms >= 0 && mode != 3 && ms >= keyoff_ms) {
			line.pts.push_back({ float(ms), att_db(level) });
			mode = 3;
		}
		if (mode == 0) {
			level -= avg_step((atk >> 8) + ((int(level) >> 9) << 2)) * CHUNK;
			if (level <= 0) {
				level = 0;
				mode = 1;
				line.attack_ms = float(ms + chunk_ms);
			}
		} else if (mode == 1 || mode == 2) {
			const u16 reg = mode == 1 ? dc1 : dc2;
			const double limit = double((reg & 0xff) << 6);
			const double step = avg_step(reg >> 8) * CHUNK;
			if (level < limit)
				level = std::min(limit, level + step);
			else if (level > limit)
				level = std::max(limit, level - step);
			if (level == limit) {
				if (mode == 1)
					line.decay1_ms = float(ms + chunk_ms);
				if (mode == 2 && keyoff_ms < 0) {
					ms += chunk_ms;
					line.pts.push_back({ float(ms), att_db(level) });
					break;                          // 減衰 2 の終わり（伸ばしている音量）
				}
				if (mode == 1)
					mode = 2;
			}
		} else {
			level = std::min(double(0x3fff), level + avg_step(((rel >> 8) ^ 0x80) & 0xff) * CHUNK);
		}
		ms += chunk_ms;
		// 点は形が変わるところと、途中は間引いて
		if (mode != last_mode || ++since >= 8) {
			line.pts.push_back({ float(ms), att_db(level) });
			last_mode = mode;
			since = 0;
		}
		if (mode == 3 && att_db(level) < -72.0f)
			break;
	}
	line.sustain_db = att_db(double((dc2 & 0xff) << 6));
	line.keyoff_ms = keyoff_ms;
	return line;
}

// 要素ごとの音量の動き。鍵を離すのは、いちばん遅く減衰 2 を終えた要素から hold_ms 後
// （減衰 2 がとても長い音色――ピアノなど――は 2 秒で打ち切って離す）
inline std::vector<amp_line> amp_lines(const u8 *rom, u32 rec, const u8 *part, float hold_ms)
{
	namespace nv = xg::nv;
	std::vector<amp_line> out;
	if (!rom || !rec)
		return out;
	const int n = nv::element_count(rom, rec);
	float keyoff = 0;
	for (int e = 0; e < n; e++) {
		const amp_line a = amp_run(rom, nv::element(rom, rec, e), part, -1, 2000.0f);
		keyoff = std::max(keyoff, a.pts.back().ms);
	}
	keyoff += hold_ms;
	for (int e = 0; e < n; e++)
		out.push_back(amp_run(rom, nv::element(rom, rec, e), part, keyoff, keyoff + 12000.0f));
	return out;
}

} // namespace shape
} // namespace ui

#endif // S_MU2000_UI_VOICE_SHAPE_H
