/* license:BSD-3-Clause *
 *
 * tests/plg/stub.c - a PLG card that does nothing, in C.
 *
 * The point of this file is that it is C. The ABI is a C interface, and a C
 * interface that has only ever been implemented from C++ is an interface whose
 * layout and linkage nobody has checked. Compiling this with $(CC) and loading
 * it with dlsym proves both: the struct offsets are the C ones, and the four
 * entry points are unmangled.
 *
 * It is also the reference a card author copies. Everything the host promises
 * and everything a card must not do shows up here as the smallest thing that
 * still does something:
 *
 *   - state is a fixed array, so save/load need no allocation
 *   - run() touches nothing but its own struct and the io it was handed
 *   - the parameters are static, so params() allocates nothing
 *   - the ROM is asked for in create(), not in run()
 *
 * What it deliberately does NOT do: claim to be a real card. model_id is
 * PLG_MODEL_ANY, so a host that trusts the panel's PLG lamps learns nothing from
 * it. A card that claims PLG_MODEL_DX has to be one.
 */
#include "plg/plg1500.h"

#include <string.h>

#define STUB_RAM_BYTES 4096

struct stub {
	/* Something a test can see change: a counter the host does not know about,
	 * a ramp it can watch, and a parameter that moves. */
	uint32_t frames;
	int32_t  ramp;
	int32_t  level;
	uint32_t param_value;
	int      midi_phase;
	/* RAM the host holds for us, which is what a real card's bus writes into.
	 * The host saves it, so a project comes back with it intact. */
	uint8_t *ram;
	uint32_t ram_len;
	int      reset_count;
};

static const plg_card_info g_info = {
	PLG_ABI_VERSION,
	PLG_KIND_SELF,
	PLG_MODEL_ANY,
	PLG_F_NEEDS_ROM,
	"smu2000.stub",
	"S-MU2000 stub card",
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
			strncpy(err, "host abi mismatch", err_len - 1);
		return 0;
	}
	if (g_info.flags & PLG_F_NEEDS_ROM) {
		/* The ROM is asked for here, in create, where allocating is allowed. A
		 * card that asked in run() would be allocating on the audio thread. */
		uint32_t len = 0;
		const uint8_t *rom = host->rom(host->ctx, 0, &len);
		if (!rom || len == 0) {
			if (err && err_len)
				strncpy(err, "stub wants a ROM and there is none", err_len - 1);
			return 0;
		}
	}
	struct stub *s = (struct stub *)host->alloc(host->ctx, sizeof(struct stub));
	if (!s) {
		if (err && err_len)
			strncpy(err, "no ram", err_len - 1);
		return 0;
	}
	/* Bus RAM, as a card with a CPU bus would ask for. Zeroed by the host. */
	s->ram = (uint8_t *)host->alloc(host->ctx, STUB_RAM_BYTES);
	s->ram_len = s->ram ? STUB_RAM_BYTES : 0;
	s->level = 1 << 20;
	/* The cast is needed, and this file is C on purpose: plg_card is an
	 * incomplete type, so `struct stub *` does not convert to it implicitly the
	 * way it does in C++. The first Windows cross build of this file is what
	 * found that out. */
	return (plg_card *)s;
}

PLG_EXPORT void plg1500_destroy(plg_card *c)
{
	/* The RAM belongs to the host: it frees what alloc handed out, in eject. A
	 * card that freed it here would be freeing a pointer the host still has in
	 * its save list. */
	(void)c;
}

/* ---- the rest of the table -------------------------------------------------
 *
 * Written as functions rather than as a static initialiser because the struct has
 * grown since version 1 and a static initialiser would have to be edited every
 * time it does. Assigning member by member means a member added later is simply
 * left NULL, which the host already knows how to read.
 */

static void stub_reset(plg_card *c)
{
	struct stub *s = (struct stub *)c;
	s->frames = 0;
	s->ramp = 0;
	s->reset_count++;
}

static void stub_run(plg_card *c, plg_slot_io *io)
{
	struct stub *s = (struct stub *)c;
	if (!io)
		return;
	if (!io->reset) {
		/* Nothing is running, so say nothing. A card that carried on here would
		 * be making noise out of a machine that is still resetting. */
		io->out[0] = io->out[1] = 0;
		return;
	}
	s->frames++;
	/* A ramp the test can check, and the bus RAM written to as a real card's
	 * firmware would write its registers. */
	s->ramp += 1024;
	if (s->ram)
		s->ram[4] = (uint8_t)(s->frames >> 8);
	/* Pass one channel through and add a fixed offset on the other, so a test can
	 * tell the two apart and tell a stale buffer from a fresh one. */
	io->out[0] = io->in[0] + s->level;
	io->out[1] = s->level;
}

static int stub_midi_tx(plg_card *c)
{
	/* The line as it is now, not a queued bit: the host samples this at the
	 * SCI4's bit times and there is nowhere to put a queue. A 500 Hz square from
	 * the sample clock is enough for a test to see edges. */
	struct stub *s = (struct stub *)c;
	s->midi_phase ^= 1;
	return s->midi_phase;
}

static size_t stub_save(plg_card *c, void *dst, size_t cap)
{
	const struct stub *s = (const struct stub *)c;
	const size_t n = sizeof(struct stub);
	if (!dst || cap < n)
		return n;              /* how much is needed; nothing written */
	memcpy(dst, s, n);
	return n;
}

static int stub_load(plg_card *c, const void *src, size_t n)
{
	struct stub *s = (struct stub *)c;
	if (n != sizeof(struct stub))
		return 1;
	memcpy(s, src, n);
	/* The RAM pointer is not ours to restore: it belongs to the host and is
	 * handed back in the section after this one. A card that saved it and
	 * restored it would be pointing at freed memory. */
	s->ram = 0;
	s->ram_len = 0;
	return 0;
}

static size_t stub_params(plg_card *c, plg_param_desc *out, size_t cap)
{
	struct stub *s = (struct stub *)c;
	(void)s;
	/* Two parameters, static, so params() allocates nothing and can be called
	 * from a screen at any time. A Japanese name is here on purpose: the ABI says
	 * names go out untranslated, and this is the test that it does. The count
	 * returned is how many the card *has*, which is what lets a host say that a
	 * list did not fit the page. */
	static const plg_param_desc p[] = {
		{ 1, PLG_PARAM_F_INT,  0, 100, 50, "level" },
		{ 2, PLG_PARAM_F_BOOL, 0, 1,   1,  "\xe5\x88\x9d\xe6\x9c\xac" },
	};
	if (out && cap)
		memcpy(out, p, (sizeof(p) / sizeof(p[0]) < cap) ? sizeof(p) : cap * sizeof(p[0]));
	return sizeof(p) / sizeof(p[0]);
}

static int stub_set_param(plg_card *c, uint32_t id, int32_t value)
{
	struct stub *s = (struct stub *)c;
	switch (id) {
	case 1:
		if (value < 0 || value > 100)
			return 1;
		s->param_value = (uint32_t)value;
		s->level = value << 14;      /* 0..100 -> 0..1<<20 */
		return 0;
	case 2:
		s->param_value = value ? 1u : 0u;
		return 0;
	default:
		return 2;                    /* not mine: the host can say so */
	}
}

static int stub_get_param(plg_card *c, uint32_t id, int32_t *value)
{
	struct stub *s = (struct stub *)c;
	if (!value)
		return 1;
	switch (id) {
	case 1: *value = (int32_t)s->param_value; return 0;
	case 2: *value = s->param_value ? 1 : 0; return 0;
	default: return 2;
	}
}

static int stub_selftest(plg_card *c)
{
	/* Called once, with no audio running, so this may be slow. It may also
	 * return something other than 0 to be refused: the host refuses a card
	 * whose selftest fails, and says so. */
	struct stub *s = (struct stub *)c;
	return s ? 0 : 1;
}

/* The table, filled in once. Assigning member by member means a member added in
 * a later ABI version is left NULL here, and the host already reads NULL as
 * "this card does not do that". */
static plg_card_ops g_ops;
static int g_ops_filled;

PLG_EXPORT const plg_card_ops *plg1500_ops(const plg_card *c)
{
	(void)c;
	if (!g_ops_filled) {
		memset(&g_ops, 0, sizeof(g_ops));
		g_ops.size      = sizeof(g_ops);
		g_ops.abi       = PLG_ABI_VERSION;
		g_ops.reset     = stub_reset;
		g_ops.run       = stub_run;
		g_ops.midi_tx   = stub_midi_tx;
		g_ops.save      = stub_save;
		g_ops.load      = stub_load;
		g_ops.params    = stub_params;
		g_ops.set_param = stub_set_param;
		g_ops.get_param = stub_get_param;
		g_ops.selftest  = stub_selftest;
		g_ops_filled    = 1;
	}
	return &g_ops;
}
