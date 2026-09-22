// license:BSD-3-Clause

#include "part_shapes.h"

#include "overview.h"
#include "fx_editor.h"
#include "fx_help.h"
#include "fx_icons.h"

#include "imgui.h"
#include "xg/fx_params.h"
#include "xg/fx_types.h"
#include "xg/sysfx.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace ui {

using namespace xgui;

namespace {

// パートは口 A-D の 64（C・D は実機では USB だけの口）
constexpr int PARTS = XG_PARTS;
constexpr float BAR_SCALE = 0.85f;   // 下の説明の帯の字の大きさ（本文に対して）

// 「グラフ ○ つまみ」の切り替え。見出しの行の右端に描き、押されたら true。
// 見出しと並べて入らなければ字を外して切り替えだけにし、それでも入らなければ見出しを切る
bool title_toggle(const char *title, const char *id, bool knobs, bool &toggle_hovered)
{
	const float fs = ImGui::GetFontSize();
	const char *l = "グラフ", *r = "つまみ";
	const float lw = ImGui::CalcTextSize(l).x, rw = ImGui::CalcTextSize(r).x;
	const float th = fs * 0.9f, tw = th * 1.8f, gap = fs * 0.3f;
	const float room = ImGui::GetContentRegionAvail().x;
	const float title_w = ImGui::CalcTextSize(title).x;
	const bool words = title_w + fs + lw + gap + tw + gap + rw <= room;
	const float total = words ? lw + gap + tw + gap + rw : tw;
	const ImVec2 start = ImGui::GetCursorScreenPos();
	const float line_h = ImGui::GetTextLineHeight();
	// 見出し（切り替えにかからないところまで）
	ImDrawList *dl = ImGui::GetWindowDrawList();
	const float title_room = std::max(0.0f, room - total - fs * 0.5f);
	dl->PushClipRect(start, ImVec2(start.x + title_room, start.y + line_h), true);
	dl->AddText(start, ImGui::GetColorU32(ImGuiCol_Text), title);
	dl->PopClipRect();
	ImGui::Dummy(ImVec2(title_room, line_h));
	if (ImGui::IsItemHovered() && title_w > title_room)
		hint("%s", title);
	ImGui::SameLine(0, 0);
	ImGui::SetCursorScreenPos(ImVec2(start.x + room - total, start.y));
	const ImVec2 p = ImGui::GetCursorScreenPos();
	const bool pressed = ImGui::InvisibleButton(id, ImVec2(total, line_h));
	toggle_hovered = ImGui::IsItemHovered();
	if (toggle_hovered)
		hint(knobs ? "いまは「つまみ」（値の棒で触る）。クリックで「グラフ」（絵で触る）に切り替える"
		           : "いまは「グラフ」（絵で触る）。クリックで「つまみ」（値の棒で触る）に切り替える");
	const ImU32 on = ImGui::GetColorU32(ImGuiCol_Text), off = ImGui::GetColorU32(ImGuiCol_TextDisabled);
	float sx = p.x;
	if (words) {
		dl->AddText(p, knobs ? off : on, l);
		dl->AddText(ImVec2(p.x + lw + gap * 2 + tw, p.y), knobs ? on : off, r);
		sx += lw + gap;
	}
	const float ty = p.y + (line_h - th) * 0.5f;
	const ImVec2 a(sx, ty), b(sx + tw, ty + th);
	dl->AddRectFilled(a, b, ImGui::GetColorU32(ImGui::IsItemHovered() ? ImGuiCol_FrameBgHovered : ImGuiCol_FrameBg), th * 0.5f);
	const float kx = knobs ? b.x - th * 0.5f : a.x + th * 0.5f;
	dl->AddCircleFilled(ImVec2(kx, ty + th * 0.5f), th * 0.38f, ImGui::GetColorU32(ImGuiCol_SliderGrabActive));
	return pressed;
}

// 1 つの区画。見出しと、大きな絵か値の棒（右上の切り替えで選ぶ。index が負なら絵は無く棒だけ、
// PANEL_FIXED なら切り替えは無く、いつも draw で描く）
constexpr int PANEL_FIXED = -2;
template <typename Draw>
void panel(const char *id, const char *title, float w, float h, int part, xg::model &m, bridge &br,
           std::initializer_list<const char *> keys, int index, Draw draw, const char *about = nullptr)
{
	const float fs = ImGui::GetFontSize();
	// 見出しを枠の上端に寄せる（上下の余白を詰める）
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(ImGui::GetStyle().WindowPadding.x, fs * 0.1f));
	const bool open = ImGui::BeginChild(id, ImVec2(w, h), ImGuiChildFlags_Borders);
	ImGui::PopStyleVar();
	if (!open) {
		ImGui::EndChild();
		return;
	}
	const bool knobs = index != PANEL_FIXED && (index < 0 || shapes_knobs(index));
	bool toggle_hovered = false;
	ImGui::PushFont(nullptr, fs * 0.8f);      // 見出しは小さめに
	if (index < 0)
		ImGui::TextUnformatted(title);
	else if (title_toggle(title, "##mode", knobs, toggle_hovered))
		set_shapes_knobs(index, !knobs);
	ImGui::PopFont();
	const std::string before = hint_text();
	if (!knobs) {
		// 絵だけ。区画の残りを全部使う
		const ImVec2 avail = ImGui::GetContentRegionAvail();
		draw(part, m, br, avail.x, std::max(fs * 4.0f, avail.y));
	} else {
		ImGui::PushItemWidth(-fs * 6.0f);
		for (const char *k : keys)
			param_slider(k, part, m, br);
		ImGui::PopItemWidth();
	}
	// カーソルの下の部品が説明を出さなかったら、区画そのものの説明を
	if (about && !toggle_hovered && hint_text() == before &&
	    ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem))
		hint("%s\n%s", title, about);
	ImGui::EndChild();
}

// ---- モジュレーションのマトリクス。操作子 6 つ（行）× 行き先 6 つ（列）の深さ
//
// 行き先の 3 つ（音程・切る高さ・音量）は「動かす」量で 64 が 0（±）、残りの 3 つは LFO の揺れの深さ（0-127）。
// マスの色は、± の量なら + が青・− が赤、深さなら橙で、濃さが量。既定から外れたマスは枠を明るく。
// マスを上下にドラッグ（2 ドットで 1）、マウスホイール（1 目で 1、Ctrl で 10）、ダブルクリックで既定に戻す
void mod_matrix(int part, xg::model &m, bridge &br, float w, float h)
{
	struct src { const char *key, *name, *about; };
	static const src SRCS[6] = {
		{ "mw",   "モジュレーション",   "モジュレーションホイール（CC1）" },
		{ "bend", "ピッチベンド",       "ピッチベンド（中央から離した量。向きは問わない）" },
		{ "cat",  "チャンネル AT",      "チャンネルアフタータッチ（鍵盤を押し込む強さ。チャンネルに 1 つ）" },
		{ "pat",  "ポリ AT",            "ポリアフタータッチ（鍵ごとの押し込む強さ）" },
		{ "ac1",  "AC1",                "AC1（AC1 CC No で選んだコントロールチェンジ）" },
		{ "ac2",  "AC2",                "AC2（AC2 CC No で選んだコントロールチェンジ）" },
	};
	struct dst { const char *key, *name, *sub; };
	static const dst DSTS[6] = {
		{ "pitch",    "音程",     "Pitch" },
		{ "filter",   "音色",     "Filter" },
		{ "amp",      "音量",     "Amp" },
		{ "lfo_pmod", "LFO 音程", "ビブラート" },
		{ "lfo_fmod", "LFO 音色", "ワウ" },
		{ "lfo_amod", "LFO 音量", "トレモロ" },
	};
	ImGuiIO &io = ImGui::GetIO();
	const float fs = ImGui::GetFontSize();
	ImDrawList *dl = ImGui::GetWindowDrawList();
	const ImVec2 org = ImGui::GetCursorScreenPos();
	const float head_w = std::min(fs * 8.0f, w * 0.26f);
	const float head_h = ImGui::GetTextLineHeight() * 2.2f;
	const float gap = std::max(2.0f, fs * 0.15f);
	const float cw = (w - head_w) / 6.0f, ch = std::max(fs * 1.6f, (h - head_h) / 6.0f);
	ImGuiStorage *st = ImGui::GetStateStorage();

	// 列の見出し（2 行: 行き先と、揺れなら何になるか）。動かすのと揺らすのの間に線
	for (int c = 0; c < 6; c++) {
		const float x = org.x + head_w + cw * float(c);
		const ImVec2 a = ImGui::CalcTextSize(DSTS[c].name), b = ImGui::CalcTextSize(DSTS[c].sub);
		dl->AddText(ImVec2(x + (cw - a.x) * 0.5f, org.y), ImGui::GetColorU32(ImGuiCol_Text), DSTS[c].name);
		dl->AddText(ImVec2(x + (cw - b.x) * 0.5f, org.y + ImGui::GetTextLineHeight()), ImGui::GetColorU32(ImGuiCol_TextDisabled), DSTS[c].sub);
	}
	{
		const float x = org.x + head_w + cw * 3.0f - gap * 0.5f;
		dl->AddLine(ImVec2(x, org.y), ImVec2(x, org.y + head_h + ch * 6.0f), ImGui::GetColorU32(ImGuiCol_Separator), 1.0f);
	}

	for (int r = 0; r < 6; r++) {
		const float y = org.y + head_h + ch * float(r);
		// 行の見出し。AC1・AC2 は CC の番号もここで（マウスホイールで変える）
		ImGui::SetCursorScreenPos(ImVec2(org.x, y));
		ImGui::PushID(r);
		ImGui::InvisibleButton("##row", ImVec2(head_w - gap, ch - gap));
		const bool row_hover = ImGui::IsItemHovered();
		dl->AddRectFilled(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), ImGui::GetColorU32(row_hover ? ImGuiCol_HeaderHovered : ImGuiCol_Header, 0.35f), 3.0f);
		std::string label = SRCS[r].name;
		if (r >= 4) {
			const std::string cck = std::string("part.") + SRCS[r].key + "_cc";
			const xg::param &cp = P(cck.c_str());
			int ccv = 0;
			if (m.get(cp, part, ccv)) {
				char b[32];
				std::snprintf(b, sizeof(b), "  CC%d", ccv);
				label += b;
				if (row_hover) {
					ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);
					if (io.MouseWheel != 0.0f) {
						const int nv = std::clamp(ccv + (io.MouseWheel > 0 ? 1 : -1) * (io.KeyCtrl ? 10 : 1), cp.min, cp.max);
						if (nv != ccv)
							br.send(m.set(cp, part, nv));
					}
				}
			}
		}
		const ImVec2 ls = ImGui::CalcTextSize(label.c_str());
		dl->AddText(ImVec2(org.x + fs * 0.3f, y + (ch - gap - ls.y) * 0.5f), ImGui::GetColorU32(ImGuiCol_Text), label.c_str());
		if (row_hover) {
			if (r >= 4)
				hint("%s\n%s。この行の 6 つのマスが、この操作子で動かす量。見出しの上でマウスホイールを回すと CC の番号が変わる",
				     official_name((std::string("part.") + SRCS[r].key + "_cc").c_str()).c_str(), SRCS[r].about);
			else
				hint("%s\nこの行の 6 つのマスが、この操作子で動かす量", SRCS[r].about);
		}

		for (int c = 0; c < 6; c++) {
			const std::string key = std::string("part.") + SRCS[r].key + "_" + DSTS[c].key;
			const xg::param &p = P(key.c_str());
			int v = p.def;
			const bool known = m.get(p, part, v);
			const float x = org.x + head_w + cw * float(c);
			ImGui::SetCursorScreenPos(ImVec2(x, y));
			ImGui::PushID(c);
			ImGui::InvisibleButton("##cell", ImVec2(cw - gap, ch - gap));
			const ImGuiID id = ImGui::GetItemID();
			const bool hov = ImGui::IsItemHovered(), act = ImGui::IsItemActive();
			if (known) {
				int nv = v;
				if (ImGui::IsItemActivated()) {
					st->SetInt(id, v);
					st->SetFloat(id + 1, io.MousePos.y);
				}
				if (act && !ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
					nv = st->GetInt(id, v) + int((st->GetFloat(id + 1, io.MousePos.y) - io.MousePos.y) / 2.0f);
				if (hov) {
					ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);
					if (io.MouseWheel != 0.0f)
						nv = v + (io.MouseWheel > 0 ? 1 : -1) * (io.KeyCtrl ? 10 : 1);
					if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
						nv = p.def;
				}
				nv = std::clamp(nv, p.min, p.max);
				if (nv != v) {
					drag_send(br, m.set(p, part, nv));
					v = nv;
				}
			}
			// 描く
			const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
			const bool bip = p.how == xg::view::center;
			float t = 0.0f;
			ImU32 fill;
			if (bip) {
				const float span = float(v >= p.center ? p.max - p.center : p.center - p.min);
				t = span > 0 ? float(v - p.center) / span : 0.0f;
				fill = t >= 0 ? IM_COL32(70, 140, 235, int(40 + 190 * t)) : IM_COL32(230, 80, 70, int(40 - 190 * t));
			} else {
				t = float(v - p.min) / float(std::max(1, p.max - p.min));
				fill = IM_COL32(235, 160, 50, int(30 + 200 * t));
			}
			dl->AddRectFilled(a, b, ImGui::GetColorU32(ImGuiCol_FrameBg), 3.0f);
			if ((bip && v != p.center) || (!bip && v != p.min))
				dl->AddRectFilled(a, b, fill, 3.0f);
			const bool changed = known && v != p.def;
			dl->AddRect(a, b, changed ? IM_COL32(250, 230, 150, 255)
			                          : ImGui::GetColorU32(hov || act ? ImGuiCol_Border : ImGuiCol_BorderShadow), 3.0f,
			            0, changed ? 1.5f : 1.0f);
			const std::string text = known ? xg::format(p, v) : std::string("--");
			const ImVec2 ts = ImGui::CalcTextSize(text.c_str());
			dl->AddText(ImVec2((a.x + b.x - ts.x) * 0.5f, (a.y + b.y - ts.y) * 0.5f),
			            ImGui::GetColorU32(known && ((bip && v != p.center) || (!bip && v != p.min)) ? ImGuiCol_Text : ImGuiCol_TextDisabled),
			            text.c_str());
			if (hov || act) {
				const char *help = help_for(key.c_str());
				const std::string to = c >= 3 ? std::string(DSTS[c].name) + "（" + DSTS[c].sub + "）" : std::string(DSTS[c].name);
				hint("%s  %s\n%s → %s。%s（上下にドラッグ・マウスホイール・ダブルクリックで既定の %s）",
				     official_name(key.c_str()).c_str(), text.c_str(), SRCS[r].name, to.c_str(), help ? help : "",
				     xg::format(p, p.def).c_str());
			}
			ImGui::PopID();
		}
		ImGui::PopID();
	}
	ImGui::SetCursorScreenPos(ImVec2(org.x, org.y + head_h + ch * 6.0f));
	ImGui::Dummy(ImVec2(w, 0));
}

// ---- エフェクトの区画（「形」のタブの EG の右。横に送って見る）
//
// slot は設定の窓（fx_editor）と同じ番号: 1-4 がインサーション、5 がリバーブ、6 がコーラス、7 がバリエーション

const char *fx_prefix(int slot)
{
	static const char *const P4[4] = { "insertion1", "insertion2", "insertion3", "insertion4" };
	return slot <= 4 ? P4[std::clamp(slot, 1, 4) - 1] : slot == 5 ? "reverb" : slot == 6 ? "chorus" : "variation";
}

// エフェクトのパラメータの値の字（fx_editor と同じ書き方）
std::string fx_value_text(const xg::fx_param &p, int v)
{
	char buf[24];
	switch (p.fmt) {
	case xg::fx_fmt::table:
		if (p.texts && v >= p.lo && v <= p.hi)
			return p.texts[v - p.lo];
		break;
	case xg::fx_fmt::tenths:
		std::snprintf(buf, sizeof(buf), "%.1f", v / 10.0);
		return buf;
	default:
		break;
	}
	std::snprintf(buf, sizeof(buf), "%d", v);
	return buf;
}

int get_value(xg::model &m, const std::string &key, int part = 0)
{
	const xg::param &p = P(key.c_str());
	int v = p.def;
	m.get(p, part, v);
	return v;
}

// パラメータの番地。インサーションは表のまま、システムエフェクトは xg/sysfx.h で読み替える（fx_editor::where と同じ）
bool fx_where(int slot, const xg::fx_param &fp, u32 &addr, int &size)
{
	if (slot <= 4) {
		addr = xg::pack(0x03, u8(slot - 1), fp.addr);
		size = fp.size;
		return true;
	}
	const xg::sysfx which = slot == 5 ? xg::sysfx::reverb : slot == 6 ? xg::sysfx::chorus : xg::sysfx::variation;
	const int lo = xg::sysfx_addr(which, fp, size);
	if (lo < 0)
		return false;
	addr = xg::pack(0x02, 0x01, u8(lo));
	return true;
}

// 音源のエフェクトの入口・出口の番号（mu2000::scope_fx）
int scope_fx_of(int slot)
{
	return slot <= 4 ? mu2000::SCOPE_INS1 + (slot - 1) : slot == 5 ? mu2000::SCOPE_REV : slot == 6 ? mu2000::SCOPE_CHO : mu2000::SCOPE_VAR;
}

// エフェクト 1 つ（メゾネット）。上の段に種類の名前と、通したあと（緑）・通す前（灰）のスペクトラム。
// 下の段に種類の選択、設定の窓を開くボタン、つまみ（システムエフェクトは戻りとパンを先に）。
// part_only は、このパートだけの音か（インサーション・インサーション接続のバリエーション）、
// 全パートの送りを混ぜた音か（システムのリバーブ・コーラス・バリエーション）。
// バリエーション（slot 7）は下の段の頭に「このパートのインサーションにする」のチェックを置き、
// 入っていなければこのパートの送り（Var Send）だけを触れるようにする（種類やパラメータは見るだけ）
void fx_cell(int slot, bool part_only, int part, xg::model &m, bridge &br, float w, float h)
{
	const bool is_var = slot == 7;
	const bool var_sys = get_value(m, "variation.connect") == 1;
	const int var_part = get_value(m, "variation.part");
	const bool var_mine = !var_sys && var_part == part;     // このパートのインサーションになっている
	const bool locked = is_var && !var_mine;
	const float fs = ImGui::GetFontSize();
	ImDrawList *dl = ImGui::GetWindowDrawList();
	const ImVec2 pos = ImGui::GetCursorScreenPos();
	const float pad = fs * 0.25f;
	const float split = pos.y + h * overview::MAISON_SPLIT;
	const std::string prefix = fx_prefix(slot);
	const xg::param &ptype = P((prefix + ".type").c_str());
	int type = -1;
	if (!m.get(ptype, 0, type))
		type = -1;
	const int msb = type >= 0 ? type >> 7 : 0;
	dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h), ImGui::GetColorU32(ImGuiCol_FrameBg), 3.0f);

	// ---- 上の段
	const float line = ImGui::GetTextLineHeight();
	float y = pos.y + pad;
	if (type >= 0)
		fx_icon(dl, ImVec2(pos.x + pad, y), line, msb, IM_COL32(250, 250, 240, 230));
	const std::string name = type >= 0 ? xg::fx_name(type) : std::string("--");
	dl->AddText(ImVec2(pos.x + pad + line * 1.3f, y), ImGui::GetColorU32(ImGuiCol_Text), name.c_str());
	if (is_var) {
		// 右から [PART] [INS] と、掛かっているパートの字。INS はインサーション接続（切ればシステム接続）、
		// PART はこのパートに掛ける（切れば OFF）。PART はインサーション接続のときだけ意味があるので、そのときだけ触れる
		ImGui::PushFont(nullptr, fs * 0.7f);
		const float sfs = ImGui::GetFontSize();
		float rx = pos.x + w - pad;
		auto toggle = [&](const char *id, const char *text, bool on, bool enabled) {
			const ImVec2 ts = ImGui::CalcTextSize(text);
			const ImVec2 sz(ts.x + sfs * 0.8f, line * 0.9f);
			rx -= sz.x;
			const ImVec2 a(rx, y + (line - sz.y) * 0.5f), b(a.x + sz.x, a.y + sz.y);
			rx -= sfs * 0.3f;
			ImGui::SetCursorScreenPos(a);
			ImGui::BeginDisabled(!enabled);
			const bool clicked = ImGui::InvisibleButton(id, sz);
			const bool hov = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
			ImGui::EndDisabled();
			const ImU32 fill = on ? (enabled ? IM_COL32(60, 150, 230, 255) : IM_COL32(50, 80, 110, 255))
			                      : (hov && enabled ? IM_COL32(70, 76, 90, 255) : IM_COL32(40, 44, 54, 255));
			dl->AddRectFilled(a, b, fill, 3.0f);
			dl->AddRect(a, b, on ? IM_COL32(150, 200, 255, enabled ? 255 : 120) : IM_COL32(120, 125, 140, enabled ? 200 : 90), 3.0f);
			dl->AddText(ImVec2(a.x + (sz.x - ts.x) * 0.5f, a.y + (sz.y - ts.y) * 0.5f),
			            on ? IM_COL32(255, 255, 255, enabled ? 255 : 150) : IM_COL32(200, 200, 210, enabled ? 220 : 110), text);
			return std::make_pair(clicked && enabled, hov);
		};
		const auto part_sw = toggle("##vpart", "PART", !var_sys && var_part == part, !var_sys);
		if (part_sw.first)
			br.send(m.set(P("variation.part"), 0, var_part == part ? 127 : part));
		if (part_sw.second)
			hint("%s\nオンでバリエーションをこのパートに掛ける（切るとどのパートにも掛けない）。インサーション接続（INS）のときだけ"
			     "意味があり、触れる。INS と PART の両方が入っていれば、この区画で種類とパラメータを触れる",
			     official_name("variation.part").c_str());
		const auto ins_sw = toggle("##vins", "INS", !var_sys, true);
		if (ins_sw.first)
			br.send(m.set(P("variation.connect"), 0, var_sys ? 0 : 1));
		if (ins_sw.second)
			hint("%s\nオンでインサーション接続（掛けたパートの音を丸ごと通してから、乾いた音とリバーブ・コーラスへの送りに"
			     "分かれる）、オフでシステム接続（全パートの Var Send を集めて掛け、戻りで混ぜる）。バリエーションは 1 つしか"
			     "無いので、ほかのパートとは取り合いになる。INS と PART の両方が入っていない間は、この区画ではこのパートの "
			     "Send だけを触れる", official_name("variation.connect").c_str());
		// 掛かっているパート
		std::string cap;
		if (var_sys)
			cap = "SYSTEM（全パートの Send）";
		else if (var_part < XG_PARTS + 2)
			cap = "→ " + part_name(var_part);
		else
			cap = "→ OFF";
		const ImVec2 cs = ImGui::CalcTextSize(cap.c_str());
		const ImU32 cc = var_mine ? IM_COL32(150, 230, 190, 255) : IM_COL32(200, 200, 210, 200);
		dl->AddText(ImVec2(rx - cs.x, y + (line - cs.y) * 0.5f), cc, cap.c_str());
		ImGui::PopFont();
	}
	y += line * 1.25f;
	const int fx = scope_fx_of(slot);
	std::string label = part_only ? "緑: 通したあと  灰: 通す前（このパートだけ）" : "緑: 出口  灰: 入口（全パートの送りを混ぜた音）";
	if (is_var && !var_sys && !var_mine)
		label = var_part < XG_PARTS + 2 ? "ほかのパート（" + part_name(var_part) + "）のインサーション。このパートの Var Send は効かない"
		                                : std::string("インサーション接続で、どのパートにも掛かっていない。このパートの Var Send は効かない");
	overview::spectrum_view(br, part, bridge::scope_src(fx, true), bridge::scope_src(fx, false), 10 + slot,
	                        ImVec2(pos.x + pad, y), ImVec2(pos.x + w - pad, split - pad), label.c_str());
	dl->AddLine(ImVec2(pos.x, split), ImVec2(pos.x + w, split), ImGui::GetColorU32(ImGuiCol_Border), 1.0f);

	// ---- 下の段: 種類と、設定の窓
	ImGui::SetCursorScreenPos(ImVec2(pos.x + pad, split + pad));
	ImGui::BeginDisabled(locked);
	const std::vector<xg::fx_type> &types = slot == 5 ? xg::rev_types() : slot == 6 ? xg::cho_types() : xg::ins_types();
	ImGui::SetNextItemWidth(std::min(fs * 11.0f, w * 0.62f));
	if (begin_fx_combo("##type", type, ImGuiComboFlags_HeightLarge)) {
		int chosen = 0;
		if (fx_type_menu(types, type, chosen)) {
			br.send(m.set(ptype, 0, chosen));
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndCombo();
	}
	if (ImGui::IsItemHovered()) {
		const char *th = type >= 0 ? fx_type_help(msb, type & 0x7f) : nullptr;
		hint("%s  %s\n%s", official_name((prefix + ".type").c_str()).c_str(), name.c_str(), th ? th : "エフェクトの種類");
	}
	ImGui::SameLine();
	if (ImGui::SmallButton("詳しく"))
		request_fx(slot);
	if (ImGui::IsItemHovered())
		hint("エフェクトの設定の窓\nこのエフェクトを大きなつまみと説明で触る窓を開く");
	ImGui::EndDisabled();

	// ---- つまみ。入る大きさまで縮める
	struct knob_item { const xg::fx_param *fp; const xg::param *mp; const char *label; bool lock; };
	std::vector<knob_item> items;
	if (locked)
		items.push_back({ nullptr, &P("part.variation_send"), "Send", false });
	if (slot >= 5 && !part_only) {
		items.push_back({ nullptr, &P((prefix + ".return").c_str()), "Return", locked });
		items.push_back({ nullptr, &P((prefix + ".pan").c_str()), "Pan", locked });
	}
	const xg::fx_def *def = type >= 0 ? xg::fx_find(type) : nullptr;
	if (def)
		for (int i = 0; i < def->count; i++) {
			u32 a = 0;
			int sz = 0;
			if (fx_where(slot, def->params[i], a, sz))
				items.push_back({ &def->params[i], nullptr, def->params[i].label, locked });
		}
	const float kx0 = pos.x + pad, kx1 = pos.x + w - pad;
	const float ky0 = ImGui::GetCursorScreenPos().y + pad, ky1 = pos.y + h - pad;
	ImGui::PushFont(nullptr, fs * 0.7f);
	const float kfs = ImGui::GetFontSize();
	const int n = int(items.size());
	float ksize = kfs * 3.4f, cw = 0, ch = 0;
	int per_row = 1;
	float label_w = 0;
	for (const knob_item &it : items)
		label_w = std::max(label_w, ImGui::CalcTextSize(it.label).x);
	for (;; ksize -= kfs * 0.2f) {
		cw = std::max(ksize + kfs * 1.9f, label_w + kfs * 0.8f);
		ch = ksize + kfs * 2.8f;
		per_row = std::max(1, int((kx1 - kx0) / cw));
		const int rows = (n + per_row - 1) / per_row;
		if (float(rows) * ch <= ky1 - ky0 || ksize <= kfs * 1.4f)
			break;
	}
	for (int k = 0; k < n; k++) {
		const knob_item &it = items[size_t(k)];
		ImGui::SetCursorScreenPos(ImVec2(kx0 + float(k % per_row) * cw, ky0 + float(k / per_row) * ch));
		char id[8];
		std::snprintf(id, sizeof(id), "k%d", k);
		const ImVec2 cell0 = ImGui::GetCursorScreenPos();
		ImGui::BeginDisabled(it.lock);
		if (it.mp) {
			const int pp = it.mp->where == xg::area::part ? part : 0;
			int v = it.mp->def;
			const bool known = m.get(*it.mp, pp, v);
			const std::string text = known ? xg::format(*it.mp, v) : std::string("--");
			if (fx_editor::knob(id, v, it.mp->min, it.mp->max, ksize, it.label, text.c_str(), false) && known)
				drag_send(br, m.set(*it.mp, pp, v));
			if (it.lock && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
				hint("%s  %s\nこのパートのインサーションにしていないので、ここでは見るだけ（上の INS と PART を両方入れると触れる）",
				     official_name(it.mp->key).c_str(), text.c_str());
			else if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
				const char *help = help_for(it.mp->key);
				hint("%s  %s\n%s（上下にドラッグ・ホイール・ダブルクリックで数を打つ）", official_name(it.mp->key).c_str(), text.c_str(),
				     help ? help : "");
			}
		} else {
			u32 addr = 0;
			int size = 0;
			fx_where(slot, *it.fp, addr, size);
			int v = 0;
			const bool known = m.get_raw(addr, size, v);
			v = std::clamp(v, int(it.fp->lo), int(it.fp->hi));
			const std::string text = known ? fx_value_text(*it.fp, v) : std::string("--");
			if (fx_editor::knob(id, v, it.fp->lo, it.fp->hi, ksize, it.fp->label, text.c_str(), false) && known)
				drag_send(br, m.set_raw(addr, size, v));
			if (it.lock && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
				hint("%s  %s\nこのパートのインサーションにしていないので、ここでは見るだけ（上の INS と PART を両方入れると触れる）",
				     it.fp->label, text.c_str());
			else if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
				const char *help = fx_param_help(it.fp->label);
				hint("%s  %s\n%s（上下にドラッグ・ホイール・ダブルクリックで数を打つ）", it.fp->label, text.c_str(),
				     help ? help : "（まだ説明が無い）");
			}
		}
		ImGui::EndDisabled();
		// 触れないつまみは暗く
		if (it.lock)
			dl->AddRectFilled(cell0, ImVec2(cell0.x + cw, cell0.y + ch), IM_COL32(16, 20, 28, 150), 4.0f);
	}
	if (!def || def->count == 0) {
		ImGui::SetCursorScreenPos(ImVec2(kx0, ky0 + (n ? float((n + per_row - 1) / per_row) * ch : 0.0f)));
		ImGui::TextDisabled("%s", msb == 0 ? "NO EFFECT（種類を選ぶと、ここにつまみが並ぶ）"
		                          : msb == 0x40 ? "THRU（パラメータは無い）" : "この種類のパラメータの表はまだ無い");
	}
	ImGui::PopFont();
	ImGui::SetCursorScreenPos(pos);
	ImGui::Dummy(ImVec2(w, h));
}

// ---- つなぎの区画（メゾネット）。上の段に流れの絵、下の段に送りのフェーダーと、つなぎ方の型。
//
// XG のシステムエフェクトは、並びが「バリエーション → コーラス → リバーブ」に決まっていて、
// 前から後ろへの送り（VAR→CHO・VAR→REV・CHO→REV）の量だけを変えられる（後ろから前へは送れない）。
// だから「順序」は、この 3 本を開けるか閉じるかで選ぶ（並列・直列など）
void route_cell(int part, xg::model &m, bridge &br, float w, float h)
{
	ImGuiIO &io = ImGui::GetIO();
	const float fs = ImGui::GetFontSize();
	ImDrawList *dl = ImGui::GetWindowDrawList();
	const ImVec2 pos = ImGui::GetCursorScreenPos();
	const float pad = fs * 0.25f;
	const float split = pos.y + h * overview::MAISON_SPLIT;
	dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h), ImGui::GetColorU32(ImGuiCol_FrameBg), 3.0f);
	const bool var_sys = get_value(m, "variation.connect") == 1;
	const int var_part = get_value(m, "variation.part");

	// ---- 流れの絵。節は 横一列に パート・VAR・CHO・REV・出力
	const float x0 = pos.x + fs * 1.8f, x1 = pos.x + w - fs * 1.8f;
	const float ny = (pos.y + split) * 0.5f;
	const float node_w = std::max(fs * 2.8f, ImGui::CalcTextSize("パート").x + fs * 0.6f), node_h = fs * 1.5f;
	auto nx = [&](int i) { return x0 + (x1 - x0) * float(i) / 4.0f; };
	struct edge { int from, to; const char *key; bool part_param; int side; };   // side: 0 は一列の上、-1 は上の弧、1 は下の弧
	static const edge EDGES[] = {
		{ 0, 1, "part.variation_send", true, 0 },  { 1, 2, "variation.to_chorus", false, 0 },
		{ 2, 3, "chorus.to_reverb", false, 0 },    { 3, 4, "reverb.return", false, 0 },
		{ 0, 2, "part.chorus_send", true, -1 },    { 0, 3, "part.reverb_send", true, -1 },
		{ 0, 4, "part.dry_level", true, -1 },
		{ 1, 3, "variation.to_reverb", false, 1 }, { 1, 4, "variation.return", false, 1 },
		{ 2, 4, "chorus.return", false, 1 },
	};
	// 弧の高さの刻み。いちばん長い弧（上は 4 つ先、下は 3 つ先）の札が枠に入るように
	const float room = ny - pos.y - node_h * 0.5f - fs * 1.2f;
	const float step_up = std::max(fs * 0.4f, room / 3.8f), step_down = std::max(fs * 0.4f, room / 2.8f);
	ImGuiStorage *st = ImGui::GetStateStorage();
	for (int e = 0; e < int(sizeof(EDGES) / sizeof(EDGES[0])); e++) {
		const edge &E = EDGES[e];
		const xg::param &p = P(E.key);
		int v = p.def;
		const bool known = m.get(p, E.part_param ? part : 0, v);
		// バリエーションがインサーション接続なら、パートからの送りは効かない（掛けたパートにはつながって見せる）
		const bool var_ins_edge = E.from == 0 && E.to == 1 && !var_sys;
		const float span = float(E.to - E.from);
		const float xa = nx(E.from) + node_w * 0.5f, xb = nx(E.to) - node_w * 0.5f;
		const float hh = E.side == 0 ? 0.0f : (E.side < 0 ? step_up : step_down) * (span - 1.0f + 0.8f) * float(E.side);
		const float ya = ny + (E.side == 0 ? 0.0f : float(E.side) * node_h * 0.5f);
		const ImVec2 A(E.side == 0 ? xa : nx(E.from), ya), B(E.side == 0 ? xb : nx(E.to), ya);
		const ImVec2 C1(A.x, ya + hh * 1.33f), C2(B.x, ya + hh * 1.33f);
		const float t = var_ins_edge ? (var_part == part ? 1.0f : 0.0f) : float(v) / 127.0f;
		const ImU32 lc = t > 0.0f ? IM_COL32(140, 240, 190, int(70 + 185 * t)) : IM_COL32(200, 200, 210, 45);
		const float thick = t > 0.0f ? 1.0f + 3.0f * t : 1.0f;
		if (E.side == 0)
			dl->AddLine(A, B, lc, thick);
		else
			dl->AddBezierCubic(A, C1, C2, B, lc, thick);
		// 矢じり
		const ImVec2 tip = B;
		const float ah = fs * 0.35f;
		if (E.side == 0)
			dl->AddTriangleFilled(tip, ImVec2(tip.x - ah, tip.y - ah * 0.6f), ImVec2(tip.x - ah, tip.y + ah * 0.6f), lc);
		else
			dl->AddTriangleFilled(tip, ImVec2(tip.x - ah * 0.6f, tip.y + float(E.side) * ah), ImVec2(tip.x + ah * 0.6f, tip.y + float(E.side) * ah), lc);
		// 値の札（弧の頂か線の真ん中）。上下にドラッグ・ホイール・ダブルクリックで 0 と既定を行き来
		const ImVec2 mid = E.side == 0 ? ImVec2((A.x + B.x) * 0.5f, A.y) : ImVec2((A.x + B.x) * 0.5f, ya + hh);
		char text[16];
		if (var_ins_edge)
			std::snprintf(text, sizeof(text), "%s", var_part == part ? "INS" : "--");
		else if (known)
			std::snprintf(text, sizeof(text), "%d", v);
		else
			std::snprintf(text, sizeof(text), "--");
		const float tfs = fs * 0.62f;
		const ImVec2 ts = ImGui::GetFont()->CalcTextSizeA(tfs, FLT_MAX, 0.0f, text);
		const ImVec2 r0(mid.x - ts.x * 0.5f - fs * 0.25f, mid.y - ts.y * 0.5f - 1.0f), r1(mid.x + ts.x * 0.5f + fs * 0.25f, mid.y + ts.y * 0.5f + 1.0f);
		ImGui::SetCursorScreenPos(r0);
		ImGui::PushID(e);
		ImGui::InvisibleButton("##edge", ImVec2(r1.x - r0.x, r1.y - r0.y));
		const ImGuiID id = ImGui::GetItemID();
		const bool hov = ImGui::IsItemHovered(), act = ImGui::IsItemActive();
		if (known && !var_ins_edge) {
			int nv = v;
			if (ImGui::IsItemActivated()) {
				st->SetInt(id, v);
				st->SetFloat(id + 1, io.MousePos.y);
			}
			if (act && !ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
				nv = st->GetInt(id, v) + int((st->GetFloat(id + 1, io.MousePos.y) - io.MousePos.y) / 2.0f);
			if (hov) {
				ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);
				if (io.MouseWheel != 0.0f)
					nv = v + (io.MouseWheel > 0 ? 1 : -1) * (io.KeyCtrl ? 10 : 1);
				if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
					nv = v ? 0 : (p.def ? p.def : 64);
			}
			nv = std::clamp(nv, p.min, p.max);
			if (nv != v)
				drag_send(br, m.set(p, E.part_param ? part : 0, nv));
		}
		dl->AddRectFilled(r0, r1, hov || act ? IM_COL32(60, 70, 64, 255) : IM_COL32(20, 24, 22, 230), 4.0f);
		dl->AddRect(r0, r1, lc, 4.0f);
		dl->AddText(ImGui::GetFont(), tfs, ImVec2(mid.x - ts.x * 0.5f, mid.y - ts.y * 0.5f), t > 0.0f ? IM_COL32(210, 255, 225, 255) : IM_COL32(200, 200, 210, 150), text);
		if (hov || act) {
			if (var_ins_edge)
				hint("バリエーションはインサーション接続\n%s。パートからの送り（Var Send）は効かない。バリエーションの区画の「このパートのインサーションにする」で切り替える",
				     var_part == part ? "このパートに直に掛かっている（通したあとが乾いた音と送りに分かれる）"
				                      : "ほかのパートに掛かっていて、このパートの音は通らない");
			else {
				const char *help = help_for(E.key);
				hint("%s  %d\n%s（上下にドラッグ・ホイール・ダブルクリックで 0 と既定を行き来）", official_name(E.key).c_str(), v, help ? help : "");
			}
		}
		ImGui::PopID();
	}
	// 節
	static const char *const NODE[5] = { "パート", "VAR", "CHO", "REV", "出力" };
	static const char *const TYPE_KEY[5] = { nullptr, "variation.type", "chorus.type", "reverb.type", nullptr };
	for (int i = 0; i < 5; i++) {
		const ImVec2 a(nx(i) - node_w * 0.5f, ny - node_h * 0.5f), b(nx(i) + node_w * 0.5f, ny + node_h * 0.5f);
		bool on = true;
		std::string sub;
		if (TYPE_KEY[i]) {
			const int ty = get_value(m, TYPE_KEY[i]);
			on = (ty >> 7) != 0;
			sub = on ? xg::fx_name(ty) : std::string("OFF");
			if (i == 1 && !var_sys)
				sub = "INS 接続";
		}
		dl->AddRectFilled(a, b, on ? IM_COL32(44, 70, 58, 255) : IM_COL32(40, 40, 44, 255), 5.0f);
		dl->AddRect(a, b, on ? IM_COL32(140, 240, 190, 200) : IM_COL32(120, 120, 130, 160), 5.0f, 0, 1.5f);
		const ImVec2 ts = ImGui::CalcTextSize(NODE[i]);
		dl->AddText(ImVec2(nx(i) - ts.x * 0.5f, ny - ts.y * 0.5f), on ? ImGui::GetColorU32(ImGuiCol_Text) : ImGui::GetColorU32(ImGuiCol_TextDisabled), NODE[i]);
		if (!sub.empty()) {
			const float sfs = fs * 0.55f;
			const ImVec2 ss = ImGui::GetFont()->CalcTextSizeA(sfs, FLT_MAX, 0.0f, sub.c_str());
			dl->AddText(ImGui::GetFont(), sfs, ImVec2(nx(i) - ss.x * 0.5f, b.y + 1.0f), ImGui::GetColorU32(ImGuiCol_TextDisabled), sub.c_str());
		}
	}
	dl->AddLine(ImVec2(pos.x, split), ImVec2(pos.x + w, split), ImGui::GetColorU32(ImGuiCol_Border), 1.0f);

	// ---- 下の段: つなぎ方の型（前から後ろへの 3 本だけを変える）と、バリエーションの接続
	ImGui::SetCursorScreenPos(ImVec2(pos.x + pad, split + pad));
	const int vc = get_value(m, "variation.to_chorus"), vr = get_value(m, "variation.to_reverb"), cr = get_value(m, "chorus.to_reverb");
	struct preset { const char *name, *about; int vc, vr, cr; };
	static const preset PRESETS[] = {
		{ "並列", "3 つを別々に鳴らす（前から後ろへの送りを全部 0）", 0, 0, 0 },
		{ "VAR→CHO→REV", "バリエーションの出口をコーラスへ、コーラスの出口をリバーブへ（直列）", 127, 0, 127 },
		{ "VAR→REV", "バリエーションの出口だけをリバーブへ", 0, 127, 0 },
		{ "CHO→REV", "コーラスの出口だけをリバーブへ", 0, 0, 127 },
	};
	ImGui::PushFont(nullptr, fs * 0.8f);
	ImGui::TextDisabled("つなぎ方");
	for (const preset &pr : PRESETS) {
		// 入らなければ次の行へ
		const float bw = ImGui::CalcTextSize(pr.name).x + ImGui::GetStyle().FramePadding.x * 2.0f;
		ImGui::SameLine();
		if (ImGui::GetCursorScreenPos().x + bw > pos.x + w - pad)
			ImGui::NewLine();
		const bool cur = (vc > 0) == (pr.vc > 0) && (vr > 0) == (pr.vr > 0) && (cr > 0) == (pr.cr > 0);
		if (cur)
			ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetColorU32(ImGuiCol_ButtonActive));
		if (ImGui::SmallButton(pr.name)) {
			br.send(m.set(P("variation.to_chorus"), 0, pr.vc));
			br.send(m.set(P("variation.to_reverb"), 0, pr.vr));
			br.send(m.set(P("chorus.to_reverb"), 0, pr.cr));
		}
		if (cur)
			ImGui::PopStyleColor();
		if (ImGui::IsItemHovered())
			hint("つなぎ方: %s\n%s。XG の並びは VAR → CHO → REV に決まっていて、後ろから前へは送れない。"
			     "パートからの送りと戻りはそのまま", pr.name, pr.about);
	}
	ImGui::PopFont();
	// 送りのフェーダー。左の 4 本がこのパートから、右の 3 本がエフェクトからエフェクトへ
	static const char *const KEYS[7] = { "part.dry_level", "part.variation_send", "part.chorus_send", "part.reverb_send",
	                                     "variation.to_chorus", "variation.to_reverb", "chorus.to_reverb" };
	static const char *const NAMES[7] = { "Dry", "Var", "Cho", "Rev", "V>C", "V>R", "C>R" };
	const ImVec2 fa = ImGui::GetCursorScreenPos();
	overview::fader_strip("##sends", KEYS, NAMES, 7, 3, part, m, br, ImVec2(w - pad * 2.0f, std::max(fs * 3.0f, pos.y + h - pad - fa.y)));
	ImGui::SetCursorScreenPos(pos);
	ImGui::Dummy(ImVec2(w, h));
}

} // namespace


void part_shapes::draw(xg::model &m, const xg_snapshot &ram, bridge &br)
{
	set_current_ram(&ram);            // 絵が音色の中身を読むため（ピッチ EG など）
	begin_hint_bar();                 // 絵や名前の説明は、マウスのそばでなく下の帯に出す
	const ImGuiViewport *vp = ImGui::GetMainViewport();
	ImGui::SetNextWindowPos(vp->WorkPos);
	ImGui::SetNextWindowSize(vp->WorkSize);
	const ImGuiWindowFlags wf = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
	                            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus;
	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
	ImGui::Begin("part_shapes", nullptr, wf);
	ImGui::PopStyleVar();

	// 表示の大きさ。文字も絵も同じ倍率で縮む（editor.ini に覚える）
	float &zoom = shapes_zoom();
	ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * zoom);

	const float fs = ImGui::GetFontSize();
	int part = shape_window_part();
	int scope = -1;                   // パートの音を拾うか（形のタブのフィルタが絵のときだけ）

	// ---- パートを選ぶ。音色の名前も出す
	ImGui::SetNextItemWidth(fs * 5);
	if (ImGui::BeginCombo("##part", part_name(part).c_str(), ImGuiComboFlags_HeightLarge)) {
		for (int p = 0; p < PARTS; p++) {
			if (p && p % 16 == 0)
				ImGui::Separator();          // 口の境目
			if (ImGui::Selectable(part_name(p).c_str(), p == part))
				set_shape_window_part(part = p);
			if (p == part && ImGui::IsWindowAppearing())
				ImGui::SetScrollHereY();
		}
		ImGui::EndCombo();
	}
	ImGui::SameLine();
	if (ImGui::ArrowButton("##prev", ImGuiDir_Left))
		set_shape_window_part(part = (part + PARTS - 1) % PARTS);
	ImGui::SameLine();
	if (ImGui::ArrowButton("##next", ImGuiDir_Right))
		set_shape_window_part(part = (part + 1) % PARTS);
	ImGui::SameLine();
	int msb = 0, lsb = 0, prog = 0;
	std::string voice = "--";
	if (m.get(P("part.bank_msb"), part, msb) && m.get(P("part.bank_lsb"), part, lsb) && m.get(P("part.program"), part, prog)) {
		voice = voice_text(msb, lsb, prog);
		if (const xg::voice_rom *vr = voices()) {
			const std::string real = vr->name(ram.parts[part], msb, prog);
			if (!real.empty()) {
				char buf[48];
				std::snprintf(buf, sizeof(buf), "%3d  %s", prog + 1, real.c_str());
				voice = buf;
			}
		}
	}
	ImGui::TextUnformatted(voice.c_str());

	// 表示の大きさと、説明のチェックボックスは右端へ
	ImGui::SameLine(ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX() - fs * 18);
	if (ImGui::SmallButton("-"))
		set_shapes_zoom(zoom - 0.1f);
	ImGui::SameLine();
	ImGui::Text("%d%%", int(std::lround(zoom * 100)));
	ImGui::SameLine();
	if (ImGui::SmallButton("+"))
		set_shapes_zoom(zoom + 0.1f);
	ImGui::SameLine();
	help_checkbox();

	// ---- 上のペイン: エフェクト、棒、鍵盤
	const ImGuiStyle &st = ImGui::GetStyle();
	{
		// インサーションの行、見出しと棒の行、鍵盤の行
		const float strip_h = overview::part_strip_height() + st.WindowPadding.y * 2.0f + fs * 0.2f;
		if (ImGui::BeginChild("strip", ImVec2(0, strip_h), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar))
			m_strip.part_strip(part, m, ram, br);
		ImGui::EndChild();
	}

	// ---- 左に音色を選ぶ面、右はタブ: 「形」は 4 つの区画（2 × 2）、「すべて」はパートのパラメータ全部
	const ImVec2 avail = ImGui::GetContentRegionAvail();
	// 音色を選ぶ面は、左に分類・右に音色とバンク違いの 2 列（xgui::program_pane）
	const float pane_w = std::min(fs * 15.6f, avail.x * 0.3f);     // 前の 6 割
	// 下の説明の帯（小さめの字で 3 行）。「説明を出す」を切っていれば帯ごと出さず、その高さを絵に回す
	const bool show_bar = help_on();
	ImGui::PushFont(nullptr, fs * BAR_SCALE);
	const float bar_h = show_bar ? ImGui::GetTextLineHeightWithSpacing() * 3.0f + st.WindowPadding.y * 2.0f + st.ItemSpacing.y : 0.0f;
	ImGui::PopFont();
	const float body_h = std::max(fs * 8.0f, avail.y - bar_h);

	if (ImGui::BeginChild("voicepane", ImVec2(pane_w, body_h)))
	{
		ImGui::PushFont(nullptr, fs * 0.85f);   // 分類・音色・バンク違いの 3 つは小さめの字で
		program_pane(part, m, &ram, br);
		ImGui::PopFont();
	}
	ImGui::EndChild();
	ImGui::SameLine();

	ImGui::BeginGroup();
	const float top_y = ImGui::GetCursorScreenPos().y;
	if (ImGui::BeginTabBar("right")) {
		if (ImGui::BeginTabItem("形")) {
			scope = part;
			// 列を音の流れの順に横へ並べ、画面に入るのは 3 列ぶん（残りは横に送って見る）。
			// 左に VIB（上）とモジュレーション（下）。フィルタと EQ、EG とピッチ EG、エフェクトは上下 2 段が
			// つながった区画（メゾネット。上の段が絵、下の段がフェーダーやつまみ）。
			// EG の右に、このパートに掛かっているインサーション（とインサーション接続のバリエーション）、
			// つなぎ（送りと順序）、送っているシステムエフェクト（バリエーション・コーラス・リバーブ）。
			// エフェクトの区画は、そのエフェクトがこのパートの音に効いているときだけ出す。ただしバリエーションは
			// いつも出す（このパートのインサーションならインサーションの並びに、でなければつなぎの右に）。
			// ポルタメントは「すべて」のタブにある
			const int var_type = get_value(m, "variation.type"), cho_type = get_value(m, "chorus.type"), rev_type = get_value(m, "reverb.type");
			const bool var_sys = get_value(m, "variation.connect") == 1;
			struct fx_col { int slot; bool part_only; };
			std::vector<fx_col> inline_fx, sys_fx;
			for (int n = 1; n <= 4; n++)
				if (get_value(m, std::string(fx_prefix(n)) + ".part") == part)
					inline_fx.push_back({ n, true });
			if (!var_sys && get_value(m, "variation.part") == part)
				inline_fx.push_back({ 7, true });
			const bool var_on = var_sys && (var_type >> 7) != 0 && get_value(m, "part.variation_send", part) > 0;
			const bool cho_on = (cho_type >> 7) != 0 &&
			                    (get_value(m, "part.chorus_send", part) > 0 || (var_on && get_value(m, "variation.to_chorus") > 0));
			const bool rev_on = (rev_type >> 7) != 0 &&
			                    (get_value(m, "part.reverb_send", part) > 0 || (cho_on && get_value(m, "chorus.to_reverb") > 0) ||
			                     (var_on && get_value(m, "variation.to_reverb") > 0));
			if (!(!var_sys && get_value(m, "variation.part") == part))
				sys_fx.push_back({ 7, false });   // バリエーションはいつも出す
			if (cho_on) sys_fx.push_back({ 6, false });
			if (rev_on) sys_fx.push_back({ 5, false });

			const float room_h = body_h - (ImGui::GetCursorScreenPos().y - top_y);
			const float w = (ImGui::GetContentRegionAvail().x - st.ItemSpacing.x * 2.0f) / 3.0f;
			// 横の送りの棒のぶんを引く（いつもつなぎの列があるので、はみ出す）
			const float h = (room_h - st.ScrollbarSize - st.ItemSpacing.y) * 0.5f;
			const float tall = h * 2.0f + st.ItemSpacing.y;
			ImGui::BeginChild("flow", ImVec2(0, room_h), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
			ImGui::BeginGroup();
			panel("vib", "ビブラート（VIB）", w, h, part, m, br, { "part.vib_rate", "part.vib_depth", "part.vib_delay" }, 0,
			      [](int p, xg::model &mm, bridge &b, float pw, float ph) { overview::vib_cell(p, mm, b, pw, ph, false); },
			      "音色の揺れ（ビブラート）。絵は実際の揺れで、右のフェーダーで速さ（Rate）・深さ（Depth）・"
			      "掛かり始めるまでの時間（Delay）を変える");
			panel("mod", "モジュレーション（MW）", w, h, part, m, br,
			      { "part.mw_lfo_pmod", "part.mw_pitch", "part.mw_filter", "part.mw_amp", "part.mw_lfo_fmod", "part.mw_lfo_amod" }, 5,
			      [](int p, xg::model &mm, bridge &b, float pw, float ph) { overview::mod_cell(p, mm, b, pw, ph, false); },
			      "モジュレーションホイールを上げたときに足すビブラート。横がホイールの位置、縦が揺れの深さ。"
			      "左のホイールが CC1、右のホイールが MW LFO PM。音色自身の揺れ（背景の帯）とは足さず、深いほうが効く");
			ImGui::EndGroup();
			ImGui::SameLine();
			panel("filter", "フィルタと EQ（FILTER・EQ）", w, tall, part, m, br,
			      { "part.cutoff", "part.resonance", "part.hpf_cutoff",
			        "part.eq_bass_gain", "part.eq_bass_freq", "part.eq_treble_gain", "part.eq_treble_freq" }, 1,
			      [](int p, xg::model &mm, bridge &b, float pw, float ph) { overview::filter_cell(p, mm, b, pw, ph, false); },
			      "音の明るさ。横は実際の周波数で、緑がこのパートの今の音のスペクトラム。太線がフィルタとパートの EQ を"
			      "合わせた実際の特性（細線がフィルタだけ、点線が EQ だけ）。どちらも声ごとに掛かり、EQ はフィルタのすぐ後ろ"
			      "（インサーションより前）。下のフェーダーで Cutoff・Resonance・HPF と、EQ の低音・高音のゲインと周波数を変える");
			ImGui::SameLine();
			panel("env", "EG とピッチ EG（EG・PEG）", w, tall, part, m, br,
			      { "part.attack", "part.decay", "part.release",
			        "part.peg_init_level", "part.peg_attack_time", "part.peg_rel_level", "part.peg_rel_time" }, 2,
			      [](int p, xg::model &mm, bridge &b, float pw, float ph) { overview::env_cell(p, mm, b, pw, ph); },
			      "音量の形（青。立ち上がり → 落ち着き → 伸ばし → 離して消える）と音程の動き（橙）を、同じ実際の時間の目盛り・"
			      "同じ離す時刻で 1 枚に。縦は左が音量（dB）、右が音程（セント）。下のフェーダーで EG の Attack・Decay・Release と"
			      "ピッチ EG の Init・Attack・Rel Lv・Rel Tm を変える。緑の背景は EG を通したあとの音のスペクトラム（横は周波数。"
			      "インサーションの前）");
			// エフェクトの列
			auto fx_panel = [&](const fx_col &c) {
				ImGui::SameLine();
				char id[16], title[64];
				std::snprintf(id, sizeof(id), "fx%d", c.slot);
				if (c.slot <= 4)
					std::snprintf(title, sizeof(title), "インサーション %d（INS %d）", c.slot, c.slot);
				else if (c.slot == 7)
					std::snprintf(title, sizeof(title), "%s", c.part_only ? "バリエーション（VAR・インサーション接続）" : "バリエーション（VAR）");
				else
					std::snprintf(title, sizeof(title), "%s", c.slot == 6 ? "コーラス（CHO）" : "リバーブ（REV）");
				const int slot = c.slot;
				const bool only = c.part_only;
				panel(id, title, w, tall, part, m, br, {}, PANEL_FIXED,
				      [slot, only](int p, xg::model &mm, bridge &b, float pw, float ph) { fx_cell(slot, only, p, mm, b, pw, ph); },
				      only ? "このパートに掛かっているエフェクト。上の段は、通したあと（緑）と通す前（灰）のこのパートの音の"
				             "スペクトラム（同じ目盛り）。下の段で種類とパラメータを変える。「詳しく」で設定の窓"
				           : "このパートが送っているシステムエフェクト。上の段は、出口（緑）と入口（灰）のスペクトラムで、"
				             "全パートの送りを混ぜた音。下の段で種類・戻り（Return）・パン・パラメータを変える。「詳しく」で設定の窓");
			};
			for (const fx_col &c : inline_fx)
				fx_panel(c);
			ImGui::SameLine();
			panel("route", "つなぎ（送りと順序）", w, tall, part, m, br, {}, PANEL_FIXED,
			      [](int p, xg::model &mm, bridge &b, float pw, float ph) { route_cell(p, mm, b, pw, ph); },
			      "このパートの音がどのエフェクトを通って出ていくか。上の絵の数字（送りの量）を上下にドラッグ・ホイール・"
			      "ダブルクリックで変える。XG の並びは VAR → CHO → REV に決まっていて、「つなぎ方」で前から後ろへの送りを"
			      "開け閉めして順序（並列・直列）を選ぶ。送りが 0 のエフェクトの区画は出さない。横に送るのは Shift + ホイール");
			for (const fx_col &c : sys_fx)
				fx_panel(c);
			ImGui::EndChild();
			ImGui::EndTabItem();
		}
		if (ImGui::BeginTabItem("マトリクス")) {
			// 操作子 6 つ × 行き先 6 つ（モジュレーションのマトリクス）
			const float room_h = body_h - (ImGui::GetCursorScreenPos().y - top_y);
			if (ImGui::BeginChild("matrix", ImVec2(0, room_h), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar)) {
				const ImVec2 r = ImGui::GetContentRegionAvail();
				mod_matrix(part, m, br, r.x, r.y);
			}
			ImGui::EndChild();
			ImGui::EndTabItem();
		}
		if (ImGui::BeginTabItem("すべて")) {
			// エディタのパートの面と同じ組（xgui::PART_GROUPS）を、幅に合わせた列数で並べる
			if (ImGui::BeginChild("all", ImVec2(0, body_h - (ImGui::GetCursorScreenPos().y - top_y)))) {
				const int columns = std::clamp(int(ImGui::GetContentRegionAvail().x / (fs * 16.0f)), 1, 3);
				if (ImGui::BeginTable("groups", columns, ImGuiTableFlags_SizingStretchSame)) {
					int n = 0;
					for (const part_group &g : PART_GROUPS) {
						if (n++ % columns == 0)
							ImGui::TableNextRow();
						ImGui::TableNextColumn();
						ImGui::SeparatorText(g.title);
						ImGui::PushItemWidth(-fs * 6.0f);
						for (const char *key : g.keys) {
							if (!key)
								break;
							param_slider(key, part, m, br);
						}
						ImGui::PopItemWidth();
						ImGui::Spacing();
					}
					ImGui::EndTable();
				}
			}
			ImGui::EndChild();
			ImGui::EndTabItem();
		}
		ImGui::EndTabBar();
	}
	ImGui::EndGroup();

	// ---- 説明の帯。カーソルを載せた項目の説明（無ければ使い方のひとこと）。「説明を出す」を切れば出さない
	if (show_bar) {
	if (ImGui::BeginChild("hint", ImVec2(0, 0), ImGuiChildFlags_Borders)) {
		ImGui::PushFont(nullptr, fs * BAR_SCALE);
		const std::string &t = hint_text();
		if (t.empty()) {
			ImGui::TextDisabled("項目にカーソルを載せると、ここに説明が出る（「説明を出す」を切るとこの欄は消える）");
		} else {
			// 1 行目（区画や項目の名前）は色を変える
			const size_t nl = t.find('\n');
			ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 214, 120, 255));
			ImGui::TextUnformatted(t.c_str(), t.c_str() + (nl == std::string::npos ? t.size() : nl));
			ImGui::PopStyleColor();
			if (nl != std::string::npos)
				ImGui::TextWrapped("%s", t.c_str() + nl + 1);
		}
		ImGui::PopFont();
	}
	ImGui::EndChild();
	}
	end_hint_bar();
	br.want_scope(scope);

	ImGui::PopFont();
	ImGui::End();
}

} // namespace ui
