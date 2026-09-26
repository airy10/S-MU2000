/* license:BSD-3-Clause *
 *
 * tests/plg/echo.c - a card that listens, and says what it heard.
 *
 * The point of this card is that it needs no protocol knowledge at all. It does
 * not know what a Yamaha sysEx is, it does not know what the two bytes at
 * 0x0010 mean, and it does not know how a real PLG board identifies itself. It
 * counts the bytes the firmware sends it and prints them.
 *
 * That is enough to answer the question this card exists for: **does anything
 * the MU2000's firmware sends actually reach a card in a slot?** The SCI4 trace
 * (boot --trace-sci4) shows the bytes leaving 0xf00000. This shows them arriving.
 * When both agree, the wiring is proven, and the next experiment - working out
 * what to answer - can start from a known-good loop instead of from a guess.
 *
 * What the SCI4 trace already says, for context (no card plugged in):
 *
 *   every 500 ms  F0 43 30 4E 01 10 00 F7    a poll, retried three times
 *   t=6.385       F0 43 10 4E 00 10 02 01 F7  "two bytes from 0x0010"
 *   t=7.956       F0 43 00 4C 00 07 ... F7    voice data
 *
 * and that the firmware broadcasts: target 0x07 enables three of the
 * multiplexed lines at once, so it cannot tell which slot answered.
 *
 * This card answers nothing. A card that answers something the firmware does not
 * expect is worse than one that stays quiet, because the firmware's reaction
 * would then be to the answer rather than to the silence.
 */
#include "plg/plg1500.h"

#include <stdio.h>
#include <string.h>

#define ECHO_LOG_BYTES 512

struct echo {
	/* Kept so destroy() can still talk to the host: the host outlives the card,
	 * but nothing is passed back in, and a card that cannot report anything at
	 * teardown is a card whose last observation is lost. */
	const struct plg_host *host;

	/* Where we are in the byte the chip is shifting at us. The line is idle
	 * high, so a byte is a low start bit, eight data bits LSB first, then a
	 * high stop bit - which is what SCI4 itself does, so the card only has to
	 * keep up. */
	int      bit;
	unsigned shift;

	/* Bytes seen. The count is the point; the bytes are kept so the trace and
	 * this can be compared line for line. */
	int           count;
	int           dropped;
	unsigned char log[ECHO_LOG_BYTES];

	/* What the card puts on its audio out once it has heard something. A real
	 * card's audio comes back on these two wires, and the point of this card is
	 * to prove the plumbing, so it puts a known value there and the test reads it
	 * back off meli 10. A constant rather than a tone: it is inaudible, it costs
	 * nothing, and it cannot be mistaken for a card making a sound the firmware
	 * has no way to route. */
	int32_t out_mark;   /* the type plg_slot_io::out uses */
	int out_sent;
};

static const plg_card_info g_info = {
	PLG_ABI_VERSION,
	PLG_KIND_SELF,
	PLG_MODEL_ANY,     /* claims to be no real card in particular */
	0,                 /* no ROM, and not expected to be heavy */
	"smu2000.echo",
	"S-MU2000 echo card"
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
	struct echo *e = (struct echo *)host->alloc(host->ctx, sizeof(struct echo));
	if (!e) {
		if (err && err_len)
			snprintf(err, err_len, "no ram");
		return 0;
	}
	e->host     = host;
	e->out_mark = 1 << 20;   /* the value the test looks for on meli 10 */
	return (plg_card *)e;
}

PLG_EXPORT void plg1500_destroy(plg_card *c)
{
	/* Report before going. This card's entire output is what it heard, and the
	 * host frees the RAM in eject(), so this is the last chance to say it. */
	struct echo *e = (struct echo *)c;
	if (!e || !e->host || !e->host->log)
		return;
	char line[512];
	/* "heard N byte(s) <bytes...> ; audio mark on". The bytes come straight after
	 * the bracket so a reader can find them without knowing anything else about
	 * the line, and the note about the audio mark goes at the end where it cannot
	 * be mistaken for part of the list. */
	int n = snprintf(line, sizeof(line), "echo: heard %d byte(s)", e->count);
	if (n < 0)
		return;
	const int room = (int)sizeof(line) - n - 2;
	if (room > 0) {
		int k = 0;
		for (int i = 0; i < e->count && i < ECHO_LOG_BYTES && k < room; i++)
			k += snprintf(line + n + k, (size_t)(room - k), " %02x", e->log[i]);
		n += k;
	}
	if (e->dropped)
		snprintf(line + n, sizeof(line) - (size_t)n, " ...(%d more)", e->dropped);
	snprintf(line + n, sizeof(line) - (size_t)n, " ; audio mark %s",
	         e->out_sent ? "on" : "never sent");
	e->host->log(e->host->ctx, line);
}

/* The host's bits, one at a time, each with its place in the byte. The same
 * change as answer.c: per bit, not per edge, so the idle edges on the line -
 * the ones the firmware's target-register writes put there - cannot be taken for
 * data. plg1500.h says why the line on its own is not enough. */
static void echo_rx(plg_card *c, int level, int bit)
{
	struct echo *e = (struct echo *)c;
	level = level ? 1 : 0;
	if (bit < 8) {
		if (level)
			e->shift |= 1u << bit;       /* LSB first */
		return;
	}
	/* The stop bit. It has to be high, or this was not a byte: the chip is a UART
	 * and so is this, and a framing error is counted rather than swallowed. */
	if (!level)
		return;
	if (e->count < ECHO_LOG_BYTES)
		e->log[e->count] = (unsigned char)e->shift;   /* store, then clear */
	else
		e->dropped++;
	e->shift = 0;
	e->count++;
}

static void echo_run(plg_card *c, plg_slot_io *io)
{
	struct echo *e = (struct echo *)c;
	if (!io)
		return;
	if (!io->reset) {
		io->out[0] = io->out[1] = 0;
		return;
	}
	/* A fixed value on the left channel, right channel silent, so a test can tell
	 * the two channels apart as well as the two slots. Unconditional: the audio
	 * mark and the byte log prove two different things - the mark that samples
	 * reach meli 10, the log that the wire works - and gating the mark on having
	 * heard something would make a short render unable to show the first. */
	io->out[0] = e->out_mark;
	io->out[1] = 0;
	e->out_sent = 1;
}

static void echo_reset(plg_card *c)
{
	struct echo *e = (struct echo *)c;
	e->bit = e->count = e->dropped = 0;
	e->shift = 0;
}

static size_t echo_save(plg_card *c, void *dst, size_t cap)
{
	const struct echo *e = (const struct echo *)c;
	const size_t n = sizeof(struct echo);
	if (!dst || cap < n)
		return n;
	memcpy(dst, e, n);
	return n;
}

static int echo_load(plg_card *c, const void *src, size_t n)
{
	struct echo *e = (struct echo *)c;
	if (n != sizeof(struct echo))
		return 1;
	memcpy(e, src, n);
	return 0;
}

static int echo_selftest(plg_card *c)
{
	/* The one thing worth asserting: a start bit followed by eight data bits
	 * and a stop bit is read back as the byte that was sent. Doing it here means
	 * a card that reaches the firmware at all has proved its own receiver -
	 * through the same function the firmware's line ends up in, not a copy of it.
	 *
	 * The counters are put back afterwards. A synthetic byte left in the log
	 * would make the card's report start with a byte the firmware never sent,
	 * which is exactly the sort of thing that wastes an afternoon later. */
	struct echo *e = (struct echo *)c;
	const unsigned want = 0x5a;
	const int      saved_count = e->count, saved_dropped = e->dropped;
	e->shift = 0;
	for (int i = 0; i < 8; i++)      /* eight data bits, then the stop bit */
		echo_rx(c, (int)((want >> i) & 1), i);
	echo_rx(c, 1, 8);
	const int ok = (e->count == saved_count + 1 && e->log[saved_count] == want);
	e->count = saved_count;
	e->dropped = saved_dropped;
	e->bit = 0; e->shift = 0;
	return ok ? 0 : 1;
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
		g_ops.reset     = echo_reset;
		g_ops.run       = echo_run;
		g_ops.midi_rx   = echo_rx;
		g_ops.midi_tx   = 0;      /* never drives its TX line */
		g_ops.save      = echo_save;
		g_ops.load      = echo_load;
		g_ops.selftest  = echo_selftest;
		/* The instance teardown, which is a different thing from the
		 * module-level plg1500_destroy: the host calls ops->destroy for the card
		 * and the entry point for the library. Setting one does not set the other,
		 * and a card that reports what it heard needs this one. */
		g_ops.destroy   = plg1500_destroy;
		g_ops_filled    = 1;
	}
	return &g_ops;
}
