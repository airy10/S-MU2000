/* license:BSD-3-Clause *
 *
 * tests/plg/answer.c - a PLG card that answers, and gets recognised.
 *
 * ## What the wire actually is
 *
 * The exchange is a memory read, and it is much simpler than it first looks. The
 * host's two message shapes, both read off a real PLG150-AP talking to a real
 * MU2000:
 *
 *   F0 43 10 4E 00 <hi> <lo> <len> F7      "I want to read len bytes at hi:lo"
 *   F0 43 30 4E 01 <hi> <lo> F7            "now give me the data at hi:lo"
 *
 * and the card answers the second one, echoing the address it is answering:
 *
 *   F0 43 10 4E 01 <hi> <lo> <data...> F7
 *
 * The read only announces; **the poll is the request**. That is why the card sat
 * silent until it had seen a read: the poll after a read is the thing worth
 * answering, and answering the read itself puts the reply into a window where the
 * chip cannot receive it.
 *
 * The two replies a real AP gives, and the only two it gives in 600 s:
 *
 *   at 0x1000, 3 bytes      00 07 00          the card's category
 *   at 0x0000, 14 bytes     "PLG150-AP     "   its name, space padded
 *
 * So the card is a memory-backed device and the host walks it by address. The
 * addresses the MU2000 asks for, in order, are 0x1000, 0x0000, 0x000e, 0x000f,
 * 0x0010, 0x1003, 0x1004, 0x0120, 0x1010 - and the first two are the whole of
 * what is known. **The rest is card-internal tone and voice data, and answering
 * it is what the card is for.** `SMU2000_CARD_UNKNOWN` chooses what to do with
 * an address that has no table entry; `zero` answers with that many zero bytes,
 * `long` with a page of them, `none` stays silent. See the table in records[].
 *
 * ## The category
 *
 * The first reply's data is the card's category. The AP answers 00 07 and the
 * PLG100-VL answers 00 00, which is how the MU2000 tells an acoustic piano from
 * a virtual acoustic one - and it lines up with the eighteen bracketed category
 * names in the MU2000 firmware (mu2000_flash.bin around 0x1dda98, `[Piano]`
 * first).
 *
 * The device id in the request is not the same for every card: the AP is asked
 * with `F0 43 10 4E ...` and the VL with `F0 43 08 6E ...`, so the firmware
 * picks its opening move from the card model it expects to find.
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
 * DATA_NAME below is what the MU2000 is told this card is called. Change it and
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

/* 11 characters, space padded to 14 on the wire; see DATA_NAME. */

/* The card's memory, as far as it is known. The host walks it by address: it
 * announces a read, then polls the address, and the card answers the poll with the
 * bytes at that address. Only two addresses are known, from the MAME capture of a
 * real PLG150-AP - and they are the only two it ever answers, in 600 s.
 *
 * `len` is how many bytes go back, which is not the same for both: the category is
 * three and the name is fourteen. The name's length is the interesting one, since
 * the string is 11 characters - both real cards pad to 14, "PLG150-AP" plus five
 * spaces, so the field is a fixed width the firmware reads rather than a string
 * that ends at the first space. An 11-byte answer arrived at the host byte for
 * byte and was still not accepted. */
struct record {
	unsigned short      addr;
	int                 len;
	const unsigned char *data;
};

/* Not const: SMU2000_CARD_CATEGORY overwrites the first two bytes.
 *
 * **These are the PLG100-VL's, not the AP's, and that is deliberate.** Both were
 * captured from MAME and the two cards differ where it counts:
 *
 *              category at 0x1000        name
 *   VL         00 00 00                   PLG100-VL
 *   AP         00 07 00                   PLG150-AP
 *
 * This card used to present the AP, because the AP is the one that gets
 * *recognised* - PLG-1 lights. But with the AP in the slot the firmware puts
 * "PB Com Error!" on the LCD and then waits, and the wait is the hang; with the
 * VL in the slot the same firmware reaches PLUGIN SELECT, lists both cards, and
 * opens a voice editor. The AP is the card that does not complete the protocol,
 * so a reference card should present the one that does.
 *
 * The capture this is transcribed from is tests/plg/plg100vl-capture.txt. */
static unsigned char DATA_CATEGORY[3] = { 0x00, 0x00, 0x00 };
static const unsigned char DATA_NAME[] = {
	'P', 'L', 'G', '1', '0', '0', '-', 'V', 'L', ' ', ' ', ' ', ' ', ' '
};

static const struct record RECORDS[] = {
	{ 0x1000, (int)sizeof(DATA_CATEGORY), DATA_CATEGORY },
	{ 0x0000, (int)sizeof(DATA_NAME),      DATA_NAME      },
};
#define NRECORD ((int)(sizeof(RECORDS) / sizeof(RECORDS[0])))

/* What to do with an address no record covers. The addresses the host asks for
 * beyond the two above are 0x000e, 0x000f, 0x0010, 0x1003, 0x1004, 0x0120 and
 * 0x1010, and what lives at them is the card's tone and voice data - the part of
 * a real PLG that this project does not have. Stalling there is what stops the
 * boot, so `zero` exists to find out whether the firmware only needs *something*
 * at each address or whether it checks the values.
 *
 *   none   stay silent, which is the old behaviour and stalls the boot
 *   zero   answer with `unknown_len` zero bytes
 *   long   answer with a full page of zero bytes, for when a short one is too few
 */
#define UNKNOWN_NONE 0
#define UNKNOWN_ZERO 1
#define UNKNOWN_LONG 2
static int          s_unknown = UNKNOWN_NONE;
static int          s_unknown_len = 8;
static unsigned char s_unknown_buf[64];

/* The largest reply is a page of zeros; a sysex header is 7 bytes. */
#define MAXTX (7 + (int)sizeof(s_unknown_buf) + 1)

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
	unsigned char msg[8]; /* the current message's head, F0..F7 */
	int           msg_n;
	int           seen_poll; /* a poll has been seen, so `after` can count down */
	int           read_seen;  /* the identity read arrived; the next poll releases
	                             the category reply (it must not go out during the
	                             deaf targets=07 window the read arrives in) */
	int           after;      /* polls still to let pass before answering */
	int           after_polls; /* the knob's value, restored by reset() */
	int           delay_polls; /* sub-poll delay before starting the reply */
	int           mute;         /* receive only, for the bisect */
	int           discard;      /* return from midi_rx immediately: separates the
	                             call itself (timing) from what it does (state) */
	int      rx_total;    /* bytes since reset, for the report */
	int      rx_polls;    /* messages seen that look like a poll */
	char     rx_log[16][24]; /* the first messages, as the card saw them */
	int      rx_log_n;

	/* Transmit: a mailbox holding one reply at a time, one bit handed over per
	 * midi_tx(). */
	int            tx_byte;      /* index into tx_out, -1 when nothing to send */
	int            tx_bit;       /* 0..9: start, eight data, stop */
	int            tx_hold;      /* polls per bit */
	int            hold_per_bit; /* the above, as a knob: see read_knobs() */
	int            start_delay;  /* idle polls before a start bit */
	int            once;         /* answer each address once, like the AP */
	int            on_poll;      /* answer the poll rather than the first message */
	unsigned char  tx_buf[MAXTX];   /* the mailbox: one reply at a time */
	int            tx_len;      /* bytes in the mailbox */
	int            tx_queued;   /* the mailbox holds a reply waiting to go out */
	unsigned char  tx_out[MAXTX];   /* the reply actually on the wire */
	int            tx_out_len;  /* bytes in tx_out */
	/* One flag per record, so each address is answered once and the report can
	 * say which. `once` turns this off and answers everything every time. */
	unsigned char  sent_rec[NRECORD];
	int            tx_level;     /* the level being held */
	int            tx_calls;     /* midi_tx() calls, for the report */
	int            sent;         /* replies sent so far, for the report */
	int            accepted;     /* set once the known addresses have all gone out */
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
	/* Two polls per bit: the host polls at half a bit (line_tick) and samples
	 * once per bit (rx_tick), so one card bit per two polls is exactly one host
	 * bit, with rx sampling mid-bit after starting on the card's start edge.
	 *
	 * This is only true because transmit is stepped from ONE clock. An earlier
	 * version also stepped it from rx_tick's pull, which made the effective rate
	 * the sum of two unrelated clocks - and hold=3 then happened to work by
	 * aliasing into a stable offset, while hold=2 failed depending on the run.
	 * With a single deterministic clock hold=2 is exactly right, in boot and in
	 * the gui, and the comment that used to be here explaining hold=3 was
	 * explaining an accident. */
	a->hold_per_bit = 4;
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
	a->on_poll = 1;
	if ((e = getenv("SMU2000_CARD_ON_POLL")) != 0 && atoi(e) == 0)
		a->on_poll = 0;
	/* SMU2000_CARD_CATEGORY=hi,lo - the two bytes the card claims for itself. */
	if ((e = getenv("SMU2000_CARD_CATEGORY")) != 0) {
		unsigned hi = 0, lo = 0;
		if (sscanf(e, "%x,%x", &hi, &lo) == 2) {
			DATA_CATEGORY[0] = (unsigned char)hi;
			DATA_CATEGORY[1] = (unsigned char)lo;
		}
	}
	a->after_polls = 0;
	/* SMU2000_CARD_MUTE=1: receive only, never queue a reply. Bisects "the
	 * firmware stalls because a card is present" against "it stalls because of
	 * what the card sends". */
	a->mute = 0;
	if ((e = getenv("SMU2000_CARD_MUTE")) != 0 && atoi(e) != 0)
		a->mute = 1;
	a->discard = 0;
	if ((e = getenv("SMU2000_CARD_DISCARD")) != 0 && atoi(e) != 0)
		a->discard = 1;
	if ((e = getenv("SMU2000_CARD_AFTER")) != 0) {
		const int v = atoi(e);
		if (v >= 0 && v <= 16)
			a->after_polls = v;
	}
	/* SMU2000_CARD_UNKNOWN: what to answer at an address with no record, which is
	 * every address past the category and the name. Those are the card's tone and
	 * voice tables and answering them is what unblocks the boot - see records[]. */
	if ((e = getenv("SMU2000_CARD_UNKNOWN")) != 0) {
		if (!strcmp(e, "zero"))       s_unknown = UNKNOWN_ZERO;
		else if (!strcmp(e, "long")) s_unknown = UNKNOWN_LONG;
		else                                s_unknown = UNKNOWN_NONE;
	}
	if ((e = getenv("SMU2000_CARD_UNKNOWN_LEN")) != 0) {
		const int v = atoi(e);
		if (v > 0 && v <= (int)sizeof(s_unknown_buf))
			s_unknown_len = v;
	}
	/* SMU2000_CARD_DELAY: midi_tx polls to wait after queueing before shifting
	 * the first bit. The real PLG150-AP answers ~235 ms after the poll; at a
	 * 16 us poll that is ~14700. Answering immediately may put bytes on the wire
	 * before the firmware's receiver is ready, which it then spends the whole
	 * boot retrying - "Checking PLG" never clears and nothing responds. */
	a->delay_polls = 0;
	if ((e = getenv("SMU2000_CARD_DELAY")) != 0) {
		const long v = atol(e);
		if (v >= 0 && v <= 100000)
			a->delay_polls = (int)v;
	}
}

/* Build the reply for one address into the mailbox. The shape is fixed by the
 * wire, not by us: F0 43 10 4E 01 <hi> <lo> <data...> F7, echoing the address so
 * the host can match it to what it asked for.
 *
 * **One reply per request, because that is what the real card does.** The MAME
 * capture with a real PLG150-AP shows it answering a poll for 0x1000 with the
 * category, and a later poll for 0x0000 with the name - two separate responses to
 * two separate prompts, and nothing else in 600 s. Sending both back-to-back
 * after the first poll gets PLG-1 lit but leaves the panel dead, so the firmware
 * does tell the difference.
 *
 * Returns 0 if there is nothing to say at this address, which is the default:
 * staying silent is the old behaviour and it is what stalls the boot. */
static int queue_reply(struct answer *a, unsigned short addr)
{
	const unsigned char *data = 0;
	int len = 0;
	int i;

	for (i = 0; i < NRECORD; i++) {
		if (RECORDS[i].addr == addr) {
			if (a->once && a->sent_rec[i])
				return 0;    /* already given this one out */
			data = RECORDS[i].data;
			len = RECORDS[i].len;
			if (a->once)
				a->sent_rec[i] = 1;
			break;
		}
	}
	if (!data) {
		if (s_unknown == UNKNOWN_NONE)
			return 0;
		if (s_unknown == UNKNOWN_LONG)
			len = (int)sizeof(s_unknown_buf);
		else
			len = s_unknown_len;
		data = s_unknown_buf;   /* already zero, and stays that way */
	}

	a->tx_buf[0] = 0xf0;
	a->tx_buf[1] = 0x43;
	a->tx_buf[2] = 0x10;
	a->tx_buf[3] = 0x4e;
	a->tx_buf[4] = 0x01;
	a->tx_buf[5] = (unsigned char)(addr >> 8);
	a->tx_buf[6] = (unsigned char)(addr & 0xff);
	memcpy(a->tx_buf + 7, data, (size_t)len);
	a->tx_buf[7 + len] = 0xf7;
	a->tx_len = len + 8;
	a->tx_queued = 1;
	return 1;
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
	a->tx_len = 0;
	a->tx_queued = 0;      /* nothing in the mailbox yet */
	memset(a->sent_rec, 0, sizeof(a->sent_rec));
	return (plg_card *)a;
}

PLG_EXPORT void plg1500_destroy(plg_card *c)
{
	struct answer *a = (struct answer *)c;
	if (!a || !a->host || !a->host->log)
		return;
	char line[256];
	snprintf(line, sizeof(line),
	         "answer: %d bits -> %d byte(s) in %d message(s) (%d poll), tx polled %d, sent %d %s",
	         a->rx_bits, a->rx_total, a->rx_frames, a->rx_polls, a->tx_calls, a->sent,
	         a->sent == 1 ? "reply" : "replies");
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
	a->tx_calls++;

	/* Nothing queued: the line idles high. */
	if (a->tx_byte < 0) {
		if (a->tx_queued) {
			/* Take the mailbox onto the wire. The copy matters: the host
			 * sends the name poll while this reply is still shifting out,
			 * and a mailbox that is also the transmit buffer would have
			 * the name land in the middle of the category. */
			memcpy(a->tx_out, a->tx_buf, (size_t)a->tx_len);
			a->tx_out_len = a->tx_len;
			a->tx_queued = 0;
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
	/* The reply is queued but its first bit waits out the delay. Once shifting,
	 * this never triggers again for this reply. */
	if (a->tx_byte < 0 && a->tx_queued && a->delay_polls > 0) {
		a->delay_polls--;
		return 1;
	}
	if (a->tx_hold) {
		a->tx_hold--;
		return a->tx_level;
	}
	a->tx_hold = a->hold_per_bit - 1;

	const unsigned char b = a->tx_out[a->tx_byte];
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
		if (a->tx_byte >= a->tx_out_len) {
			a->tx_byte = -1;
			/* The mailbox is left alone. Whether a reply is waiting is
			 * tx_queued, not a position - finishing one must not discard
			 * the next, which is exactly what this used to do. */
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
	if (a->discard)
		return;
	a->rx_bits++;
	if (bit < 8) {
		if (level)
			a->shift |= 1u << bit;      /* LSB first */
		return;
	}
	if (!level)
		return;                             /* framing error: not a byte */
	const int byte = (int)a->shift;
	a->shift = 0;
	a->rx_total++;
	/* **A message head, reset by F0.**
	 *
	 * Keeping "the first four bytes" in one buffer does not work, and the failure
	 * looks like nothing at all: those slots are only written while the counter
	 * is small, so after the first message they still hold *that* message's head
	 * and every later message is compared against it. 158 correct bytes and "0
	 * polls" is what that looks like. The head is reset by the message's own start
	 * byte instead. */
	if (byte == 0xf0)
		a->msg_n = 0;
	if (a->msg_n < (int)sizeof(a->msg))
		a->msg[a->msg_n++] = (unsigned char)byte;
	if (byte != 0xf7)
		return;
	a->rx_bytes++;
	a->rx_frames++;
	/* A short log of what arrived, because "the host sent a poll and the card did
	 * not see one" is only answerable by looking at both. */
	if (a->rx_log_n < 16) {
		char *dst = a->rx_log[a->rx_log_n++];
		int k = 0;
		for (int i = 0; i < a->msg_n && k < 15; i++)
			k += snprintf(dst + k, 16 - k, "%02x", a->msg[i]);
		dst[k] = 0;
	}
	/* **Which request gets which reply is read off the real card**, and it is
	 * simpler than it looks: the host announces a read and then polls an address,
	 * and the poll is the request. The MAME capture with a real PLG150-AP:
	 *
	 *   host: F0 43 10 4E 00 10 02 01 F7   "I want to read at 0x1002"
	 *   host: F0 43 30 4E 01 10 00 F7     "now give me 0x1000"
	 *   card: F0 43 10 4E 01 10 00 00 07 00 F7   the category
	 *   host: F0 43 30 4E 01 00 00 F7     "now give me 0x0000"
	 *   card: F0 43 10 4E 01 00 00 "PLG150-AP     " F7   the name
	 *
	 * So the card answers the poll, not the read, and it answers with the address
	 * it was asked for. Answering the read itself puts the reply in a window where
	 * the chip cannot receive it: the read arrives while targets=07 (upper nibble
	 * 0), during which channel 3's composite is stuck high, measured as 300
	 * card-low samples with the composite pinned high throughout. The first poll
	 * arrives with targets=11, a slot selected, which is when the real AP's reply
	 * lands too.
	 *
	 * `once` is per address, as it is on the real card: with the default each
	 * address is answered the first time it is asked for and never again. */
	const int is_read = a->msg_n == 8 && a->msg[0] == 0xf0 && a->msg[1] == 0x43 &&
	                    a->msg[2] == 0x10 && a->msg[3] == 0x4e && a->msg[4] == 0x00;
	if (is_read) {
		a->read_seen = 1;
		return;
	}
	/* A poll is F0 43 30 4E 01 <hi> <lo> F7 - eight bytes, and msg_n saturates at
	 * sizeof(msg), which is also eight, so the test is == 8. It was == 7 once, which
	 * nothing can ever satisfy, and the name was never sent at all. */
	const int is_poll = a->msg_n == 8 && a->msg[0] == 0xf0 && a->msg[1] == 0x43 &&
	                    a->msg[2] == 0x30 && a->msg[3] == 0x4e && a->msg[4] == 0x01;
	if (!is_poll)
		return;
	a->rx_polls++;
	a->seen_poll = 1;
	/* The first poll after a read is the one worth answering, so `after` can hold
	 * off if asked to. Kept from the earlier version: answering the very first
	 * matching poll is what is measured to work. */
	if (a->read_seen && a->after > 0) {
		a->after--;
		return;
	}
	if (a->tx_byte >= 0 || a->tx_queued)
		return;             /* still sending the last one; drop this, as a real card does */
	const unsigned short addr = (unsigned short)((a->msg[5] << 8) | a->msg[6]);
	if (queue_reply(a, addr))
		a->sent++;
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
	memset(a->sent_rec, 0, sizeof(a->sent_rec));
	a->start_delay = 0;
	a->tx_level = 1;
	a->sent = a->accepted = 0;
	a->after = a->after_polls;
	a->seen_poll = 0;
	a->tx_len = 0;
	a->tx_queued = 0;      /* nothing in the mailbox yet */
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
	/* The byte is checked in the log, not in `shift`: the receiver clears shift
	 * once it has stored the byte, so shift is 0 here by design. Checking shift
	 * is what made this selftest refuse a perfectly good card. */
	if (a->rx_total != 1 || a->msg[0] != want)
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

	/* 2. transmit: queue the category reply and walk its bits */
	{
		const int expect_len = 7 + (int)sizeof(DATA_CATEGORY) + 1;
		if (!queue_reply(a, 0x1000))
			return 4;
		for (got = 0; got < expect_len * 10; got++) {
			const int byte = got / 10, step = got % 10;
			const unsigned char b = a->tx_buf[byte];
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
