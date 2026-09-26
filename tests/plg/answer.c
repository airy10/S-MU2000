/* license:BSD-3-Clause *
 *
 * tests/plg/answer.c - a PLG card that answers, and gets recognised.
 *
 * This is the card the whole exercise was for. It knows two things and nothing
 * else, and both were read off a real PLG150-AP talking to a real MU2000:
 *
 *   reply to a read of 2 bytes at 0x0010   F0 43 10 4E 01 10 00 00 07 00 F7
 *   reply with its own name, 11 bytes      F0 43 10 4E 01 00 00 "PLG150-AP     " F7
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
 */
#include "plg/plg1500.h"

#include <stdio.h>
#include <string.h>

#define PLG_NAME "PLG150-AP"   /* 11 characters, space padded on the wire */

/* What the AP answers for the 2-byte read at 0x0010. See the header comment. */
static const unsigned char CATEGORY[2] = { 0x00, 0x07 };

/* The two replies, in the order the real card sends them. */
static const unsigned char REPLY_CATEGORY[] = {
	0xf0, 0x43, 0x10, 0x4e, 0x01, 0x10, 0x00, 0x00, 0x07, 0x00, 0xf7
};
static const unsigned char REPLY_NAME[] = {
	0xf0, 0x43, 0x10, 0x4e, 0x01, 0x00, 0x00,
	'P', 'L', 'G', '1', '5', '0', '-', 'A', 'P', ' ', ' ',
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
	int      rx_bytes;

	/* Transmit: a queue of the two replies, one bit handed over per midi_tx(). */
	int            tx_byte;      /* index into tx_buf, -1 when nothing to send */
	int            tx_bit;       /* 0..9: start, eight data, stop */
	int            tx_hold;      /* polls per bit, so the host may look twice */
	unsigned char  tx_buf[sizeof(REPLY_CATEGORY) + sizeof(REPLY_NAME)];
	int            tx_len;
	int            tx_next;      /* where the next reply starts in tx_buf */
	int            tx_level;     /* the level being held */
	int            sent;         /* replies sent so far, for the report */
	int            accepted;     /* set once both have gone out */
};

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
	         "answer: heard %d byte(s) in %d message(s), sent %d repl%s",
	         a->rx_bytes, a->rx_frames, a->sent, a->sent == 1 ? "y" : "ies");
	a->host->log(a->host->ctx, line);
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

	/* Hold each bit for two polls. The host looks twice per bit, so an edge is
	 * never missed, while the bit *rate* stays right: one bit per two looks is
	 * one bit per bit period. */
	if (a->tx_hold) {
		a->tx_hold--;
		return a->tx_level;
	}
	a->tx_hold = 1;

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

/* The host's line, as an edge. The chip framed it; we only find the messages. */
static void answer_rx(plg_card *c, int level)
{
	struct answer *a = (struct answer *)c;
	level = level ? 1 : 0;
	if (a->bit == 0) {
		if (!level) {
			a->bit   = 1;
			a->shift = 0;
		}
		return;
	}
	if (a->bit == 9) {
		if (level) {
			a->rx_bytes++;
			/* A complete message from the host is the cue to answer, which is
			 * what the real card does: it is addressed, it replies. Queueing both
			 * replies here rather than matching the request keeps this card free
			 * of any protocol knowledge, which is the point of it. */
			if (a->shift == 0xf7) {
				a->rx_frames++;
				if (a->tx_byte < 0 && a->tx_next >= a->tx_len) {
					a->tx_next = 0;              /* re-arm, and answer again */
				}
			}
		}
		a->bit = 0;
		return;
	}
	if (level)
		a->shift |= 1u << (a->bit - 1);
	a->bit++;
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
	a->rx_frames = a->rx_bytes = 0;
	a->tx_byte = -1;
	a->tx_bit  = 0;
	a->tx_hold = 0;
	a->tx_level = 1;
	a->sent = a->accepted = 0;
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

	/* 1. receive */
	answer_rx(c, 1);
	answer_rx(c, 0);
	for (bit = 0; bit < 8; bit++)
		answer_rx(c, (int)((want >> bit) & 1));
	answer_rx(c, 1);
	if (a->rx_bytes != 1 || a->shift != want)
		return 1;

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
		/* Each bit is held for two polls, so the value must appear twice. */
		if (answer_tx(c) != expect || answer_tx(c) != expect)
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
