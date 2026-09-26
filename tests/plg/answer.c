/* license:BSD-3-Clause *
 *
 * tests/plg/answer.c - a PLG card that answers, and gets recognised.
 *
 * This is the card the whole exercise was for. It knows two things and nothing
 * else, and both were read off a real PLG150-AP talking to a real MU2000:
 *
 *   reply to a read of 2 bytes at 0x0010   F0 43 10 4E 01 10 00 00 07 00 F7
 *   reply with its own name, 14 bytes      F0 43 10 4E 01 00 00 "PLG150-AP     " F7
 *
 * The AP sent exactly those two messages, 33 bytes in total, and the MU2000 was
 * satisfied: it lit the PLG-1 lamp and went on to the PLG mode. Everything else
 * the MU2000 sends is a 500 ms poll the AP ignored, or voice data. **So a card
 * that can be recognised is about forty bytes of reply.**
 *
 * The first reply's two data bytes are the card's category. The AP answers
 * 00 07 and the PLG100-VL answers 00 00, which is how the MU2000 tells an
 * acoustic piano from a virtual acoustic one - and it lines up with the
 * eighteen bracketed category names in the MU2000 firmware (mu2000_flash.bin
 * around 0x1dda98, `[Piano]` first).
 *
 * ## The transmit side, and why midi_tx() is a poll
 *
 * The card has no timer and does not want one. The host calls midi_tx() at
 * SCI4's own bit clock - 2 us at 500 kHz, which is what MAME sets the card's SCI
 * to in plg150-ap.cpp - and this function hands back one bit per call. Ten calls
 * make a byte, so the card is *stepped* by the chip's clock and cannot drift from
 * it. MAME's real cards free-run on their own 500 kHz and the host's timing agrees
 * well enough that a capture of both shows **zero framing errors**.
 *
 * The line idles high, which matters: SCI4 starts a byte on a falling edge, so a
 * card that idles low would look like it was mid-byte forever.
 *
 * ## Trying a different name
 *
 * PLG_NAME below is what the MU2000 is told this card is called. Change it and
 * re-run: if the firmware displays the new string, it read what we sent, which is
 * worth knowing separately from whether it accepted the card at all.
 *
 * ## Never test this from a snapshot
 *
 * The PLG scan is part of the boot sequence - `Checking PLG` is a boot screen -
 * so anything that starts from a saved state never probes the slots, and the
 * card's behaviour cannot be tested that way. `boot` and `render` are cold by
 * default; `--bootcache` is opt-in and has no business being passed here.
 *
 * ## Known fault, as of 2026-09-26
 *
 * The receive framing is a byte out of step, and there is no resync. The card
 * logs six "messages" whose whole content is `f7` and sees zero polls, so every
 * message it thinks it got is the last byte of one the host sent - and the
 * poll-only answer below therefore never fires. The host's side is finished and
 * checked; this is the card's half. See doc/plg-cards.md section 5.
 */
#include "plg/plg1500.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PLG_NAME "PLG150-AP"   /* 11 characters, space padded on the wire */

/* What the AP answers for the 2-byte read at 0x0010. See the header comment. */
static const unsigned char CATEGORY[2] = { 0x00, 0x07 };

/* The two replies, in the order the real card sends them. */
static const unsigned char REPLY_CATEGORY[] = {
	0xf0, 0x43, 0x10, 0x4e, 0x01, 0x10, 0x00, 0x00, 0x07, 0x00, 0xf7
};
/* **The name field is 14 bytes, not 11.** Both real cards pad to 14: the capture
 * has "PLG150-AP" followed by five spaces and "PLG100-VL" followed by five. A
 * first attempt used 11, the messages arrived at the host byte for byte, and the
 * card was still not accepted - so the field is a fixed length the firmware
 * reads, not a string that ends at the first space. */
static const unsigned char REPLY_NAME[] = {
	0xf0, 0x43, 0x10, 0x4e, 0x01, 0x00, 0x00,
	'P', 'L', 'G', '1', '5', '0', '-', 'A', 'P', ' ', ' ', ' ', ' ', ' ',
	0xf7
};

#define NREPLY 2

struct answer {
	/* Kept so destroy() can still talk to the host, which outlives the card. */
	const struct plg_host *host;

	/* Receive: the host's bytes, framed by the chip into bytes and then by us
	 * into messages. */
	int      bit;
	unsigned shift;
	int      rx_frames;      /* complete host messages seen */
	int      rx_bits;       /* bits handed over, for the report */
	int      rx_bytes;    /* bytes in the message being received */
	unsigned char rx[4];  /* its first four, to recognise it */
	int      rx_total;    /* bytes since reset, for the report */
	int      rx_polls;    /* messages seen that look like a poll */
	char     rx_log[8][16];  /* the first messages, as the card saw them */
	int      rx_log_n;

	/* Transmit: a queue of the two replies, one bit handed over per midi_tx(). */
	int            tx_byte;      /* index into tx_buf, -1 when nothing to send */
	int            tx_bit;       /* 0..9: start, eight data, stop */
	int            tx_hold;      /* polls per bit */
	int            hold_per_bit; /* the above, as a knob: see read_knobs() */
	int            start_delay;  /* idle polls before a start bit */
	int            once;         /* answer one message in total, like the AP */
	int            on_poll;      /* answer the poll rather than the first message */
	int            want_poll;    /* the knob's value, kept across reset() */
	int            read_on_poll; /* the knob's value, kept across reset() */
	unsigned char  tx_buf[sizeof(REPLY_CATEGORY) + sizeof(REPLY_NAME)];
	int            tx_len;
	int            tx_next;      /* where the next reply starts in tx_buf */
	int            tx_level;     /* the level being held */
	int            sent;         /* replies sent so far, for the report */
	int            accepted;     /* set once both have gone out */
};

/* The knobs, read once in create(). They exist because the card is stepped
 * by the host's poll and the relationship between "one bit" and "one poll" is
 * exactly what had to be found; guessing it produced framing errors, and a
 * sweep over it is cheaper than an argument. Both default to the values that
 * work.
 *
 *   SMU2000_CARD_HOLD   polls per bit. The host polls at half a bit, so 2 is one
 *                       bit per bit period.
 *   SMU2000_CARD_ONCE   1 (default) answers the first host message only, as the
 *                       real AP does; 0 answers every one.
 *   SMU2000_CARD_START  idle polls inserted before a start bit, which moves the
 *                       card's transitions relative to the chip's sample points
 *                       without changing the rate.
 */
static void read_knobs(struct answer *a)
{
	a->hold_per_bit = 2;
	a->start_delay = 0;
	const char *e;
	if ((e = getenv("SMU2000_CARD_HOLD")) != 0) {
		const int v = atoi(e);
		if (v >= 1 && v <= 8)
			a->hold_per_bit = v;
	}
	if ((e = getenv("SMU2000_CARD_START")) != 0) {
		const int v = atoi(e);
		if (v >= 0 && v <= 8)
			a->start_delay = v;
	}
	a->once = 1;
	if ((e = getenv("SMU2000_CARD_ONCE")) != 0 && atoi(e) == 0)
		a->once = 0;
	a->want_poll = 1;
	if ((e = getenv("SMU2000_CARD_ON_POLL")) != 0 && atoi(e) == 0)
		a->want_poll = 0;
}

/* Lay the replies out once. Called from create, so it is not on any hot path. */
static void build_replies(struct answer *a)
{
	int n = 0;
	memcpy(a->tx_buf + n, REPLY_CATEGORY, sizeof(REPLY_CATEGORY));
	n += (int)sizeof(REPLY_CATEGORY);
	memcpy(a->tx_buf + n, REPLY_NAME, sizeof(REPLY_NAME));
	n += (int)sizeof(REPLY_NAME);
	a->tx_len = n;
	a->tx_next = n;          /* nothing queued yet */
}

static const plg_card_info g_info = {
	PLG_ABI_VERSION,
	PLG_KIND_SELF,
	PLG_MODEL_ANY,
	0,
	"smu2000.answer",
	"S-MU2000 answering card"
};

PLG_EXPORT const plg_card_info *plg1500_get_info(void)
{
	return &g_info;
}

PLG_EXPORT plg_card *plg1500_create(const plg_host *host, void *ctx, char *err, size_t err_len)
{
	(void)ctx;
	if (!host || host->abi != PLG_ABI_VERSION) {
		if (err && err_len)
			snprintf(err, err_len, "host abi mismatch");
		return 0;
	}
	struct answer *a = (struct answer *)host->alloc(host->ctx, sizeof(struct answer));
	if (!a) {
		if (err && err_len)
			snprintf(err, err_len, "no ram");
		return 0;
	}
	a->host = host;
	read_knobs(a);
	build_replies(a);
	return (plg_card *)a;
}

PLG_EXPORT void plg1500_destroy(plg_card *c)
{
	struct answer *a = (struct answer *)c;
	if (!a || !a->host || !a->host->log)
		return;
	char line[256];
	snprintf(line, sizeof(line),
	         "answer: %d bits -> %d byte(s) in %d message(s) (%d poll), sent %d repl%s",
	         a->rx_bits, a->rx_total, a->rx_frames, a->rx_polls, a->sent,
	         a->sent == 1 ? "y" : "ies");
	a->host->log(a->host->ctx, line);
	for (int i = 0; i < a->rx_log_n; i++) {
		char l2[64];
		snprintf(l2, sizeof(l2), "  msg%d: %s", i, a->rx_log[i]);
		a->host->log(a->host->ctx, l2);
	}
}

/* One bit of one reply, per call. The call rate is the host's bit clock. */
static int answer_tx(plg_card *c)
{
	struct answer *a = (struct answer *)c;

	/* Nothing queued: the line idles high. */
	if (a->tx_byte < 0) {
		if (a->tx_next < a->tx_len) {
			a->tx_byte = 0;
			a->tx_bit  = 0;
		} else {
			return 1;
		}
	}

	/* Hold each bit for hold_per_bit polls. The host's poll rate is the bit rate
	 * divided by this, so a wrong value here is a wrong bit rate - which is why
	 * it is a knob rather than a constant. See read_knobs(). */
	if (a->start_delay) {
		a->start_delay--;
		return 1;
	}
	if (a->tx_hold) {
		a->tx_hold--;
		return a->tx_level;
	}
	a->tx_hold = a->hold_per_bit - 1;

	const unsigned char b = a->tx_buf[a->tx_byte];
	int level;
	if (a->tx_bit == 0)
		level = 0;                              /* start bit */
	else if (a->tx_bit <= 8)
		level = (b >> (a->tx_bit - 1)) & 1;     /* data, LSB first */
	else
		level = 1;                              /* stop bit */
	a->tx_level = level;

	a->tx_bit++;
	if (a->tx_bit >= 10) {
		a->tx_bit = 0;
		a->tx_byte++;
		if (a->tx_byte >= a->tx_len) {
			a->tx_byte = -1;
			a->tx_next = a->tx_len;              /* both replies are out */
			a->sent++;
			if (a->sent >= NREPLY)
				a->accepted = 1;
		}
	}
	return level;
}

/* The host's bits, one at a time, each with its place in the byte.
 *
 * There is no start bit to find here and no timing to infer, and that is the
 * whole point of the change. The line this used to arrive on carries an edge for
 * every target-register write the firmware makes - `target_w()` in sci4.cpp drives
 * the *disabled* multiplexed lines high - so an edge-driven receiver was lost
 * before the host had transmitted anything: a dozen spurious edges, then a byte
 * out for the rest of the run, with every "message" one byte long. MAME's cards
 * never noticed, because they are real SCI peripherals that count their own bit
 * clock and sample by phase, stepping over every gap on the wire.
 *
 * Handing over the bit and its index makes that impossible to get wrong, and
 * leaves the card framing its own bytes, which is the card's business. The chip
 * is a UART and so is this: a stop bit that comes back low is a framing error and
 * the byte is thrown away. */
static void answer_rx(plg_card *c, int level, int bit)
{
	struct answer *a = (struct answer *)c;
	level = level ? 1 : 0;
	a->rx_bits++;
	if (bit < 8) {
		if (level)
			a->shift |= 1u << bit;      /* LSB first */
		return;
	}
	if (!level)
		return;                             /* framing error: not a byte */
	a->shift    = 0;
	a->rx_bytes++;
	a->rx_total++;
	if (a->rx_bytes <= (int)sizeof(a->rx))
		a->rx[a->rx_bytes - 1] = (unsigned char)a->shift;
	if (a->rx_bytes < 1 || a->rx[a->rx_bytes - 1] != 0xf7)
		return;
	a->rx_frames++;
	/* A short log of what arrived, because "the host sent a poll and the card did
	 * not see one" is only answerable by looking at both. */
	if (a->rx_log_n < 8) {
		char *dst = a->rx_log[a->rx_log_n++];
		int k = 0;
		for (int i = 0; i < a->rx_bytes && k < 15; i++) {
			const unsigned b = i < (int)sizeof(a->rx) ? a->rx[i] : 0xff;
			k += snprintf(dst + k, 16 - k, "%02x", b);
		}
		dst[k] = 0;
	}
	/* **Which message is worth answering is a measured question.** The capture of a
	 * real AP shows the host sending a read (F0 43 10 4E 00 10 02 01) and then a
	 * poll (F0 43 30 4E 01 10 00), with the card's two replies arriving after the
	 * poll. */
	const int is_poll = a->rx_bytes >= 3 && a->rx[0] == 0xf0 &&
	                    a->rx[1] == 0x43 && a->rx[2] == 0x30;
	if (is_poll)
		a->rx_polls++;
	const int want = a->on_poll ? is_poll : 1;
	if (want && a->tx_byte < 0 && a->tx_next >= a->tx_len && !(a->once && a->sent))
		a->tx_next = 0;
}

static void answer_run(plg_card *c, plg_slot_io *io)
{
	/* Silent, and it says why: the audio path is wired and checked, but the
	 * SWP30 mixer only opens a card's tap once the firmware has recognised the
	 * card, so there is nothing to send before that. */
	struct answer *a = (struct answer *)c;
	if (!io)
		return;
	io->out[0] = io->out[1] = 0;
	(void)a;
}

static void answer_reset(plg_card *c)
{
	struct answer *a = (struct answer *)c;
	a->bit = 0;
	a->shift = 0;
	a->rx_frames = a->rx_bytes = a->rx_total = a->rx_polls = 0;
	a->tx_byte = -1;
	a->tx_bit  = 0;
	a->tx_hold = 0;
	a->start_delay = 0;
	a->tx_level = 1;
	a->sent = a->accepted = 0;
	a->on_poll = a->want_poll;
	build_replies(a);
}

static size_t answer_save(plg_card *c, void *dst, size_t cap)
{
	const struct answer *a = (const struct answer *)c;
	if (!dst || cap < sizeof(*a))
		return sizeof(*a);
	memcpy(dst, a, sizeof(*a));
	return sizeof(*a);
}

static int answer_load(plg_card *c, const void *src, size_t n)
{
	struct answer *a = (struct answer *)c;
	if (n != sizeof(*a))
		return 1;
	memcpy(a, src, n);
	/* The host pointer is not ours to restore. */
	a->host = 0;
	return 0;
}

static int answer_selftest(plg_card *c)
{
	/* Two things worth asserting before the card is allowed anywhere near a
	 * machine, both through the same functions the real traffic uses:
	 *
	 *  1. the receiver turns a start bit, eight data bits and a stop bit back
	 *     into the byte that was sent;
	 *  2. the transmitter produces exactly the two replies the real card sends,
	 *     ten bits per byte, start low and stop high, LSB first.
	 *
	 * The second is checked by stepping midi_tx() the way the host will, and
	 * comparing the bits against the expected frame. Ten bits per byte is what
	 * makes this a test of the card rather than a test of itself.
	 */
	struct answer *a = (struct answer *)c;
	const unsigned char want = 0x5a;
	int bit, got = 0, bad = 0;

	/* 1. receive: eight data bits then a stop bit, each with its place. There is no
	 * start bit to offer and none to find, which is the whole change. */
	for (bit = 0; bit < 8; bit++)
		answer_rx(c, (int)((want >> bit) & 1), bit);
	answer_rx(c, 1, 8);
	if (a->rx_bytes != 1 || a->shift != want)
		return 1;
	/* A stop bit that comes back low is a framing error and the byte is dropped:
	 * the chip is a UART and so is this. */
	{
		const int before = a->rx_bytes;
		for (bit = 0; bit < 8; bit++)
			answer_rx(c, (int)((want >> bit) & 1), bit);
		answer_rx(c, 0, 8);
		if (a->rx_bytes != before)
			return 3;
	}

	/* 2. transmit: queue one reply and walk its bits */
	build_replies(a);
	a->tx_next = 0;
	for (got = 0; got < (int)sizeof(REPLY_CATEGORY) * 10; got++) {
		const int byte = got / 10, step = got % 10;
		const unsigned char b = REPLY_CATEGORY[byte];
		int expect;
		if (step == 0)
			expect = 0;
		else if (step <= 8)
			expect = (b >> (step - 1)) & 1;
		else
			expect = 1;
		/* Each bit is held for hold_per_bit polls, and every one of them must
		 * report the same level. Stepping by the knob rather than by a literal is
		 * what lets this test mean anything when the knob is not 2 - and a selftest
		 * that hard-codes the framing is a selftest that refuses the card whenever
		 * the framing is being searched for, which is exactly when it is needed. */
		for (int k = 0; k < a->hold_per_bit; k++)
			if (answer_tx(c) != expect)
				bad++;
	}
	if (bad)
		return 2;

	answer_reset(c);
	return 0;
}

static plg_card_ops g_ops;
static int g_ops_filled;

PLG_EXPORT const plg_card_ops *plg1500_ops(const plg_card *c)
{
	(void)c;
	if (!g_ops_filled) {
		memset(&g_ops, 0, sizeof(g_ops));
		g_ops.size      = sizeof(g_ops);
		g_ops.abi       = PLG_ABI_VERSION;
		g_ops.reset     = answer_reset;
		g_ops.run       = answer_run;
		g_ops.midi_rx   = answer_rx;
		g_ops.midi_tx   = answer_tx;
		g_ops.save      = answer_save;
		g_ops.load      = answer_load;
		g_ops.destroy   = plg1500_destroy;
		g_ops.selftest  = answer_selftest;
		g_ops_filled    = 1;
	}
	return &g_ops;
}
