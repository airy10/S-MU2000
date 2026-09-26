// license:BSD-3-Clause
//
// plg1500.h - the PLG card module ABI, version 1. The full design note is
// doc/plg-cards.md; this file is the whole contract.
//
// The MU2000 has three PLG card slots (doc/design.md:52, doc/dump/hardware.md:26)
// and this project leaves all three empty. SCI4 at 0xf00000 is emulated because
// the firmware pokes it and waits on its interrupt even with nothing plugged in;
// the three slots themselves have no sound chip and no code behind them
// (src/mame/ymmu2000.cpp:407-425 is the MAME original, kept for reference and
// not in the build).
//
// This ABI defines a module that anybody can drop into one of those slots. The
// card carries its own licence and its own ROM policy; S-MU2000 keeps its own
// licence and its own "no hardware-derived data" rule, and neither constrains
// the other.
//
// ---- Where the boundary goes ----
//
// The boundary is the SWP30 sample domain: s32 on the same scale as
// `mu2000::run_sample()` (src/mu2000.cpp:2908), one sample at a time, both
// directions.
//
// The *serial* convention belongs to the module. This side of the boundary only
// knows how to move 44100 Hz samples. What a card puts on the wires to the slave
// SWP30 is the card's business, not the host's - exactly as the YMP706's
// CHOUT/CHIN loop to its neighbour is the chip's business rather than the
// SH7044's (FSVR's docs/ymp706_registers.md calls register 0x270 "the CHOUT/CHIN
// loop"). Neither wire is characterised yet on either side, so the host hands
// over samples and the module does the codec. That way the serial format is
// settled in the place that researches it, instead of being frozen into a host
// API that every future card would then have to agree with.
//
// One sample at a time is the easy part: `run_sample()` already returns one
// sample, which is 22.7 microseconds at 44100 Hz. Batching would buy nothing but
// a different clock at the boundary.
//
// NOTE: `in[2]` / `out[2]` being enough is an *expectation*, not a measurement
// (doc/plg-cards.md, section 4). Widening them means bumping PLG_ABI_VERSION.
//
// ---- What the host promises ----
//
//   * run() is called from the audio thread and must be realtime safe: no
//     allocation, no locks, no I/O (doc/plg-cards.md, section 3).
//   * Insert, eject, save and restore happen on another thread while the
//     machine lock (`vst3::engine::m_machine`) is held. SmartMedia's insert and
//     eject already work this way (src/vst3/engine.h:191-194).
//   * No MIDI bit clock is handed over. The connector is *assumed* to carry two
//     audio wires and SCI4's TX line and nothing else, and that assumption has
//     no evidence behind it yet. `reserved` is there so it can be filled in once
//     the schematic settles it - and filling it in means bumping the ABI.
//
// ---- What a module promises ----
//
//   * C linkage, C data layout. No STL, no exceptions, no RTTI across the line.
//   * Every entry point the host calls is realtime safe; run() comes from the
//     audio thread.
//   * The PLG_MODEL_* and PLG_F_* values above are the whole vocabulary. The
//     `id` string goes into the save state and is compared against, so it must
//     not change between versions, and it carries no sentiment: something like
//     "yamaha.plg150-dx".
//
// ---- Provenance ----
//
// The PLG1X0 numbering is MAME's bus (`plg1x0.h`, BSD-3-Clause, Olivier
// Galibert) spelled the same way; PLG1500 is the PLG1X0 1500 type. PLG100-VL and
// PLG150-VL sit on the same connector, though MAME only implements PLG100-VL
// (`plg100-vl.cpp`).

#ifndef S_MU2000_PLG_PLG1500_H
#define S_MU2000_PLG_PLG1500_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// 2026-09-26, version 1. Same number means the same thing; a different thing
// gets a different number. Version 0 is treated as "unreadable" - if anything
// claims to be 0 it predates this file.
#define PLG_ABI_VERSION 1

// Only the module side needs an export. The host looks the entry points up by
// name (dlsym / GetProcAddress), so it needs no import declaration, and keeping
// the two apart stops the reader and the writer of this header from sharing
// library symbols. VST3's pluginterfaces does the same.
#if defined(_WIN32)
#  define PLG_EXPORT __declspec(dllexport)
#else
#  define PLG_EXPORT
#endif

// What may sit in a slot. This is MAME's device_plg1x0_interface (`plg1x0.h`)
// reduced to what a binary interface can carry. MAME's comment promises two
// midi-rate serial lines each way and two stereo sample streams each way, which
// is the whole of what is known.
enum {
	// The host runs the card's CPU; the module supplies the chips (address
	// space, reset line, audio wires, MIDI wires).
	//
	// PLG150-DX is this shape: an SH7043 and everything around it is already
	// in this repository (src/mame/cpu/sh7042.h, and src/mu2000.cpp:72 runs
	// one). Plug such a card in and the MU2000's own firmware drives it, from
	// "Checking PLG" through the PLG mode, with the host doing nothing.
	PLG_KIND_BUS = 1,
	// The module owns everything: MIDI bytes in, samples out. There is no card
	// firmware, so the MU2000 does not see a card at all. An FSVR engine would
	// arrive in this shape.
	PLG_KIND_SELF = 2
};

// Which real card a module claims to be. The host needs this because the
// firmware is what lights the panel's PLG-1..3 lamps (src/ui/panel.cpp:794) and
// the host cannot light them on the firmware's behalf, and because a combination
// the real hardware would refuse is better refused early.
enum {
	PLG_MODEL_ANY = 0,   // claims to be none of them in particular
	PLG_MODEL_DX  = 1,   // PLG100-DX / PLG150-DX  ... YMP706
	PLG_MODEL_AN  = 2,   // PLG150-AN              ... YSS236 (VOP3) + YSS233 (MDSP)
	PLG_MODEL_VL  = 3,   // PLG100-VL / PLG150-VL  ... YSS217 (DSP-V)
	PLG_MODEL_AP  = 4    // PLG150-AP              ... SWX00
};

// The PLG150-AN's parts list, from doc/plg150-an-sm.pdf in rgwan's
// fs1r_firmware_RE, reads YSS236-F: IC4 and YSS233-F: IC5. The YSS236 is the same
// VOP3 the FS1R carries, and it is the AN1x and AN200's synthesis engine too
// (FSVR's docs/vop3_microcode.md). So an AN card is the case where the chip
// arrives with its program already written, and the work is decoding rather
// than modelling.
//
// The PLG150-DX is the same story one generation back: YMP706-F: IC8, the chip
// FSVR's docs/ymp706_registers.md documents register by register.

enum {
	// rom() is required, or the card cannot start. The host asks for the ROM
	// path *before* inserting, the way it asks for the MU2000's own ROMs.
	PLG_F_NEEDS_ROM = 1u << 0,
	// run() is expected to be expensive. The host then reports the card's load
	// on a line of its own, measured the way `vst3::engine::publish_load`
	// measures, so three cards do not add up into one number.
	PLG_F_HEAVY     = 1u << 1
};

typedef struct plg_card plg_card;   // opaque, the module's own
typedef struct plg_host plg_host;   // what the host provides, below

// Read before inserting. It has to stay readable when the card is broken, and it
// has to stay readable when nothing is plugged in at all, because a bad
// descriptor in one slot must not lock the other two out. Three pointers and
// four integers: cheap enough to read from any thread, and copied rather than
// kept, so the module is free to build it in a constructor.
typedef struct plg_card_info {
	uint32_t    abi;       // == PLG_ABI_VERSION. Anything else is not inserted
	uint32_t    kind;      // PLG_KIND_*
	uint32_t    model_id;  // PLG_MODEL_*
	uint32_t    flags;     // PLG_F_*
	// Stable identity. It goes into the save state and is compared on load, so
	// it must not change between versions, and it carries no sentiment:
	// something like "yamaha.plg150-dx".
	const char *id;
	// Shown as it is. The host has no translation table for it and will not
	// grow one: adding card names to src/ui/texts*.h would put the card's
	// vocabulary into tools/check_texts.py's tables, which count the emulator's
	// own strings. A card author who wants Japanese labels may write them.
	const char *name;
} plg_card_info;

// One sample, both ways, on the slave SWP30's scale: the same s32 the melo
// lines use, clamped at 1<<26 (src/mu2000.cpp, next to set_meli).
//
//   PLG1:  in = slave melo 18,19   out = slave meli 10,11
//   PLG2:  in = the same two       out = meli 12,13
//   PLG3:                          out = meli 14,15
//
// All three cards hearing the same two wires is what MAME does too:
// ymmu2000.cpp:407-427 routes the slave's outputs 18 and 19 to each of the
// three connectors. Only the return path differs, two meli lines per card.
//
// `reset` is 1 while the slot runs. A card that wants to be certain of starting
// from a known state returns early and zeroes out[] while it is 0, rather than
// relying on the host to clear the buffer.
//
// `reserved` stays zero in version 1. It is not padding: it is where the
// connector's other signals go once the PLG150-DX schematic has been read, and
// putting something there means a new PLG_ABI_VERSION.
typedef struct plg_slot_io {
	int32_t in[2];       // the MU2000's side; the host writes out[]
	int32_t out[2];      // the card's return; the host reads it into meli
	int32_t reset;
	int32_t reserved[4];
} plg_slot_io;

// A parameter the host can show and move. The card decides what these are; the
// host has no idea what any of them mean.
//
// Names go out as they are, untranslated. One whose name does not fit in
// PLG_PARAM_NAME_MAX is truncated, so a card author may write Japanese.
#define PLG_PARAM_NAME_MAX 32

enum {
	PLG_PARAM_F_INT  = 1u << 0,   // an integer range; min == max is legal
	PLG_PARAM_F_BOOL = 1u << 1,   // 0 or 1 only; a host may draw a switch
	PLG_PARAM_F_ENUM = 1u << 2    // a fixed set; a host may draw a menu
};

typedef struct plg_param_desc {
	uint32_t id;                        // the card's; echoed back by param_changed
	uint32_t flags;                     // PLG_PARAM_F_*
	int32_t  min, max, def;             // a closed interval
	char     name[PLG_PARAM_NAME_MAX];  // UTF-8, NUL terminated
} plg_param_desc;

// What the host hands over. All of it is realtime safe, and none of it may be
// called outside create and load - alloc especially, since run() must not
// allocate.
//
// ROMs are not shipped by S-MU2000 at all (README: "hardware-derived data is not
// published or carried"), exactly as the MU2000's own ROMs are not. What comes
// back here is a file the user dumped off their own card. Keep the pointer, do
// not copy it, and do not free it.
typedef struct plg_host {
	uint32_t abi;      // == PLG_ABI_VERSION, so a module can check the arithmetic
	void    *ctx;

	// The card's firmware or waveform ROM. part is the module's own numbering
	// (0, 1, ...). Out of range returns NULL and leaves *len alone.
	const uint8_t *(*rom)(void *ctx, uint32_t part, uint32_t *len);

	// Flat RAM the host may write from a bus cycle. This is what MAME's
	// address_space became in this codebase (doc/design.md:86-96, "replace the
	// plumbing, leave the algorithm alone"), and a card asks for RAM here
	// instead of mapping anything. Legal inside create and load; never from run.
	void *(*alloc)(void *ctx, size_t bytes);
	void  (*free)(void *ctx, void *p);

	// The card's interrupt lines into the SH-2 INTC the host owns. A card calls
	// this from inside a bus access. Passing a level through a stored variable
	// is the wrong shape: the host samples it after the access returns.
	void  (*irq)(void *ctx, int line, int state);

	void     (*log)(void *ctx, const char *msg);
	// The host's 44100 Hz sample counter, so a card does not keep its own.
	uint64_t (*sample_clock)(void *ctx);
	// The card's own load line, used only when PLG_F_HEAVY is set.
	void     (*publish_load)(void *ctx, double pct);
	// The card moved one of its own parameters (its firmware echoing a request,
	// say). The host forwards it to its screen and, when the host is a plug-in,
	// to the DAW - the same path a screen edit takes
	// (`vst3::engine::set_edit_handlers`). Legal from run(); it only stores.
	void     (*param_changed)(void *ctx, uint32_t id, int32_t value);
} plg_host;

// A module's entry points. `size` and `abi` at the front are required: they are
// how the host cuts the table to the version it was built against, so appending
// to the end of this struct cannot break an older card.
//
// The order below is the order of version 1, fixed on 2026-09-26 before anything
// was published. From here on new members go at the end and nothing already
// published may move or change meaning.
typedef struct plg_card_ops {
	size_t   size;    // sizeof(plg_card_ops) as the module was compiled
	uint32_t abi;     // == PLG_ABI_VERSION
	void   (*reset)(plg_card *c);
	// One sample, from the audio thread (the promise in doc/plg-cards.md, section 3)
	void   (*run)(plg_card *c, plg_slot_io *io);
	// The level of the card's MIDI TX line, 0 or 1. The host calls this at the
	// SCI4's bit times, so it must return the line as it is *now* and not
	// consume a queued bit: there is nowhere to put a queue.
	int    (*midi_tx)(plg_card *c);
	// State. save() returns the number of bytes written, or the number it needs
	// when cap is too small, having written nothing. load() returns 0 on success.
	size_t (*save)(plg_card *c, void *dst, size_t cap);
	int    (*load)(plg_card *c, const void *src, size_t n);
	void   (*destroy)(plg_card *c);
	// The parameters to show, up to cap of them. Returns how many the card
	// *has*, which may be more than cap: that is how a host tells a card author
	// their list did not fit the page. An empty list is fine, and so is NULL for
	// out when cap is 0 (a card may use that to be asked only for the count).
	size_t (*params)(plg_card *c, plg_param_desc *out, size_t cap);
	// Move one of the ids params() just listed. Called from the host's own
	// thread - a click, a DAW automation point - and never from run(), because a
	// card changing a register under the audio thread's feet is exactly the sort
	// of thing this interface exists to make impossible. Returns 0 when the id
	// was one of the card's, so a host can tell "refused" from "not mine".
	int    (*set_param)(plg_card *c, uint32_t id, int32_t value);
	int    (*get_param)(plg_card *c, uint32_t id, int32_t *value);
	// Does this card work at all? 0 passes. Called once, with no audio running,
	// so a card can afford to be slow here.
	int    (*selftest)(plg_card *c);

	// ---- PLG_KIND_BUS only. read8 and write8 may both be NULL ----
	//
	// Both halves of the address map the host decoded, most significant byte
	// first. A card whose CPU bus reaches further than its own chips - a VOP3 at
	// 0x400000-0x40007f on a PLG150-AN, say - leaves the rest to the host's
	// address space, and an unimplemented address reads as 0. Nothing may trap
	// here, not even the YMP706's PBUSY wait, which is a spin on a port bit
	// rather than a bus cycle (doc/plg-cards.md, section 4).
	void   (*read8)(plg_card *c, uint32_t addr, uint8_t *data, uint32_t mask);
	void   (*write8)(plg_card *c, uint32_t addr, uint8_t value, uint32_t mask);
} plg_card_ops;

// ---- The entry points. These four names and nothing else -------------------

PLG_EXPORT const plg_card_info *plg1500_get_info(void);

PLG_EXPORT plg_card *plg1500_create(const plg_host *host, void *ctx,
                                     char *err, size_t err_len);

PLG_EXPORT void plg1500_destroy(plg_card *c);

PLG_EXPORT const plg_card_ops *plg1500_ops(const plg_card *c);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // S_MU2000_PLG_PLG1500_H
