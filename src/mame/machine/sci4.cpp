// license:BSD-3-Clause
// copyright-holders:Olivier Galibert

// Yamaha SCI4 / XV833A00, 7-lines serial chip with 4 multiplexed on one and the other 3 separated

#include "sci4.h"

sci4_device::sci4_device(const machine_config &mconfig, const char *tag, device_t *owner, uint32_t clock) :
	device_t(mconfig, SCI4, tag, owner, clock),
	m_tx(*this),
	m_irq(*this)
{
}


// S-MU2000: MAME の address_map を素の振り分けにした。
// 元の並びは
//   0x00-0x3f  default_r/default_w
//   0x00 data / 0x01 enable / 0x02 status / 0x03 datamode / 0x05 reset
//              （いずれも .select(0x18)、つまり 0x08 刻みで 4 チャンネル）
//   0x20       target_w
// select が付いたものは offset をそのまま渡す（受け側が >>3 している）。

u8 sci4_device::read8(offs_t offset)
{
	if(offset < 0x20) {
		switch(offset & 7) {
		case 0: return data_r(offset);
		case 1: return enable_r(offset);
		case 2: return status_r(offset);
		case 3: return datamode_r(offset);
		case 5: return reset_r(offset);
		}
	}
	return default_r(offset);
}

void sci4_device::write8(offs_t offset, u8 data)
{
	if(offset == 0x20) {
		target_w(data);
		return;
	}
	if(offset < 0x20) {
		switch(offset & 7) {
		case 0: data_w(offset, data); return;
		case 1: enable_w(offset, data); return;
		case 3: datamode_w(offset, data); return;
		}
	}
	default_w(offset, data);
}


void sci4_device::device_start()
{
	save_item(NAME(m_rx));
	save_item(NAME(m_enable));
	save_item(NAME(m_status));
	save_item(NAME(m_datamode));
	save_item(NAME(m_div));
	save_item(NAME(m_cur_rx));
	save_item(NAME(m_tdr));
	save_item(NAME(m_tsr));
	save_item(NAME(m_tdr_full));
	save_item(NAME(m_tx_step));
	save_item(NAME(m_tx_active));
	save_item(NAME(m_rdr));
	save_item(NAME(m_rsr));
	save_item(NAME(m_rdr_full));
	save_item(NAME(m_rx_step));
	save_item(NAME(m_rx_active));

	save_item(NAME(m_targets));

	for(u32 i=0; i != 4; i++) {
		m_tx_timer[i] = timer_alloc(FUNC(sci4_device::tx_tick), this);
		m_rx_timer[i] = timer_alloc(FUNC(sci4_device::rx_tick), this);
	// S-MU2000: only armed when a line source is set (set_line_source).
	m_line_timer = timer_alloc(FUNC(sci4_device::line_tick), this);
	}
}

void sci4_device::device_reset()
{
	std::fill(m_rx.begin(), m_rx.end(), 1);
	std::fill(m_enable.begin(), m_enable.end(), 0);
	std::fill(m_status.begin(), m_status.end(), 0);
	std::fill(m_datamode.begin(), m_datamode.end(), 0);
	std::fill(m_div.begin(), m_div.end(), 0);
	std::fill(m_cur_rx.begin(), m_cur_rx.end(), 1);
	std::fill(m_tdr.begin(), m_tdr.end(), 0);
	std::fill(m_tsr.begin(), m_tsr.end(), 0);
	std::fill(m_tdr_full.begin(), m_tdr_full.end(), 0);
	std::fill(m_tx_step.begin(), m_tx_step.end(), 0);
	std::fill(m_tx_active.begin(), m_tx_active.end(), 0);
	std::fill(m_rdr.begin(), m_rdr.end(), 0);
	std::fill(m_rsr.begin(), m_rsr.end(), 0);
	std::fill(m_rdr_full.begin(), m_rdr_full.end(), 0);
	std::fill(m_rx_step.begin(), m_rx_step.end(), 0);
	std::fill(m_rx_active.begin(), m_rx_active.end(), 0);

	m_targets = 0;

	// S-MU2000: the host installs its line source in its constructor, which is
	// before this device's timers exist, so the poll is armed here rather than
	// there. Doing it on every reset also picks up the divisor the firmware
	// programmed, which is not known any earlier.
	if(m_line_fn && m_line_timer) {
		const u32 div = m_div[3] ? m_div[3] : 0x100;
		m_line_timer->adjust(attotime::from_ticks(div * 8, clock()));
	}
}

void sci4_device::do_rx_w(int sci, int state)
{
	if(sci >= 30)
		sci = sci - 30 + 3;

	m_rx[sci] = state;
	if(sci < 3) {
		if(state != m_cur_rx[sci]) {
			m_cur_rx[sci] = state;
			rx_changed(sci);
		}
	}
	u8 rx = ((((m_rx[6] << 3) | (m_rx[5] << 2) | (m_rx[4] << 1) | m_rx[3]) | ~(m_targets >> 4)) & 0xf) == 0xf;
	if(rx != m_cur_rx[3]) {
		m_cur_rx[3] = rx;
		rx_changed(3);
	}
}

// S-MU2000: pull a line from a source instead of from a wire, and hand it to
// do_rx_w() as if it had arrived on one. That is the whole point: the edge
// detection, the multiplexed-line composite and the start-bit handling are the
// chip's own, so none of them is copied here. Only where the level comes from
// changes. See set_line_source() in the header.
void sci4_device::pull_line(int sci)
{
	if(!m_line_fn)
		return;
	do_rx_w(sci, m_line_fn(m_line_ctx, sci) ? 1 : 0);
}

// S-MU2000: the free-running poll. Nothing calls do_rx_w() for a line that comes
// from a card, because nothing pushes it - the card is *stepped*, not listened
// to, and it emits one bit per step. So the poll rate is the bit rate exactly:
// twice per bit would make the card talk at twice the rate the chip samples, and
// the chip would read every other bit. wait() computes the period the same way,
// from the divisor the firmware programmed, so this tracks whatever rate is set
// rather than assuming one - which is how a card set to 500 kHz ends up at 2 us
// a bit without that number appearing anywhere here.
TIMER_CALLBACK_MEMBER(sci4_device::line_tick)
{
	if(m_line_fn)
		for(int i = 0; i < 7; i++)
			pull_line(i);
	const u32 div = m_div[3] ? m_div[3] : 0x100;
	m_line_timer->adjust(attotime::from_ticks(div * 8, clock()));
}

// S-MU2000: arm the poll when a source appears, and stop it when one goes away.
// Nothing else in the chip knows this timer exists, so a machine with no card
// behaves exactly as before - which is the case for every tool here today.
void sci4_device::set_line_source(line_fn fn, void *ctx)
{
	m_line_fn  = fn;
	m_line_ctx = ctx;
	if(!m_line_timer)
		return;
	if(!fn) {
		m_line_timer->adjust(attotime::never);
		return;
	}
	const u32 div = m_div[3] ? m_div[3] : 0x100;
	m_line_timer->adjust(attotime::from_ticks(div * 8, clock()));
}

void sci4_device::default_w(offs_t offset, u8 data)
{
	logerror("reg_w %02x, %02x (%s)\n", offset, data, machine().describe_context());
}

u8 sci4_device::default_r(offs_t offset)
{
	logerror("reg_r %02x (%s)\n", offset, machine().describe_context());
	return 0;
}

void sci4_device::datamode_w(offs_t slot, u8 data)
{
	m_datamode[slot >> 3] = data;
}

u8 sci4_device::datamode_r(offs_t slot)
{
	return m_datamode[slot >> 3];
}

void sci4_device::data_w(offs_t slot, u8 data)
{
	slot >>= 3;
	if(m_datamode[slot] == 0x80) {
		m_div[slot] = data;
		if(data)
			logerror("channel %d baud rate %dHz\n", slot, clock()/16/data);
		else
			logerror("channel %d off\n");
	} else if(m_datamode[slot] & 2)
		fifo_w(slot, data);
}

u8 sci4_device::data_r(offs_t slot)
{
	slot >>= 3;
	if(m_datamode[slot] == 0x80)
		return m_div[slot];
	else if(m_datamode[slot] & 1)
		return fifo_r(slot);
	else
		return 0;
}

void sci4_device::enable_w(offs_t slot, u8 data)
{
	slot >>= 3;
	u8 old = m_enable[slot];
	m_enable[slot] = data;
	if((data & 2) && !(old & 2))
		tx_enabled(slot);

	else if(!(data & 2) && (old & 2)) {
		if(m_status[slot] == 2) {
			m_status[slot] = 0;
			m_irq[slot](0);
		}
	}
}

u8 sci4_device::enable_r(offs_t slot)
{
	return m_enable[slot >> 3];
}

u8 sci4_device::status_r(offs_t slot)
{
	return m_status[slot >> 3];
}

u8 sci4_device::reset_r(offs_t slot)
{
	slot >>= 3;
	m_status[slot] = 0;
	m_tx_timer[slot]->adjust(attotime::never);
	m_tx_active[slot] = 0;
	tx_set(slot, 1);
	m_tdr_full[slot] = 0;
	m_irq[slot](0);
	return 0;
}


void sci4_device::target_w(u8 data)
{
	m_targets = data;
	u8 rx = ((((m_rx[6] << 3) | (m_rx[5] << 2) | (m_rx[4] << 1) | m_rx[3]) | ~(m_targets >> 4)) & 0xf) == 0xf;
	if(rx != m_cur_rx[3]) {
		m_cur_rx[3] = rx;
		rx_changed(3);
	}
	for(u32 i=0; i != 4; i++)
		if(!(m_targets & (1<<i)))
			m_tx[i+3](1);
}

void sci4_device::tx_set(int chan, int state)
{
	if(chan < 3)
		m_tx[chan](state);
	else
		for(u32 i=0; i != 4; i++)
			if((m_targets & (1<<i)))
				m_tx[i+3](state);

}

void sci4_device::fifo_w(int chan, u8 data)
{
	if(m_tdr_full[chan] && (m_enable[chan] & 4)) {
		m_status[chan] = 6;
		m_irq[chan](1);

	} else {
		m_tdr[chan] = data;
		m_tdr_full[chan] = 1;
		if(m_status[chan] == 2) {
			m_status[chan] = 0;
			m_irq[chan](0);
		}
		if(!m_tx_active[chan] && (m_enable[chan] & 2))
			tx_start(chan);
	}
}

u8 sci4_device::fifo_r(int chan)
{
	m_rdr_full[chan] = 0;
	if(m_status[chan] == 4) {
		m_status[chan] = 0;
		m_irq[chan](0);
	}
	return m_rdr[chan];
}

void sci4_device::rx_changed(int chan)
{
	if(!m_rx_active[chan] && !m_cur_rx[chan] && (m_enable[chan] & 1)) {
		m_rx_active[chan] = 1;
		m_rx_step[chan] = 0;
		m_rsr[chan] = 0;
		wait(1, 0, chan);
	} else if(m_rx_active[chan]) {
		if(m_rx_step[chan] == 0) {
			// Start bit gone before half-time
			m_rx_active[chan] = 0;
			m_rx_timer[chan]->adjust(attotime::never);
		} else
			// Force a precise resync
			wait(1, 0, chan);
	}
}

void sci4_device::tx_enabled(int chan)
{
	if(m_tdr_full[chan])
		tx_start(chan);
	else {
		m_status[chan] |= 2;
		m_irq[chan](1);
	}
}

std::string sci4_device::chan_id(u8 chan, u8 target)
{
	return chan < 3 ? util::string_format("%d", chan) : util::string_format("3:%s%s%s%s",
																			target & 1 ? "0" : "",
																			target & 2 ? "1" : "",
																			target & 4 ? "2" : "",
																			target & 8 ? "3" : "");
}

void sci4_device::tx_start(int chan)
{
	m_tx_active[chan] = 1;
	m_tsr[chan] = m_tdr[chan];
	m_tdr_full[chan] = 0;
	m_status[chan] |= 2;
	m_irq[chan](1);

	logerror("chan %s transmit %02x\n",
			 chan_id(chan, m_targets),
			 m_tsr[chan]);

	tx_set(chan, 0);
	m_tx_step[chan] = 0;
	wait(0, 1, chan);
}

void sci4_device::wait(int timer, int full, int chan)
{
	u32 div = m_div[chan] ? m_div[chan] : 0x100;
	u32 cycles = div*(full ? 16 : 8);
	(timer ? m_rx_timer : m_tx_timer)[chan]->adjust(attotime::from_ticks(cycles, clock()), chan);
}

TIMER_CALLBACK_MEMBER(sci4_device::tx_tick)
{
	u32 step = m_tx_step[param]++;
	if(step < 9) {
		const int level = (step == 8) ? 1 : ((m_tsr[param] >> step) & 1);
		tx_set(param, level);
		// S-MU2000: tell a PLG card what bit is going out, and when.
		//
		// MAME's plg1x0 interface hands a card the line and nothing else - no clock,
		// no bit index - and its cards cope because they are real SCI peripherals
		// that count their own bit clock and sample by phase. A card written against
		// this ABI has no such clock, and an edge-driven receiver on this line is
		// lost immediately: the firmware writes the target register often, and
		// target_w() drives the *disabled* multiplexed lines high, so every one of
		// those writes is an edge on the card's line with no byte attached. A card
		// measuring a real MU2000 was handed a dozen such edges before the host had
		// said anything at all, and every message after that was a byte out.
		//
		// So the bit goes across as a bit, at the moment it is shifted, with its
		// place in the byte. A card can then frame the byte itself and ignore
		// anything between bits - which is what the real cards do, and what the
		// interface has always implied.
		// Once per **selected line**, not once per channel: the three PLG slots are
		// one SCI4 channel (3) told apart by the target register, which is why the
		// firmware broadcasts and cannot tell which board answered. Delivering per
		// channel would hand the bit to one arbitrary slot.
		if(m_tx_notify && param == 3) {
			for(int line = 0; line != 4; line++)
				if(m_targets & (1 << line))
					m_tx_notify(m_tx_notify_ctx, line, level, int(step));
		}
		wait(0, 1, param);

	} else {
		if((m_enable[param] & 2) && m_tdr_full[param])
			tx_start(param);
		else
			m_tx_active[param] = 0;
	}
}

TIMER_CALLBACK_MEMBER(sci4_device::rx_tick)
{
	// S-MU2000: the bit clock is here, so a line that comes from a card is
	// sampled here too - before the value is read, and before the start-bit
	// check that the step-0 comment below talks about.
	pull_line(param);
	u32 step = m_rx_step[param]++;
	if(step == 0)
		wait(1, 1, param); // Value already checked in rx_changed
	else if(step < 9) {
		if(m_cur_rx[param])
			m_rsr[param] |= 1 << (step - 1);
		wait(1, 1, param);

	} else {
		if(!m_rx[param]) {
			logerror("chan %s framing error/break\n", chan_id(param, m_targets >> 4));
			// S-MU2000: the same event, on a trace the caller opened.
			if(m_rx_trace)
				std::fprintf(m_rx_trace, "SCI4I %s framing error\n",
				             chan_id(param, m_targets >> 4).c_str());
		}
		else {
			logerror("chan %s recieved %02x\n", chan_id(param, m_targets >> 4), m_rsr[param]);
			// S-MU2000: and the byte itself, which is the whole point of the trace.
			if(m_rx_trace)
				std::fprintf(m_rx_trace, "SCI4I %s rx %02x\n",
				             chan_id(param, m_targets >> 4).c_str(), m_rsr[param]);
			m_rx_active[param] = 0;
			m_rdr[param] = m_rsr[param];
			if(m_rdr_full[param] && (m_enable[param] & 4))
				m_status[param] = 6;
			else
				m_status[param] = 4;
			m_irq[param](1);
		}
	}
}

DEFINE_DEVICE_TYPE(SCI4, sci4_device, "sci4", "Yamaha SCI4 quad-serial gate array")

void sci4_device::state(state_io &s)
{
	s.tag("sci4");
	s.stdarr(m_rx);
	s.stdarr(m_enable); s.stdarr(m_status); s.stdarr(m_datamode);
	s.stdarr(m_div); s.stdarr(m_cur_rx);
	s.stdarr(m_tdr); s.stdarr(m_tsr); s.stdarr(m_tdr_full);
	s.stdarr(m_tx_step); s.stdarr(m_tx_active);
	s.stdarr(m_rdr); s.stdarr(m_rsr); s.stdarr(m_rdr_full);
	s.stdarr(m_rx_step); s.stdarr(m_rx_active);
	s.v(m_targets);
}
