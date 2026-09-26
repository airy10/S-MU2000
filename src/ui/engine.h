// license:BSD-3-Clause
//
// The synth side of a front end: load the ROMs, boot, and render blocks on
// demand from the audio device.
//
// This used to live inside gui.cpp. It moved here when the macOS front end
// arrived, because the part worth sharing is not the boilerplate but the
// routing in fill(): which MIDI port feeds which part, and what gets echoed
// back out. Two copies of that is two things that can drift, and a drift there
// would show up as the two platforms sounding different.
//
// Nothing in here touches a window, so it is the same on both platforms. What
// it does reach for is the OS clock and the settings file, both through
// compat/ (see doc/porting-macos.md).

#ifndef S_MU2000_UI_ENGINE_H
#define S_MU2000_UI_ENGINE_H

#pragma once

#include "audio_in.h"
#include "audio_out.h"
#include "bridge.h"
#include "driver.h"
#include "midi_guard.h"
#include "midi_in.h"
#include "midi_out.h"
#include "texts.h"
#include "analog_out.h"
#include "mu2000.h"
#include "bootcache.h"
#include "nvram.h"

#include "compat/platform.h"
#include "plg/cards.h"
#include "plg/host.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace ui {

struct engine {
	mu2000 mu;

	// A PLG card, and what was asked for. **The card goes in before reset()**,
	// inside boot(), because the PLG scan is part of the boot sequence: a board
	// that appears later is never noticed (boot --plg-late measures it - zero
	// messages, no lamp). The ordering lives here rather than in a comment,
	// because getting it wrong is silent.
	plg::host   m_plg;
	std::string m_plg_path;
	int         m_plg_builtin = -1;
	std::string m_plg_msg;    // why the card is not in the slot, if it is not
	std::string m_plg_trace_path;
	std::FILE *m_plg_trace = nullptr;
	std::string m_plg_tx_path;
	std::FILE *m_plg_tx = nullptr;

	// Which card to put in slot 1. `path` is a shared library; `builtin` is an
	// index into plg::builtin_card(). Empty and -1 means none. Call before boot().
	void set_plg_card(const std::string &path, int builtin)
	{ m_plg_path = path; m_plg_builtin = builtin; }
	// Where to write what arrives on a PLG line (--trace-sci4-in). Same as boot's.
	// Call before boot(), like set_plg_card.
	void set_plg_trace(const std::string &path) { m_plg_trace_path = path; }
	void set_plg_tx_trace(const std::string &path) { m_plg_tx_path = path; }
	const std::string &plg_message() const { return m_plg_msg; }
	bridge   &br;
	midi_in  &midi;        // MIDI IN A（パート 1-16）
	// B-D。B は実機の 2 つめの DIN、C・D は USB だけの口（パート 33-64）。
	// [0] は使わない（midi が A）
	midi_in  *midi_p[mu2000::MIDI_PORTS] = {};
	midi_out *mout = nullptr;     // MIDI THRU A（A で受けたものを外へ）
	midi_out *mout_b = nullptr;   // MIDI THRU B（B で受けたものを外へ）
	// MIDI OUT。MU2000 が自分で送り出すもの（XG のダンプ要求への返事など）。
	// これを loopMIDI 越しに外のエディタへ返すと、外から読み書きできる
	midi_out *mout_mu = nullptr;
	audio_in *ain = nullptr;          // A/D INPUT に入れる音（無ければ無音）

	std::atomic<int> state{0};        // 0 起動中 / 1 準備完了 / 2 だめ
	// THRU A / B の流量の上限。MIDI の輪で溢れたものを実機へ流さない（midi_guard.h）
	thru_guard guard_a, guard_b;
	// fill() が機械に触っている最中か。起動し直すときはこれが落ちるのを待つ
	std::atomic<bool> in_fill{false};
	// SmartMedia を差す・抜く・書き戻す間は、音声の糸が機械を回さないようにする
	std::mutex card_lock;
	bool use_nvram = false;           // 覚えている設定で起動するか（窓を出すときだけ）
	// 音の出口。false = デジタル（S/PDIF と同じ。DPCM の直流も残る）、true = アナログ（直流を切る。analog_out.h）
	std::atomic<bool> analog{false};
	// エフェクトを C++ で鳴らす軽量モード（doc/native-dsp.md）。0 切 / 1 / 2。
	// 画面からはここへ頼むだけで、切り替えは音声の糸が fill() の頭で行う
	std::atomic<int>  want_native_fx{-1};
	std::atomic<int>  native_fx{0};
	// **firmware を走らせない口**（doc/native-engine.md の段 2）。0 切 / 1 入。
	// まだ音が実機とはっきり違うので、聞き比べのために窓から入切できるようにしてある。
	// 軽量モードと同じく、切り替えは音声の糸が fill() の頭で行う
	std::atomic<int>  want_native_engine{-1};
	std::atomic<int>  native_engine{0};
	std::string      message = "起動中...";

	driver drv;

	engine(bridge &b, midi_in &m) : br(b), midi(m) {}

	bool load(const std::string &dir)
	{
		if (!mu.load_program(dir + "/mu2000_flash.bin")) { message = mu.error(); return false; }
		if (!mu.load_wave(dir + "/dump"))                { message = mu.error(); return false; }
		if (!mu.load_sintab(dir + "/standin/sin-table.bin"))
			std::fprintf(stderr, UI_TEXT(engine_warn_fmt, "warning: %s\n"), mu.error().c_str());
		if (!mu.load_lcd_font(dir + "/hd44780u_b04.bin") &&
		    !mu.load_lcd_font(dir + "/standin/hd44780u_b04.bin"))
			std::fprintf(stderr, UI_TEXT(engine_warn_fmt, "warning: %s\n"), mu.error().c_str());
		return true;
	}

	// 起動（実機と同じ空回し）。窓を出したあと別スレッドで進める
	bool boot()
	{
		mu.set_threaded(true);
		if (use_nvram && smu2000::nvram::load(mu))
			std::printf(UI_TEXT(engine_nvram_fmt, "Settings: %s\n"), smu2000::nvram::path(mu).c_str());
		// 鍵は起動に使うワーク RAM も混ぜるので、reset() の前に作る
		u64 key = smu2000::bootcache::key(mu);
		// **The PLG card goes in before reset().** The scan is part of the boot
		// sequence, so a card that is not in the slot when the machine starts is
		// never seen: boot --plg-late inserts one 10 s in and the card is sent
		// nothing at all. There is no insert-while-running, unlike SmartMedia.
		mu.set_plg_host(&m_plg);
		if (!m_plg_tx_path.empty()) {
			m_plg_tx = std::fopen(m_plg_tx_path.c_str(), "w");
			if (m_plg_tx)
				mu.set_sci4_trace(m_plg_tx);
		}
		if (!m_plg_trace_path.empty()) {
			m_plg_trace = std::fopen(m_plg_trace_path.c_str(), "w");
			if (m_plg_trace)
				mu.set_sci4_in_trace(m_plg_trace);
		}
		if (!m_plg_path.empty() || m_plg_builtin >= 0) {
			std::string err;
			// A card's own report at teardown is the only view of what it made of the
			// exchange, and the front end is where that is easiest to lose.
			m_plg.set_log_sink([](const std::string &msg) { std::printf("[card] %s\n", msg.c_str()); });
			// **The range is checked here, not trusted.** `boot` refuses a bad
			// number, but the gui takes it from a settings file where anything can
			// be written - and builtin_card() indexes straight into a table, so an
			// unchecked index is a bus error, which is how this was found.
			bool ok = false;
			const int slot = mu.plg_slot_for(PLG_MODEL_ANY);
			if (m_plg_builtin >= 0) {
				if (m_plg_builtin < plg::builtin_count())
					ok = m_plg.insert_builtin(slot, plg::builtin_card(m_plg_builtin), err);
				else
					err = "built-in card number out of range";
			} else {
				ok = m_plg.insert(slot, m_plg_path, err);
			}
			m_plg_msg = ok ? std::string() : err;
			std::printf("PLG slot %d: %s\n", slot + 1,
			            ok ? (m_plg_builtin >= 0 ? "(built in)" : m_plg_path.c_str())
			               : err.c_str());
		}
		mu.reset();
		// 前に起動し切った姿を取ってGrossmaidwantedら、そこから始める（bootcache.h）。
		// 回した結果と 1 ビットも違わないので、音は同じ。
		// **reset() のあとで読むこと**（タイマが揃っていないと形が合わない）
		//
		// **カードが入っているときは使わない。** 写しは「カード 無しの起動の
		// 最後」の機械の状態なので、そこから再開すると PLG の探索が �わ � たない。
		// カードを入れ���のにランプが点かないのは��のせい（実測: gui は
		// 「PLG slot 1: (built in)」を出しながら PLG-1 が消えていた）。
		const bool have_card = !m_plg_path.empty() || m_plg_builtin >= 0;
		if (have_card)
			key = 0;
		if (key && smu2000::bootcache::load(mu, key)) {
			std::printf(UI_TEXT(engine_boot_cached_fmt, "Booted from snapshot (%s)\n"), smu2000::bootcache::path(key).c_str());
			publish();
			return true;
		}
		const size_t limit = size_t(30.0 * AUDIO_RATE);
		size_t i = 0;
		s32 l, r;
		for (; i < limit && !mu.midi_ready(); i++)
			mu.run_sample(l, r);
		// Whether the host still trusts the card: a faulted card is skipped rather
		// than called, so its line reads as permanently idle - which is exactly what
		// "receives everything, sends nothing" looks like.
		if (!m_plg_path.empty() || m_plg_builtin >= 0)
			for (int k = 0; k < plg::host::SLOTS; k++)
				if (m_plg.present(k))
					std::printf("PLG slot %d: running=%d faulted=%d msg=%s\n", k + 1,
					            int(m_plg.running(k)), int(m_plg.faulted(k)),
					            m_plg.message(k).c_str());
		if (i >= limit) {
			message = UI_TEXT(engine_boot_failed, "Boot failed");
			return false;
		}
		// key == 0 means a card is in the slot, and then there is nothing to cache
		// under: the snapshot would be of a machine that scanned its slots, which
		// is not the machine a cardless boot produces. Saving under 0 would also
		// write a file called 0000000000000000.bin into the settings directory.
		if (key && smu2000::bootcache::save(mu, key))
			std::printf(UI_TEXT(engine_boot_saved_fmt, "Saved boot snapshot: %s\n"), smu2000::bootcache::path(key).c_str());
		publish();
		return true;
	}

	// **NVRAM を残す前に、firmware を落ち着かせる。**
	// NVRAM はワーク RAM 256KB を丸ごと残すので、firmware の生きた状態も
	// 一緒に残る。native の口では firmware をほとんど回さないため、
	// そのまま残すと firmware から見て中途半端な状態が保存され、
	// 次に開いたときは曲の頭からおかしくなる。
	// native を切って（鳴っている音は離される）、少し回してから残す
	void settle_for_save()
	{
		if (!mu.native_engine())
			return;
		mu.set_native_engine(0);
		native_engine.store(0);
		s32 l, r;
		for (int i = 0; i < int(0.5 * AUDIO_RATE); i++)
			mu.run_sample(l, r);
	}

	// 工場出荷状態に戻す。覚えている設定を捨てて電源を入れ直す。
	// 音声の糸が機械から手を離すのを待ってから触る
	void factory_reset()
	{
		state.store(0);
		message = UI_TEXT(engine_resetting, "Factory resetting...");
		publish();
		while (in_fill.load())
			smu2000::sleep_ms(1);

		const std::vector<u8> zero(mu.nvram().size(), 0);
		mu.set_nvram(zero.data(), zero.size());
		const bool keep = use_nvram;
		use_nvram = false;
		const bool ok = boot();
		use_nvram = keep;
		if (!ok) {
			state.store(2);
			publish();
			return;
		}
		// すぐ残す。ここで落ちても前の設定に戻らないように
		smu2000::nvram::save(mu);
		std::printf("%s", UI_TEXT(engine_reset_done, "Factory reset done\n"));
		std::fflush(stdout);
		state.store(1);
		publish();
	}

	void publish()
	{
		if (state.load() == 1)
			driver::publish_now(mu, br, true, nullptr);
		else
			driver::publish_message(br, message.c_str());
	}

	// 音声デバイスに頼まれた分だけ進める
	void fill(s16 *out, u32 n)
	{
		// 非正規化数を 0 に丸める（軽量モード用。出るときに元へ戻す）
		const smu2000::denormals_off no_denormals;
		const std::lock_guard<std::mutex> hold(card_lock);
		// 先に「触っている」を立ててから state を見る。逆にすると、見た直後に
		// 起動し直しが始まって、両方が機械に触ってしまう
		in_fill.store(true);
		if (state.load() != 1) {
			in_fill.store(false);
			std::memset(out, 0, size_t(n) * 4);
			return;
		}

		// 軽量モードの切り替えは、機械を回していない今のうちに
		if (const int want = want_native_fx.exchange(-1); want >= 0) {
			mu.set_native_fx(want);
			native_fx.store(want);
		}
		// native の口の入切も同じところで。入れ直すと写し取りは白紙に戻るので、
		// その音色の 1 音目はまた firmware が鳴らす
		if (const int want = want_native_engine.exchange(-1); want >= 0) {
			mu.set_native_engine(want);
			native_engine.store(want);
		}

		guard_a.refill(n, AUDIO_RATE);
		guard_b.refill(n, AUDIO_RATE);

		drv.apply_buttons(mu, br);
		// 画面から出したものも、外の MIDI 出力へ流す（実機の THRU）
		drv.pump_midi(mu, br, [this](u8 v) { if (mout && guard_a.pass(v)) mout->send(v); });
		drv.pump_wheel(mu, br);

		u8 b;
		while (midi.pop(b)) {
			drv.watch(b, mu.midi_in(b, 0));
			if (mout && guard_a.pass(b)) mout->send(b);
		}
		// B は実機の 2 つめの DIN（内蔵 SCI ch1）。パート 17-32 に届く。
		// THRU も口ごとに分ける。A で受けたものは MIDI OUT A、
		// B で受けたものは MIDI OUT B へ。混ぜると、外に繋いだ音源で
		// パートの割り振りが崩れる
		// C・D は実機では USB だけの口で、外へ出す THRU の端子も無い
		for (int p = 1; p < mu2000::MIDI_PORTS; p++) {
			if (!midi_p[p])
				continue;
			while (midi_p[p]->pop(b)) {
				drv.watch(b, mu.midi_in(b, p));
				if (p == 1 && mout_b && guard_b.pass(b)) mout_b->send(b);
			}
		}

		const float g = br.gain();
		// アナログにした最初のブロックで、前に使ったときの状態を捨てる
		const bool to_analog = analog.load(std::memory_order_relaxed);
		if (to_analog && !m_analog_was)
			m_dc.reset();
		m_analog_was = to_analog;

		for (u32 i = 0; i < n; i++) {
			s32 l = 0, r = 0;
			if (ain) {
				s32 a1, a2;
				ain->pop(a1, a2);
				mu.set_audio_input(a1, a2);
			}
			mu.run_sample(l, r);
			// 直流を切るのは音量のつまみより前（DAC のすぐ後ろ）。デジタルのときは何もしない
			if (to_analog) {
				l = s32(std::lrint(m_dc.run(0, l)));
				r = s32(std::lrint(m_dc.run(1, r)));
			}
			l = s32(l * g) * 32768 / mu2000::DAC_FULL_SCALE;
			r = s32(r * g) * 32768 / mu2000::DAC_FULL_SCALE;
			out[i * 2 + 0] = s16(l < -32768 ? -32768 : l > 32767 ? 32767 : l);
			out[i * 2 + 1] = s16(r < -32768 ? -32768 : r > 32767 ? 32767 : r);
		}

		// firmware が送り出したもの。画面（パラメータの層）と MIDI OUT の口へ。
		// 出口が無くても取り出しておく（溜めを空ける）
		drv.pump_out(mu, br, [this](u8 v) { if (mout_mu) mout_mu->send(v); });

		drv.publish(mu, br, n, AUDIO_RATE, true, nullptr);
		in_fill.store(false);
	}

private:
	smu2000::analog_out m_dc{ AUDIO_RATE };
	bool m_analog_was = false;
};

} // namespace ui

#endif // S_MU2000_UI_ENGINE_H
