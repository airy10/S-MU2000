// license:BSD-3-Clause
//
// パネルの面。**実機の写真から採寸して並べ直した**。
//
// 論理座標の 1000 × 385 が本体の前面ぜんたい（実機の縦横比はおよそ 2.6:1）。
// 残りの 15 は面を切り替える帯で、本体の外。
//
//   左   A/D INPUT のジャックとつまみ、VOLUME、電源、MIDI IN A、PHONES、カード
//   中   LCD、その下に PART / BANK・PGM# / VOL / EXP / PAN / REV / CHO / VAR / KEY
//        の見出しと、音色カテゴリのボタン 18 個
//   右   PLAY EDIT / UTIL EFFECT / SAMPLING SEQ の 6 個（LED 入り）、
//        MUTE PART−+ / ENTER SELECT−+ / EXIT VALUE−+ の 9 個、
//        SELECT と AUDITION、そして**大きなダイヤル**

#include "panel.h"
#include "draw.h"
#include "texts.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <vector>

namespace ui {

namespace {

constexpr double PI = 3.14159265358979;

// **位置と大きさは ui::layout（src/ui/layout.*）が持っている**。
// panel.txt があればそちらで上書きされる。ここに残してあるのは
// 「どのボタンか」「札に何と書くか」だけ

struct place { mu2000::button b; const char *label; const char *sub; };

const mu2000::button CAT_B[18] = {
	mu2000::button::piano,      mu2000::button::chrom_perc, mu2000::button::organ,
	mu2000::button::guitar,     mu2000::button::bass,       mu2000::button::strings,
	mu2000::button::ensemble,   mu2000::button::brass,      mu2000::button::reed,
	mu2000::button::pipe,       mu2000::button::synth_lead, mu2000::button::synth_pad,
	mu2000::button::synth_effects, mu2000::button::ethnic,  mu2000::button::percussive,
	mu2000::button::sfx,        mu2000::button::model_excl, mu2000::button::drum,
};
const char *CAT_LABEL[18] = {
	"Piano", "Chrom. perc.", "Organ", "Guitar", "Bass", "Strings",
	"Ensemble", "Brass", "Reed", "Pipe", "Synth lead", "Synth pad",
	"Synth effects", "Ethnic", "Percussive", "SFX", "Model excl.", "Drum",
};

// LCD の下段に並ぶもの。窓の内側の左端からの割合で置く。
// 窓の下に印刷されている札も、ここから位置を取って揃える
// 位置と幅は、上の面の**点 1 つぶん**を単位にした、窓の内側の左端からの数
// （上の面は 17 桁 × 6 点 − 1 = 101 点）。
//
// 下の面の位置と幅は実機の写真から採寸した（layout.cpp の low.x / low.w）。
// 「01」「A01」の 5 桁は字間が左から 1・2・1・1 点

// n 番のバーの左端。A1 が 0、A2 が 1、パート 1 が 2 …（点の単位）
constexpr int bar_x(int i) { return (i / 2) * (CELL_W + 1) + ((i & 1) ? 3 : 0); }
constexpr int part_x(int n) { return bar_x(n + 1); }

// 実機の窓は、上の面の左に 2.7 点、右に 5.4 点ぶんの余白がある。
// 右の余白にモードの ▶ が入る（写真から採寸。単位は上の面の点の間隔）
constexpr double LCD_LEFT = 2.7, LCD_RIGHT = 5.4;
constexpr double LCD_SPAN = LCD_LEFT + (TOP_COLS * (CELL_W + 1) - 1) + LCD_RIGHT;

// 点と点の隙間。実機は点の間隔の 1 割ほどしかない
constexpr double DOT_GAP = 0.10;

// 右端の ▶ の高さ（下の面の上端から、点の間隔の単位）。
// いちばん上は札のない ▶、残りが XG / GS / PERFORM
constexpr double MODE_Y[4] = { -2.2, 0.6, 3.4, 6.2 };
const char *const MODE_LABEL[3] = { "XG", "GS", "PERFORM" };

COLORREF mix(COLORREF a, COLORREF b, double t)
{
	auto ch = [&](int x, int y) { return int(std::lround(x + (y - x) * t)); };
	return RGB(ch(GetRValue(a), GetRValue(b)), ch(GetGValue(a), GetGValue(b)),
	           ch(GetBValue(a), GetBValue(b)));
}


// 窓の下に印刷されている札。どの並びの真ん中に置くか
struct column { int at; const char *label; };
const column COLUMNS[] = {
	{ LOW_PART, "PART" }, { LOW_ICON, "BANK/PGM#" }, { LOW_VOL, "VOL" },
	{ LOW_EXP,  "EXP"  }, { LOW_PAN,  "PAN" },       { LOW_REV, "REV" },
	{ LOW_CHO,  "CHO"  }, { LOW_VAR,  "VAR" },       { LOW_KEY, "KEY" },
};

// 右上の 6 個。丸い押しボタンで、中に LED が入っている。
// LED の番号は MAME の mulcd.lay の並び（左列 0,2,4 / 右列 1,3,5）
struct mode_button { mu2000::button b; int led; const char *label; };
const mode_button MODES[6] = {
	{ mu2000::button::play,          0, "PLAY"     },
	{ mu2000::button::edit,          1, "EDIT"     },
	{ mu2000::button::util,          2, "UTIL"     },
	{ mu2000::button::effect,        3, "EFFECT"   },
	{ mu2000::button::sampling_mode, 4, "SAMPLING" },
	{ mu2000::button::seq,           5, "SEQ"      },
};

// 右端の 9 個
const place NAV[9] = {
	{ mu2000::button::mute_solo,    "MUTE",   "SOLO" },
	{ mu2000::button::part_minus,   "PART",   "-" },
	{ mu2000::button::part_plus,    "PART",   "+" },
	{ mu2000::button::enter,        "ENTER",  "" },
	{ mu2000::button::select_left,  "SELECT", "-" },
	{ mu2000::button::select_right, "SELECT", "+" },
	{ mu2000::button::exit,         "EXIT",   "" },
	{ mu2000::button::value_minus,  "VALUE",  "-" },
	{ mu2000::button::value_plus,   "VALUE",  "+" },
};

// 音色カテゴリの右にある小さな丸ボタン 2 つ
const place ROUND[2] = {
	{ mu2000::button::select,   "SELECT",   "" },
	{ mu2000::button::audition, "AUDITION", "" },
};

// 実機の色
const COLORREF PANEL_FACE = RGB(196, 189, 170);
const COLORREF PANEL_INK  = RGB(46, 44, 40);
const COLORREF KEY_FACE   = RGB(216, 205, 165);
const COLORREF KEY_EDGE   = RGB(126, 118, 92);
const COLORREF KEY_DOWN   = RGB(150, 140, 95);

} // namespace


panel::panel()
{
	resize(LOGICAL_W, LOGICAL_H);
}

panel::~panel()
{
	if (m_font_label) DeleteObject(m_font_label);
	if (m_font_small) DeleteObject(m_font_small);
	if (m_font_tiny)  DeleteObject(m_font_tiny);
}

RECT panel::scale(double x, double y, double w, double h) const
{
	RECT r;
	r.left   = m_ox + int(std::lround(x * m_scale));
	r.top    = m_oy + int(std::lround(y * m_scale));
	r.right  = m_ox + int(std::lround((x + w) * m_scale));
	r.bottom = m_oy + int(std::lround((y + h) * m_scale));
	return r;
}

// パネルに描いてある MIDI IN A のジャック（丸と札）を囲む枠
RECT panel::midi_jack() const
{
	return scale(92, 230, 96, 90);
}

bool panel::on_midi_jack(int x, int y) const
{
	if (m_page != page::front)
		return false;
	const RECT r = midi_jack();
	return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}

bool panel::on_card_slot(int x, int y) const
{
	if (m_page != page::front)
		return false;
	const RECT r = scale(m_lay.card[0], m_lay.card[1], m_lay.card[2], m_lay.card[3]);
	return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}

bool panel::on_ad_input(int x, int y) const
{
	if (m_page != page::front)
		return false;
	const RECT r = scale(m_lay.adin[0], m_lay.adin[1], m_lay.adin[2], m_lay.adin[3]);
	return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}

bool panel::on_phones(int x, int y) const
{
	if (m_page != page::front)
		return false;
	const RECT r = scale(m_lay.phones[0], m_lay.phones[1], m_lay.phones[2], m_lay.phones[3]);
	return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}

// 論理座標の点を実座標へ
POINT panel::at(double x, double y) const
{
	POINT p;
	p.x = m_ox + int(std::lround(x * m_scale));
	p.y = m_oy + int(std::lround(y * m_scale));
	return p;
}

void panel::resize(int w, int h)
{
	m_w = std::max(w, 200);
	m_h = std::max(h, 60);
	// **帯のぶんを差し引いてから合わせる**。当たりも描きも
	// `scale()` / `at()` を通るので、ここだけ直せば全部ついてくる
	const int body_h = std::max(m_h - m_top_inset, 60);

	if (m_lcd_only) {
		m_scale = std::min(double(m_w) / m_lay.lcd[2], double(body_h) / m_lay.lcd[3]);
		m_ox = int(-m_lay.lcd[0] * m_scale);
		m_oy = m_top_inset - int(m_lay.lcd[1] * m_scale);
	} else {
		m_scale = std::min(double(m_w) / LOGICAL_W, double(body_h) / LOGICAL_H);
		m_ox = int((m_w - LOGICAL_W * m_scale) / 2);
		m_oy = m_top_inset + int((body_h - LOGICAL_H * m_scale) / 2);
	}

	m_lcd    = scale(m_lay.lcd[0], m_lay.lcd[1], m_lay.lcd[2], m_lay.lcd[3]);
	if (m_lcd_only)
		m_lcd = RECT{ 0, m_top_inset, m_w, m_h };
	m_volume = scale(m_lay.volume[0] - m_lay.volume[2], m_lay.volume[1] - m_lay.volume[2],
	                 m_lay.volume[2] * 2, m_lay.volume[2] * 2);   // 当たりは丸で見る
	m_status = scale(20, 372, 700, 13);
	m_hint   = scale(20, 386, 700, 13);
	m_wheel  = scale(m_lay.dial[0] - m_lay.dial[2], m_lay.dial[1] - m_lay.dial[2],
	                 m_lay.dial[2] * 2, m_lay.dial[2] * 2);
	for (int i = 0; i < 6; i++)
		m_leds[i] = scale(m_lay.mode[i][0] - m_lay.mode_r, m_lay.mode[i][1] - m_lay.mode_r,
		                  m_lay.mode_r * 2, m_lay.mode_r * 2);

	if (m_font_label) DeleteObject(m_font_label);
	if (m_font_small) DeleteObject(m_font_small);
	if (m_font_tiny)  DeleteObject(m_font_tiny);
	auto make_font = [&](double px, int weight, int floor_px = 7) {
		return CreateFontA(-std::max(floor_px, int(px * m_scale)), 0, 0, 0, weight,
		                   FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS,
		                   CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, VARIABLE_PITCH,
		                   "Segoe UI");
	};
	m_font_label = make_font(13, FW_BOLD);
	m_font_small = make_font(8.5, FW_NORMAL);
	// 目盛りは 34 個の番号をバーの真下に並べるので、思い切り小さくする
	m_font_tiny  = make_font(6.5, FW_NORMAL, 5);

	build_spots();
}

// 触れる場所は面ごとに違う。掴んでいる途中に作り直すと迷子になるので離す
void panel::build_spots()
{
	m_held = nullptr;
	m_spots.clear();
	if (m_lcd_only)
		return;

	// 面を選ぶつまみ。本体の外（下の帯）
	m_spots.push_back({ spot_kind::tab, mu2000::button::count, CTL_TAB_FRONT,
	                    scale(700, 386, 94, 13), UI_TEXT(tab_panel, "Panel"), "" });
	m_spots.push_back({ spot_kind::tab, mu2000::button::count, CTL_TAB_EDIT,
	                    scale(800, 386, 94, 13), UI_TEXT(tab_editor, "Editor"), "" });
	m_spots.push_back({ spot_kind::tab, mu2000::button::count, CTL_TAB_FX,
	                    scale(898, 386, 94, 13), UI_TEXT(tab_effects, "Effects"), "" });

	if (m_page == page::editor) { build_editor_spots(); return; }
	if (m_page == page::effects) { build_effect_spots(); return; }

	for (int i = 0; i < 18; i++)
		m_spots.push_back({ spot_kind::button, CAT_B[i], CTL_NONE,
		                    scale(m_lay.cat_x[i % 6] - m_lay.cat_w / 2, m_lay.cat_y[i / 6],
		                          m_lay.cat_w, m_lay.cat_h),
		                    CAT_LABEL[i], "" });
	for (int i = 0; i < 6; i++)
		m_spots.push_back({ spot_kind::button, MODES[i].b, CTL_NONE,
		                    scale(m_lay.mode[i][0] - m_lay.mode_r,
		                          m_lay.mode[i][1] - m_lay.mode_r,
		                          m_lay.mode_r * 2, m_lay.mode_r * 2),
		                    MODES[i].label, "" });
	for (int i = 0; i < 9; i++)
		m_spots.push_back({ spot_kind::button, NAV[i].b, CTL_NONE,
		                    scale(m_lay.nav[i][0], m_lay.nav[i][1],
		                          m_lay.nav[i][2], m_lay.nav[i][3]),
		                    NAV[i].label, NAV[i].sub });
	for (int i = 0; i < 2; i++)
		m_spots.push_back({ spot_kind::button, ROUND[i].b, CTL_NONE,
		                    scale(m_lay.round_[i][0] - m_lay.round_[i][2] / 2,
		                          m_lay.round_[i][1] - m_lay.round_[i][3] / 2,
		                          m_lay.round_[i][2], m_lay.round_[i][3]),
		                    ROUND[i].label, ROUND[i].sub });

	m_spots.push_back({ spot_kind::wheel,  mu2000::button::count, CTL_NONE, m_wheel, "", "" });
	m_spots.push_back({ spot_kind::volume, mu2000::button::count, CTL_NONE, m_volume,
	                    "VOLUME", "" });
}

const spot *panel::hit(int x, int y) const
{
	for (const spot &s : m_spots) {
		if (x >= s.r.left && x < s.r.right && y >= s.r.top && y < s.r.bottom) {
			// 丸いものは丸の中だけ
			if (s.kind == spot_kind::wheel || s.kind == spot_kind::volume) {
				const double cx = (s.r.left + s.r.right) * 0.5;
				const double cy = (s.r.top + s.r.bottom) * 0.5;
				const double rr = (s.r.right - s.r.left) * 0.5;
				if (std::hypot(x - cx, y - cy) > rr)
					continue;
			}
			return &s;
		}
	}
	return nullptr;
}


void panel::draw_tabs(HDC dc) const
{
	for (const spot &sp : m_spots) {
		if (sp.kind != spot_kind::tab)
			continue;
		const bool on = (sp.ctl == CTL_TAB_EDIT   && m_page == page::editor) ||
		                (sp.ctl == CTL_TAB_FX     && m_page == page::effects) ||
		                (sp.ctl == CTL_TAB_FRONT  && m_page == page::front);
		round_box(dc, sp.r, on ? RGB(70, 76, 84) : RGB(38, 41, 46),
		          on ? ACCENT : RGB(70, 74, 80), int(4 * m_scale));
		text_in(dc, sp.r, sp.label, on ? TEXT : TEXT_DIM, m_font_small,
		        DT_CENTER | DT_VCENTER | DT_SINGLELINE);
	}
}

panel::lcd_geom panel::lcd_grid() const
{
	lcd_geom g{};
	const int aw = m_lcd.right - m_lcd.left, ah = m_lcd.bottom - m_lcd.top;
	g.pad = std::max(2, int(5 * m_scale));

	// 点の大きさ。上の面 17 桁と左右の余白（LCD_SPAN 点ぶん）が横幅に収まるように
	int d = std::max(1, int(aw / LCD_SPAN));
	g.tick_h = std::max(2, int(2.5 * m_scale));
	g.line_h = std::max(6, int(8.5 * m_scale));
	// 目盛りの帯は 3 段。番号／MIC と BANK と PGM#／LINE。
	// **下の面はその下**。ここを詰めると「01」と MIC が重なる
	while (d > 1 && 16 * d + g.tick_h + g.line_h * 3 + 8 * d > ah - g.pad * 2)
		d--;
	g.d = d;
	g.scale_h = g.tick_h + g.line_h * 3;
	const int stack_h = 16 * d + g.scale_h + 8 * d;

	// 点は正方形のままにして、余った幅は左右に振り分ける
	g.x0 = m_lcd.left + std::max(0, int((aw - LCD_SPAN * d) / 2)) + int(std::lround(LCD_LEFT * d));
	g.y0 = m_lcd.top + std::max(g.pad, (ah - stack_h) / 2);
	g.sy = g.y0 + 16 * d + g.scale_h;
	return g;
}

void panel::draw_lcd(HDC dc, const snapshot &s) const
{
	if (!m_lcd_only) {
		RECT bez = m_lcd;
		InflateRect(&bez, int(5 * m_scale), int(5 * m_scale));
		round_box(dc, bez, RGB(60, 58, 52), RGB(110, 106, 96), int(5 * m_scale));
	}
	fill(dc, m_lcd, LCD_BACK);

	// 実機の窓は、DDRAM の桁がそのまま横一列に並んでいるのではない。
	// ボタンを押して確かめた割り振りは（doc/gui.md）
	//
	//   上の面（点の並び。2 行、桁のあいだは 1 点、**行のあいだは空けない**）
	//     0-8   レベルメータ。1 マス 2 本で 18 本（A1 A2 と 1-16）
	//     9-16  文字 8 桁。1 行目が音色名、2 行目が ▶000◀001
	//   下の面
	//     行 0 の 17-18   部の番号「01」
	//     行 1 の 17-19   「A01」
	//     20-22（両行）   楽器のかたち。**点が細かく、正方形でもない**
	//     **23（両行）は絵ではない**。決まった形のセグメントを点けたり
	//     消したりする 64 個のビットが入っている（下の ctl）
	const lcd_geom g = lcd_grid();
	const int d = g.d, pad = g.pad, x0 = g.x0, y0 = g.y0;
	const int tick_h = g.tick_h, line_h = g.line_h, scale_h = g.scale_h;

	const COLORREF FAINT = RGB(147, 202, 45);              // 絵の区画の消え点

	// 使う色ごとに筆を 1 本。点の縁を背景と混ぜるので、色の数は描くまで決まらない
	std::vector<std::pair<COLORREF, HBRUSH>> brushes;
	auto br = [&](COLORREF c) {
		for (const auto &b : brushes)
			if (b.first == c)
				return b.second;
		brushes.emplace_back(c, CreateSolidBrush(c));
		return brushes.back().second;
	};
	HBRUSH lit = br(LCD_DOT);

	// 点 1 つ。w × h は点の間隔で、右と下に DOT_GAP ぶんの隙間を空ける。
	// 隙間が 1 画素に満たないときは、その 1 画素を背景と混ぜた色で塗る。
	// 小さい窓で隙間が丸ごと 1 画素になり、文字が薄く見えていたのを防ぐ
	const double gap = DOT_GAP * d;
	auto dotbox = [&](int l, int t, int w, int h, COLORREF ink) {
		int gi = int(gap);
		double fr = gap - gi;
		const bool part = fr > 0.05;
		int sw = w - gi - (part ? 1 : 0), sh = h - gi - (part ? 1 : 0);
		if (sw < 1 || sh < 1) {                  // 小さすぎる。隙間なしで塗る
			RECT r{ l, t, l + std::max(1, w), t + std::max(1, h) };
			FillRect(dc, &r, br(ink));
			return;
		}
		RECT r{ l, t, l + sw, t + sh };
		FillRect(dc, &r, br(ink));
		if (!part)
			return;
		const HBRUSH edge = br(mix(ink, LCD_BACK, fr));
		RECT rc{ l + sw, t, l + sw + 1, t + sh };
		FillRect(dc, &rc, edge);
		RECT rr{ l, t + sh, l + sw, t + sh + 1 };
		FillRect(dc, &rr, edge);
		RECT rx{ l + sw, t + sh, l + sw + 1, t + sh + 1 };
		FillRect(dc, &rx, br(mix(ink, LCD_BACK, 1.0 - (1.0 - fr) * (1.0 - fr))));
	};

	// 端数のある座標で描く多角形。枠線は引かない
	struct pt { double x, y; };
	auto poly = [&](std::initializer_list<pt> ps, COLORREF ink) {
		POINT q[16];
		int n = 0;
		for (const pt &p : ps)
			if (n < 16)
				q[n++] = POINT{ int(std::lround(p.x)), int(std::lround(p.y)) };
		HGDIOBJ ob = SelectObject(dc, br(ink));
		HGDIOBJ op = SelectObject(dc, GetStockObject(NULL_PEN));
		Polygon(dc, q, n);
		SelectObject(dc, op);
		SelectObject(dc, ob);
	};
	// 角度は真上が 0 度で時計回り
	auto polar = [](double cx, double cy, double r, double deg) {
		const double a = deg * PI / 180.0;
		return pt{ cx + r * std::sin(a), cy - r * std::cos(a) };
	};
	// 帯状の弧（中心 cx cy、半径 r0-r1、a0 度から a1 度）。端は半径の向きに切る
	auto ring = [&](double cx, double cy, double r0, double r1, double a0, double a1,
	                COLORREF ink) {
		const int n = std::max(4, std::min(60, int((a1 - a0) / 6)));
		std::vector<POINT> q;
		for (int i = 0; i <= n; i++) {
			const pt p = polar(cx, cy, r1, a0 + (a1 - a0) * i / n);
			q.push_back(POINT{ int(std::lround(p.x)), int(std::lround(p.y)) });
		}
		for (int i = n; i >= 0; i--) {
			const pt p = polar(cx, cy, r0, a0 + (a1 - a0) * i / n);
			q.push_back(POINT{ int(std::lround(p.x)), int(std::lround(p.y)) });
		}
		HGDIOBJ ob = SelectObject(dc, br(ink));
		HGDIOBJ op = SelectObject(dc, GetStockObject(NULL_PEN));
		Polygon(dc, q.data(), int(q.size()));
		SelectObject(dc, op);
		SelectObject(dc, ob);
	};

	// 23 桁目の制御ビット。列 A-D は bit3-bit0、行は上の桁の 0-7 と
	// 下の桁の 0-7 をつないだ 0-15。番地は実測（doc/gui.md）
	auto ctl = [&](int col, int row) -> bool {
		if (!s.lcd_on)
			return false;
		const u8 v = s.dots[((row / 8) * LCD_COLS + TOP_COLS + 6) * CELL_H + (row % 8)];
		return BIT(v, 3 - col) != 0;
	};
	enum { CA = 0, CB = 1, CC = 2, CD = 3 };

	// 1 マスぶんの点を描く
	auto cell = [&](int row, int col, int px, int py) {
		const u8 *c = s.dots + (row * LCD_COLS + col) * CELL_H;
		for (int y = 0; y < CELL_H; y++)
			for (int x = 0; x < CELL_W; x++)
				dotbox(px + x * d, py + y * d, d, d,
				       (s.lcd_on && BIT(c[y], 4 - x)) ? LCD_DOT : LCD_GHOST);
	};

	// ---- 上の面。メータ 9 マス ＋ 文字 8 桁。行のあいだは空けない
	for (int row = 0; row < LCD_ROWS; row++)
		for (int col = 0; col < TOP_COLS; col++)
			cell(row, col, x0 + col * (CELL_W + 1) * d, y0 + row * CELL_H * d);

	// ---- 目盛りの帯。ここも**印刷ではなくセグメント**で、点いたり消えたりする
	const int scale_y = y0 + 16 * d;
	const int line2 = scale_y + tick_h + line_h;
	{
		const bool on_scale = ctl(CD, 4);          // 「1」-「32」
		const bool on_a1a2  = ctl(CD, 3);          // 「A1」「A2」
		const COLORREF ink_scale = on_scale ? LCD_DOT : LCD_GHOST;
		const COLORREF ink_a1a2  = on_a1a2  ? LCD_DOT : LCD_GHOST;

		HPEN p = CreatePen(PS_SOLID, 1, on_scale ? LCD_DOT : LCD_GHOST);
		HGDIOBJ op = SelectObject(dc, p);
		for (int i = 0; i < TOP_COLS * 2; i++) {
			const int col = i / 2;
			const int bx  = x0 + col * (CELL_W + 1) * d + ((i & 1) ? 3 * d : 0) + d / 2;
			if (i >= 2 || on_a1a2) {
				MoveToEx(dc, bx, scale_y, nullptr);
				LineTo(dc, bx, scale_y + tick_h);
			}
			// 番号はパートの番号。**そのバーの真下**に置く
			const int part = i - 1;
			char n[8];
			std::snprintf(n, sizeof(n), i < 2 ? "A%d" : "%d", i < 2 ? i + 1 : part);
			RECT t{ bx - 3 * d / 2, scale_y + tick_h,
			        bx + 3 * d / 2, scale_y + tick_h + line_h };
			text_in(dc, t, n, i < 2 ? ink_a1a2 : ink_scale, m_font_tiny,
			        DT_CENTER | DT_TOP | DT_SINGLELINE);
		}
		SelectObject(dc, op);
		DeleteObject(p);

		// MIC と LINE は左端に上下に並ぶ
		RECT mic{ x0, line2, x0 + int(20 * m_scale), line2 + line_h };
		text_in(dc, mic, "MIC", ctl(CD, 1) ? LCD_DOT : LCD_GHOST, m_font_small,
		        DT_LEFT | DT_TOP | DT_SINGLELINE);
		RECT lin{ x0, line2 + line_h, x0 + int(24 * m_scale), line2 + line_h * 2 };
		text_in(dc, lin, "LINE", ctl(CD, 2) ? LCD_DOT : LCD_GHOST, m_font_small,
		        DT_LEFT | DT_TOP | DT_SINGLELINE);

		// BANK と PGM# は 2 つずつあり、パート番号の下に並んでいる。
		// 左側の組が D0、右側の組が D5 で点け消しされる
		struct { int part; const char *label; bool right; } marks[] = {
			{  3, "BANK", false }, { 11, "PGM#", false },
			{ 19, "BANK", true  }, { 27, "PGM#", true  },
		};
		for (const auto &mk : marks) {
			const int cx = x0 + (part_x(mk.part) + part_x(mk.part + 1) + 2) * d / 2;
			const int w = int(26 * m_scale);
			RECT r{ cx - w / 2, line2, cx + w / 2, line2 + line_h };
			text_in(dc, r, mk.label,
			        ctl(CD, mk.right ? 5 : 0) ? LCD_DOT : LCD_GHOST, m_font_small,
			        DT_CENTER | DT_TOP | DT_SINGLELINE);
		}
	}

	// ---- 下の面
	const int sy = g.sy;
	const int seg_h = 8 * d;                       // 文字 1 行ぶんの高さ
	auto lx = [&](int which) { return x0 + int(std::lround(m_lay.low_x[which] * d)); };
	auto lw = [&](int which) { return int(std::lround(m_lay.low_w[which] * d)); };

	// 部の番号「01」と「A01」（「 A/D1」なども同じ 5 桁）。
	// 実機の字間は 1 桁目と 2 桁目が 1 点、2 桁目と 3 桁目が 2 点、あとは 1 点。
	// 2 点あくところが「01」と「A01」の境目で、low.x の 13 はそこから来る
	for (int i = 0; i < 2; i++)
		cell(0, TOP_COLS + i, lx(LOW_PART) + i * (CELL_W + 1) * d, sy);
	for (int i = 0; i < 3; i++)
		cell(1, TOP_COLS + i, lx(LOW_BANK) + i * (CELL_W + 1) * d, sy);

	// 楽器のかたち。20-22 桁の両行が 1 枚の絵。**23 桁目は絵ではない**ので入れない。
	// 点は横長で、文字の点と同じくほんのわずかに隙間がある
	{
		const int ix = lx(LOW_ICON), iw = lw(LOW_ICON);
		const int first = TOP_COLS + 3, last = LCD_COLS - 1;   // 20-22
		const int nx = (last - first) * CELL_W, ny = LCD_ROWS * CELL_H;
		for (int row = 0; row < LCD_ROWS; row++)
			for (int col = first; col < last; col++) {
				const u8 *c = s.dots + (row * LCD_COLS + col) * CELL_H;
				for (int y = 0; y < CELL_H; y++) {
					const int yy = row * CELL_H + y;
					const int top = sy + yy * seg_h / ny;
					const int bot = sy + (yy + 1) * seg_h / ny;
					for (int x = 0; x < CELL_W; x++) {
						const int xx = (col - first) * CELL_W + x;
						const int left  = ix + xx * iw / nx;
						const int right = ix + (xx + 1) * iw / nx;
						dotbox(left, top, std::max(1, right - left), std::max(1, bot - top),
						       (s.lcd_on && BIT(c[y], 4 - x)) ? LCD_DOT : FAINT);
					}
				}
			}
	}

	// ---- 決まった形のセグメント。**23 桁目のビットで点け消しする**。
	// 形と寸法は実機の写真から採った（単位は点の間隔 d）
	{
		auto ink = [&](bool on) { return on ? LCD_DOT : LCD_GHOST; };
		const double dd = d;

		// 7 セグメント。seg は a b c d e f g の順のビット。
		// 実機のセグメントは端が斜めに切れた台形で、真ん中の g は両端がとがる
		auto seven = [&](double x, double y, double w, double h, double t, unsigned seg) {
			const double k  = std::max(0.6, 0.1 * dd);     // セグメントのあいだの隙間
			const double ym = y + h / 2, ht = t / 2;
			poly({ { x + k, y }, { x + w - k, y }, { x + w - t - k, y + t }, { x + t + k, y + t } },
			     ink(BIT(seg, 0)));                                             // a
			poly({ { x + w, y + k }, { x + w, ym - ht - k }, { x + w - ht, ym - k },
			       { x + w - t, ym - ht - k }, { x + w - t, y + t + k } },
			     ink(BIT(seg, 1)));                                             // b
			poly({ { x + w, ym + ht + k }, { x + w, y + h - k }, { x + w - t, y + h - t - k },
			       { x + w - t, ym + ht + k }, { x + w - ht, ym + k } },
			     ink(BIT(seg, 2)));                                             // c
			poly({ { x + t + k, y + h - t }, { x + w - t - k, y + h - t }, { x + w - k, y + h },
			       { x + k, y + h } },
			     ink(BIT(seg, 3)));                                             // d
			poly({ { x, ym + ht + k }, { x + ht, ym + k }, { x + t, ym + ht + k },
			       { x + t, y + h - t - k }, { x, y + h - k } },
			     ink(BIT(seg, 4)));                                             // e
			poly({ { x, y + k }, { x + t, y + t + k }, { x + t, ym - ht - k },
			       { x + ht, ym - k }, { x, ym - ht - k } },
			     ink(BIT(seg, 5)));                                             // f
			poly({ { x + ht + k, ym }, { x + t + k, ym - ht }, { x + w - t - k, ym - ht },
			       { x + w - ht - k, ym }, { x + w - t - k, ym + ht }, { x + t + k, ym + ht } },
			     ink(BIT(seg, 6)));                                             // g
		};

		// 送り量の扇。中心角 45 度の細い弧を 8 本、同じ中心で重ねたもの。
		// 中心（扇の要）は下の面の下端より少し下にある。下から N 本を点ける
		auto fan = [&](int which, const bool *on8) {
			const double cx = lx(which) + lw(which) / 2.0;
			const double cy = sy + 8.4 * dd;
			for (int k = 0; k < 8; k++) {
				const double r = (1.45 + 1.0 * k) * dd;
				ring(cx, cy, r - 0.21 * dd, r + 0.21 * dd, -22.5, 22.5, ink(on8[k]));
			}
		};

		// VOL と EXP。**行 0 の 19 桁目**に、レベルメータと同じ形で
		// 入っている（左の 2 点が VOL、右の 2 点が EXP）。
		// 実機では離れた場所に、横に長い 8 本の棒で出る
		{
			const u8 *c = s.dots + (0 * LCD_COLS + TOP_COLS + 2) * CELL_H;
			const double pitch = 8.3 * dd / 8;
			for (int y = 0; y < CELL_H; y++) {
				const int top = int(std::lround(sy - 0.5 * dd + y * pitch));
				const int bot = std::max(top + 1, int(std::lround(sy - 0.5 * dd + y * pitch + 0.48 * pitch)));
				const bool vol = s.lcd_on && (BIT(c[y], 4) || BIT(c[y], 3));
				const bool exp = s.lcd_on && (BIT(c[y], 1) || BIT(c[y], 0));
				RECT rv{ lx(LOW_VOL), top, lx(LOW_VOL) + lw(LOW_VOL), bot };
				FillRect(dc, &rv, br(ink(vol)));
				RECT re{ lx(LOW_EXP), top, lx(LOW_EXP) + lw(LOW_EXP), bot };
				FillRect(dc, &re, br(ink(exp)));
			}
		}

		// パン。下の開いた円弧の中で、針が 45 度おきの 7 か所に飛ぶ。
		// D15 が左下、D12 が真上、D9 が右下
		{
			const double cx = lx(LOW_PAN) + lw(LOW_PAN) / 2.0, cy = sy + 3.75 * dd;
			const double r = 3.6 * dd;
			// 円弧は点けたり消したりしない（実機はいつも点いている）
			ring(cx, cy, r - 0.25 * dd, r, -124.0, 124.0, ink(s.lcd_on));
			const double r0 = 0.29 * r, r1 = 0.72 * r, ht = 0.18 * dd;
			for (int k = 0; k < 7; k++) {
				const bool on = ctl(CD, 15 - k);
				const double a = -135.0 + 45.0 * k;
				const pt p0 = polar(cx, cy, r0, a), p1 = polar(cx, cy, r1, a);
				const double nx = std::cos(a * PI / 180.0) * ht, ny = std::sin(a * PI / 180.0) * ht;
				poly({ { p0.x - nx, p0.y - ny }, { p1.x - nx, p1.y - ny },
				       { p1.x + nx, p1.y + ny }, { p0.x + nx, p0.y + ny } }, ink(on));
			}
		}

		// リバーブ・コーラス・バリエーションの送り量
		{
			bool rev[8], cho[8], var[8];
			for (int k = 0; k < 8; k++) {
				rev[k] = ctl(CA, 15 - k);
				cho[k] = ctl(CB, 15 - k);
				var[k] = ctl(CC, 15 - k);
			}
			fan(LOW_REV, rev);
			fan(LOW_CHO, cho);
			fan(LOW_VAR, var);
		}

		// ノートシフト。符号（横棒は常時、縦棒が点くと ＋）と 2 桁。
		// 符号の縦棒は、横棒と交わるところで少し途切れている
		{
			const double t  = 0.4 * dd;
			const double dh = 5.4 * dd, dy = sy + 2.2 * dd, dw = 2.6 * dd;
			const double kx = lx(LOW_KEY);
			const double sw = 2.4 * dd, cy = dy + dh / 2, vh = 0.62 * dh;
			const double vx = kx + sw / 2, cut = std::max(1.0, 0.3 * dd);
			poly({ { kx, cy - t / 2 }, { kx + sw, cy - t / 2 }, { kx + sw, cy + t / 2 },
			       { kx, cy + t / 2 } }, ink(ctl(CB, 0)));
			const bool plus = ctl(CA, 0);
			poly({ { vx - t / 2, cy - vh / 2 }, { vx + t / 2, cy - vh / 2 },
			       { vx + t / 2, cy - t / 2 - cut }, { vx - t / 2, cy - t / 2 - cut } }, ink(plus));
			poly({ { vx - t / 2, cy + t / 2 + cut }, { vx + t / 2, cy + t / 2 + cut },
			       { vx + t / 2, cy + vh / 2 }, { vx - t / 2, cy + vh / 2 } }, ink(plus));

			// 十の位は a/d/e/g がひとまとめ。f は使われない
			const bool ten_adeg = ctl(CA, 1);
			unsigned ten = 0;
			if (ten_adeg) ten |= (1u << 0) | (1u << 3) | (1u << 4) | (1u << 6);
			if (ctl(CB, 1)) ten |= 1u << 1;
			if (ctl(CA, 7)) ten |= 1u << 2;
			if (ctl(CB, 4)) ten |= 1u << 5;
			seven(kx + sw + 0.2 * dd, dy, dw, dh, t, ten);

			unsigned one = 0;
			if (ctl(CB, 6)) one |= 1u << 0;   // a
			if (ctl(CA, 6)) one |= 1u << 1;   // b
			if (ctl(CA, 3)) one |= 1u << 2;   // c
			if (ctl(CA, 2)) one |= 1u << 3;   // d
			if (ctl(CB, 2)) one |= 1u << 4;   // e
			if (ctl(CB, 7)) one |= 1u << 5;   // f
			if (ctl(CB, 3)) one |= 1u << 6;   // g
			seven(kx + sw + 0.2 * dd + dw + 0.5 * dd, dy, dw, dh, t, one);
		}

		// いちばん右の ▶ は 4 つ。上の 1 つは札がなく、点く場面をまだ見ていない。
		// 残りの 3 つが XG / TG300B(GS) / PERFORM。
		// PLG のぶんは C2 か D8 のどちらかだが、まだ決められていない
		{
			const double mx = lx(LOW_MODE);
			const double th = 1.45 * dd, tw = th * 0.9;     // 正三角形に近い
			const bool mode[4] = { false, ctl(CB, 5), ctl(CA, 4), ctl(CA, 5) };
			for (int k = 0; k < 4; k++) {
				const double cy = sy + MODE_Y[k] * dd;
				poly({ { mx, cy - th / 2 }, { mx + tw, cy }, { mx, cy + th / 2 } }, ink(mode[k]));
			}
		}

		// 下の面の上に出る ▼ のカーソル。いま何を弄っているかを示す
		{
			// 先は下の面より少し上。VOL の棒や扇のいちばん上に掛からないように
			const int cur_y = sy - std::max(2, int(std::lround(1.6 * d)));
			const int hw = std::max(2, d);
			struct { int at; bool on; } cur[] = {
				{ LOW_VOL,  ctl(CC, 3) }, { LOW_EXP, ctl(CC, 4) },
				{ LOW_PAN,  ctl(CC, 5) }, { LOW_REV, ctl(CC, 6) },
				{ LOW_CHO,  ctl(CC, 7) }, { LOW_VAR, ctl(CD, 6) },
				{ LOW_KEY,  ctl(CD, 7) },
			};
			for (const auto &c : cur) {
				if (!c.on)
					continue;
				const int cx = lx(c.at) + lw(c.at) / 2;
				const POINT tri[3] = { { cx - hw, cur_y - hw }, { cx + hw, cur_y - hw },
				                       { cx, cur_y } };
				HGDIOBJ ob = SelectObject(dc, lit);
				Polygon(dc, tri, 3);
				SelectObject(dc, ob);
			}
			// バンク番号とプログラム番号のカーソルは**楽器のかたちの上**。
			// バンクは 4-5 列目、プログラムは 12-13 列目の上（実機を見て教わった）
			const int ix = lx(LOW_ICON);
			const int tops[2] = { ix + (3 + 5) * d / 2, ix + (11 + 13) * d / 2 };
			const bool ton[2] = { ctl(CC, 1), ctl(CC, 0) };
			for (int k = 0; k < 2; k++) {
				if (!ton[k])
					continue;
				const POINT tri[3] = { { tops[k] - hw, cur_y - hw },
				                       { tops[k] + hw, cur_y - hw },
				                       { tops[k], cur_y } };
				HGDIOBJ ob = SelectObject(dc, lit);
				Polygon(dc, tri, 3);
				SelectObject(dc, ob);
			}
		}
	}

	for (const auto &b : brushes)
		DeleteObject(b.second);

	if (s.message[0]) {
		RECT r = m_lcd;
		fill(dc, r, RGB(24, 26, 22));
		text_in(dc, r, s.message, RGB(210, 220, 200), m_font_label,
		        DT_CENTER | DT_VCENTER | DT_WORDBREAK);
	}
}

void panel::draw_button(HDC dc, const spot &sp, bool down) const
{
	round_box(dc, sp.r, down ? KEY_DOWN : KEY_FACE, KEY_EDGE, int(3 * m_scale));
}

// 大きなダイヤル。回した角度で窪みが回る
void panel::draw_wheel(HDC dc, int angle) const
{
	const POINT c = at(m_lay.dial[0], m_lay.dial[1]);
	const int r = int(m_lay.dial[2] * m_scale);

	// panel.txt で絵を渡されていれば、それを回して描く
	if (m_lay.dial_art) {
		m_lay.dial_art->draw(dc, RECT{ c.x - r, c.y - r, c.x + r, c.y + r },
		                     double(angle));
		return;
	}

	disc(dc, c.x, c.y, r, KEY_FACE, KEY_EDGE, std::max(1, int(2 * m_scale)));
	const double a = angle * PI / 180.0;
	const int ox = c.x + int(std::sin(a) * r * 0.36);
	const int oy = c.y - int(std::cos(a) * r * 0.36);
	disc(dc, ox, oy, int(r * 0.45), RGB(186, 176, 140), RGB(146, 137, 106),
	     std::max(1, int(m_scale)));
}

// 音量つまみ
void panel::draw_volume(HDC dc, double v) const
{
	const POINT c = at(m_lay.volume[0], m_lay.volume[1]);
	const int r = int(m_lay.volume[2] * m_scale);
	const double deg = -135.0 + 270.0 * v;       // 左いっぱいから右いっぱいまで

	if (m_lay.volume_art) {
		m_lay.volume_art->draw(dc, RECT{ c.x - r, c.y - r, c.x + r, c.y + r }, deg);
	} else {
		disc(dc, c.x, c.y, r, KEY_FACE, KEY_EDGE, std::max(1, int(m_scale)));
		const double a = deg * PI / 180.0;
		line(dc, c.x, c.y, c.x + int(std::sin(a) * r * 0.8),
		     c.y - int(std::cos(a) * r * 0.8), RGB(70, 64, 48),
		     std::max(2, int(2 * m_scale)));
	}
	text_in(dc, scale(m_lay.volume[0] - 36, m_lay.volume[1] + m_lay.volume[2] + 4,
	                  72, 12), "VOLUME", PANEL_INK, m_font_small,
	        DT_CENTER | DT_TOP | DT_SINGLELINE);
}


void panel::paint_front(HDC dc, const snapshot &s, u64 pressed, double volume,
                        const char *status) const
{
	RECT all{ 0, 0, m_w, m_h };
	fill(dc, all, RGB(24, 26, 30));
	fill(dc, scale(0, 0, LOGICAL_W, m_lay.body_h), PANEL_FACE);

	// ---- 飾り。位置も色も panel.txt から来る（doc/panel-editing.md）
	for (const deco &g : m_lay.decos) {
		if (g.k == deco::text)
			text_in(dc, scale(g.x, g.y, g.w, g.h), g.str.c_str(), g.a,
			        g.font ? m_font_label : m_font_small, g.align);
		else if (g.k == deco::disc) {
			const POINT c = at(g.x, g.y);
			disc(dc, c.x, c.y, int(g.w * m_scale), g.a, g.b,
			     std::max(1, int(g.h * m_scale)));
		} else if (g.k == deco::art) {
			if (g.pic)
				g.pic->draw(dc, scale(g.x, g.y, g.w, g.h));
		} else
			round_box(dc, scale(g.x, g.y, g.w, g.h), g.a, g.b,
			          std::max(1, int(g.radius * m_scale)));
	}

	// ---- 中

	draw_lcd(dc, s);

	// 窓の下の札は、下段の並びと同じ割合で置く。窓の中身とずれないように
	{
		const lcd_geom g = lcd_grid();
		const int y = at(0, m_lay.columns_y).y, h = int(12 * m_scale), w = int(64 * m_scale);
		for (const column &c : COLUMNS) {
			const int cx = g.x0 + int(std::lround((m_lay.low_x[c.at] + m_lay.low_w[c.at] / 2) * g.d));
			RECT r{ cx - w / 2, y, cx + w / 2, y + h };
			text_in(dc, r, c.label, PANEL_INK, m_font_small,
			        DT_CENTER | DT_TOP | DT_SINGLELINE);
		}

		// 窓の右の札。高さは液晶の中の ▶ に合わせる（横は panel.txt の modes.x）
		if (m_lay.modes_x >= 0) {
			const int x = at(m_lay.modes_x, 0).x;
			// ▶ の間隔より字が大きいと重なる（小さい窓で、字の下限が効くとき）
			const double step = (MODE_Y[2] - MODE_Y[1]) * g.d;
			const HFONT font = step < std::max(7.0, 8.5 * m_scale) ? m_font_tiny : m_font_small;
			for (int k = 0; k < 3; k++) {
				const int cy = g.sy + int(std::lround(MODE_Y[k + 1] * g.d));
				RECT r{ x, cy - h, x + int(80 * m_scale), cy + h };
				text_in(dc, r, MODE_LABEL[k], PANEL_INK, font,
				        DT_LEFT | DT_VCENTER | DT_SINGLELINE);
			}
		}
	}

	for (int i = 0; i < 18; i++)
		text_in(dc, scale(m_lay.cat_x[i % 6] - 34, m_lay.cat_y[i / 6] - 14, 68, 14),
		        CAT_LABEL[i], PANEL_INK, m_font_small,
		        DT_CENTER | DT_TOP | DT_SINGLELINE);

	// MU / PLG-1..3 の表示灯。LED は 6 番から
	{
		const char *plg[4] = { "MU", "PLG-1", "PLG-2", "PLG-3" };
		for (int i = 0; i < 4; i++) {
			const double px = m_lay.plg[0] + i * m_lay.plg[1];
			const POINT c = at(px, m_lay.plg[2]);
			const bool on = BIT(s.leds, 6 + i) != 0;
			if (const svg_art *pic = m_lay.plg_art.pick(on, false)) {
				const int r = int(6 * m_scale);
				pic->draw(dc, RECT{ c.x - r, c.y - r, c.x + r, c.y + r });
			} else {
				// 実機の表示灯は四角
				const int r = int(4 * m_scale);
				round_box(dc, RECT{ c.x - r, c.y - r, c.x + r, c.y + r },
				          on ? LED_ON : RGB(64, 62, 52), RGB(110, 106, 92), std::max(1, int(m_scale)));
			}
			text_in(dc, scale(px - 22, m_lay.plg[2] + 7, 44, 12), plg[i], PANEL_INK,
			        m_font_small, DT_CENTER | DT_TOP | DT_SINGLELINE);
		}
	}

	// ---- 右

	for (int i = 0; i < 6; i++) {
		const mode_button &m = MODES[i];
		const double mx = m_lay.mode[i][0], my = m_lay.mode[i][1];
		text_in(dc, scale(mx - 34, my - m_lay.mode_r - 19, 68, 14), m.label, PANEL_INK,
		        m_font_small, DT_CENTER | DT_TOP | DT_SINGLELINE);
		const POINT c = at(mx, my);
		const bool down = ((pressed >> int(m.b)) & 1) != 0;
		const bool on = BIT(s.leds, m.led) != 0;
		// panel.txt で絵を渡されていれば、ようすに合う 1 枚を貼る
		if (const svg_art *pic = m_lay.mode_art.pick(on, down)) {
			const int r = int(m_lay.mode_r * m_scale);
			pic->draw(dc, RECT{ c.x - r, c.y - r, c.x + r, c.y + r });
		} else {
			disc(dc, c.x, c.y, int(m_lay.mode_r * m_scale),
			     down ? KEY_DOWN : RGB(198, 188, 152), KEY_EDGE, std::max(1, int(m_scale)));
			disc(dc, c.x, c.y, int(m_lay.mode_led_r * m_scale),
			     on ? LED_ON : RGB(74, 72, 60), RGB(110, 106, 92), 1);
		}
	}

	// 四角いボタン。名札は上に重ねる
	for (int i = 0; i < 9; i++) {
		const place &p = NAV[i];
		const double px = m_lay.nav[i][0], py = m_lay.nav[i][1];
		const double pw = m_lay.nav[i][2], ph = m_lay.nav[i][3];
		const spot *sp = nullptr;
		for (const spot &q : m_spots)
			if (q.kind == spot_kind::button && q.button == p.b) { sp = &q; break; }
		if (!sp)
			continue;
		const bool down = ((pressed >> int(p.b)) & 1) != 0;
		if (const svg_art *pic = m_lay.nav_art.pick(down, down))
			pic->draw(dc, sp->r);
		else
			draw_button(dc, *sp, down);
		text_in(dc, scale(px, py + 4, pw, 12), p.label, RGB(58, 53, 38),
		        m_font_small, DT_CENTER | DT_TOP | DT_SINGLELINE);
		if (p.sub[0])
			text_in(dc, scale(px, py + ph - 14, pw, 12), p.sub, RGB(58, 53, 38),
			        m_font_small, DT_CENTER | DT_TOP | DT_SINGLELINE);
	}
	for (int i = 0; i < 18; i++) {
		RECT r = scale(m_lay.cat_x[i % 6] - m_lay.cat_w / 2, m_lay.cat_y[i / 6],
		               m_lay.cat_w, m_lay.cat_h);
		const bool down = ((pressed >> int(CAT_B[i])) & 1) != 0;
		if (const svg_art *pic = m_lay.cat_art.pick(down, down))
			pic->draw(dc, r);
		else
			round_box(dc, r, down ? KEY_DOWN : KEY_FACE, KEY_EDGE, int(3 * m_scale));
	}
	for (int i = 0; i < 2; i++) {
		const place &p = ROUND[i];
		const double px = m_lay.round_[i][0], py = m_lay.round_[i][1];
		const POINT c = at(px, py);
		const bool down = ((pressed >> int(p.b)) & 1) != 0;
		const int rr = int(m_lay.round_[i][2] / 2 * m_scale);
		if (const svg_art *pic = m_lay.round_art.pick(down, down))
			pic->draw(dc, RECT{ c.x - rr, c.y - rr, c.x + rr, c.y + rr });
		else
			disc(dc, c.x, c.y, rr, down ? KEY_DOWN : KEY_FACE, KEY_EDGE,
			     std::max(1, int(m_scale)));
		text_in(dc, scale(px - 40, py - 26, 80, 12), p.label, PANEL_INK,
		        m_font_small, DT_CENTER | DT_TOP | DT_SINGLELINE);
	}

	draw_wheel(dc, m_wheel_angle);
	draw_volume(dc, volume);

	// 状態の行は本体の一番下（body_h の内側）に載るので、ボタンの名前と同じ濃い色で書く。
	// 前は暗い帯向けの薄い灰色で、本体の地の色に溶けて読めなかった
	if (status && status[0])
		text_in(dc, m_status, status, PANEL_INK, m_font_small,
		        DT_LEFT | DT_VCENTER | DT_SINGLELINE);
	text_in(dc, m_hint, UI_TEXT(hint_front, "Turn the big dial with the wheel / click buttons / "
                             "keys: A=PLAY E=EDIT U=UTIL F=EFFECT [ ]=PART"),
	        RGB(120, 124, 130), m_font_small, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

	draw_tabs(dc);
}

// 論理座標の方眼。50 ごとに線、100 ごとに濃い線と数字を入れる。
// 絵の位置を直すときは、これを出して読み取ってから表を書き換える
void panel::draw_grid(HDC dc) const
{
	HPEN thin = CreatePen(PS_SOLID, 1, RGB(255, 80, 80));
	HPEN bold = CreatePen(PS_SOLID, 1, RGB(255, 0, 0));
	HGDIOBJ op = SelectObject(dc, thin);
	SetBkMode(dc, TRANSPARENT);

	for (int x = 0; x <= LOGICAL_W; x += 50) {
		SelectObject(dc, (x % 100) ? thin : bold);
		const POINT a = at(x, 0), b = at(x, LOGICAL_H);
		MoveToEx(dc, a.x, a.y, nullptr);
		LineTo(dc, b.x, b.y);
	}
	for (int y = 0; y <= LOGICAL_H; y += 50) {
		SelectObject(dc, (y % 100) ? thin : bold);
		const POINT a = at(0, y), b = at(LOGICAL_W, y);
		MoveToEx(dc, a.x, a.y, nullptr);
		LineTo(dc, b.x, b.y);
	}
	for (int x = 0; x <= LOGICAL_W; x += 100)
		for (int y = 0; y <= LOGICAL_H; y += 100) {
			char n[32];
			std::snprintf(n, sizeof(n), "%d,%d", x, y);
			RECT r{ at(x + 2, y + 1).x, at(x + 2, y + 1).y,
			        at(x + 60, y + 12).x, at(x + 60, y + 12).y };
			text_in(dc, r, n, RGB(200, 0, 0), m_font_small,
			        DT_LEFT | DT_TOP | DT_SINGLELINE);
		}

	SelectObject(dc, op);
	DeleteObject(thin);
	DeleteObject(bold);
}

void panel::paint(HDC dc, const snapshot &s, u64 pressed, const char *status) const
{
	if (m_lcd_only) {
		draw_lcd(dc, s);
		return;
	}
	if (m_page == page::editor)       paint_editor(dc, status);
	else if (m_page == page::effects) paint_effects(dc, status);
	else                              paint_front(dc, s, pressed, m_volume_now, status);
	if (m_grid)
		draw_grid(dc);
}

} // namespace ui
