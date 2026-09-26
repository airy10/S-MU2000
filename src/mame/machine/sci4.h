// license:BSD-3-Clause
// copyright-holders:Olivier Galibert

// Yamaha SCI4 / XV833A00, 7-lines serial chip with 4 multiplexed on one and the other 3 separated

#ifndef MAME_MACHINE_SCI4_H
#define MAME_MACHINE_SCI4_H

#pragma once

// S-MU2000: MAME 本体の代わりに互換層を使う
#include "state.h"
#include "../../compat/mamecompat.h"

class sci4_device : public device_t
{
public:
	// 状態の保存と復元（src/state.h）
	void state(state_io &s);

	sci4_device(const machine_config &mconfig, const char *tag, device_t *owner, uint32_t clock = 8000000);

	// sci port numbers are 0..2 and 30..33
	template<int Sci> void rx_w(int state) { do_rx_w(Sci, state); }
	template<int Sci> auto write_tx() { if(Sci < 3) return m_tx[Sci].bind(); else return m_tx[Sci - 30 + 3].bind(); }

	// irq line numbers are 0..3
	template<int irq> auto write_irq() { return m_irq[irq].bind(); }

	// S-MU2000: an optional source for a line, polled **at the chip's own bit
	// clock** (doc/plg-cards.md 4). A PLG card drives its TX line from its own
	// firmware, and the host has no other way to sample it in step: a bit at
	// 500 kHz is 2 us and an audio sample is 22.7 us, so polling once per sample
	// puts a whole byte's ten edges inside one instant and the chip reads the
	// same bit ten times. Pulling the line where the bit clock already lives is
	// the only place the timing is right.
	//
	// The source replaces do_rx_w for that channel: the edge is detected where the
	// bit is sampled and rx_changed() is called exactly as do_rx_w would, so the
	// start-bit handling and the resync stay the chip's own rather than a copy.
	using line_fn = int (*)(void *ctx, int sci);
	void set_line_source(line_fn fn, void *ctx);

	// S-MU2000: report what arrived on a line, for `boot --trace-sci4-in`.
	// MAME's sci4 logs the same two events with logerror, which goes
	// nowhere in a build with no debugger. This is the same question from the
	// other end: did the card's bytes arrive intact?
	void set_rx_trace(std::FILE *f) { m_rx_trace = f; }

	// S-MU2000: a PLG card's receive path, called once per bit as the bit is
	// shifted out, with its place in the byte. See the comment at the call site
	// in sci4.cpp: the line alone is not enough, because the firmware's target
	// writes put edges on it that belong to no byte.
	using tx_notify_fn = void (*)(void *ctx, int sci, int level, int bit);
	void set_tx_notify(tx_notify_fn fn, void *ctx)
	{ m_tx_notify = fn; m_tx_notify_ctx = ctx; }

	// S-MU2000: counters for the transmit chain, because "the card is handed nine
	// bits when nine bytes were written" has to be pinned to one of three places -
	// tx_start not re-entered, tx_tick not firing, or the notify gated off - and
	// guessing between them is how the last three rounds went.
	u64 dbg_tx_start(int i) const { return m_dbg_tx_start[i]; }
	u64 dbg_tx_tick(int i) const { return m_dbg_tx_tick[i]; }
	u64 dbg_notify(int i) const { return m_dbg_notify[i]; }
	// Loop entries (step < 9), as opposed to ticks: the difference is the end-of-
	// byte branch, and without it the arithmetic on the other two is guesswork.
	u64 dbg_tx_loop(int i) const { return m_dbg_tx_loop[i]; }

	// S-MU2000: address_map の代わりに素の振り分け。中身は sci4.cpp の末尾
	u8   read8 (offs_t offset);
	void write8(offs_t offset, u8 data);

protected:
	virtual void device_start() override ATTR_COLD;
	virtual void device_reset() override ATTR_COLD;

protected:
	devcb_write_line::array<7> m_tx;
	devcb_write_line::array<4> m_irq;

	emu_timer *m_tx_timer[4];
	emu_timer *m_rx_timer[4];
	emu_timer *m_line_timer = nullptr;   // S-MU2000: the free-running line poll
	std::FILE *m_rx_trace = nullptr;      // S-MU2000
	u64 m_dbg_tx_start[4] = {};   // S-MU2000
	u64 m_dbg_tx_tick[4] = {};    // S-MU2000
	u64 m_dbg_notify[4] = {};     // S-MU2000, per multiplexed line
	u64 m_dbg_tx_loop[4] = {};    // S-MU2000
	tx_notify_fn m_tx_notify = nullptr;  // S-MU2000
	void *m_tx_notify_ctx = nullptr;     // S-MU2000

	std::array<u8, 7> m_rx;
	std::array<u8, 4> m_enable, m_status, m_datamode, m_div, m_cur_rx;
	std::array<u8, 4> m_tdr, m_tsr, m_tdr_full, m_tx_step, m_tx_active;
	std::array<u8, 4> m_rdr, m_rsr, m_rdr_full, m_rx_step, m_rx_active;
	u8 m_targets = 0;

	// S-MU2000: the line source, see set_line_source(). Spelled out rather than
	// using the typedef below, because the member is declared before it.
	int (*m_line_fn)(void *ctx, int sci) = nullptr;
	void *m_line_ctx = nullptr;

	void do_rx_w(int sci, int state);
	void pull_line(int sci);   // S-MU2000: see set_line_source()
	TIMER_CALLBACK_MEMBER(line_tick);   // the free-running poll, at half a bit

	// S-MU2000: an optional source for a line, polled **at the chip's own bit

	void default_w(offs_t offset, u8 data);
	u8 default_r(offs_t offset);

	void datamode_w(offs_t slot, u8 data);
	u8 datamode_r(offs_t slot);
	void data_w(offs_t slot, u8 data);
	u8 data_r(offs_t slot);
	void enable_w(offs_t slot, u8 data);
	u8 enable_r(offs_t slot);
	u8 status_r(offs_t slot);
	u8 reset_r(offs_t slot);

	void target_w(u8 data);

	std::string chan_id(u8 chan, u8 target);

	void wait(int timer, int full, int chan);
	void tx_enabled(int chan);
	void tx_set(int slot, int state);
	void tx_start(int chan);
	void rx_changed(int chan);
	void fifo_w(int chan, u8 data);
	u8 fifo_r(int chan);

	TIMER_CALLBACK_MEMBER(tx_tick);
	TIMER_CALLBACK_MEMBER(rx_tick);
};

DECLARE_DEVICE_TYPE(SCI4, sci4_device)

#endif // MAME_MACHINE_SCI4_H
