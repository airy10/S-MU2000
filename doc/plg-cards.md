# PLG card slots — design note

The module ABI for a PLG card (a PLG150-DX and friends) and the host side that
loads one. Implementation in `src/plg/`, the contract itself in
`src/plg/plg1500.h`, the checks in `src/plgtest.cpp` and `tests/plg/stub.c`.

**Where it stands: the ABI, the host side (loading, lifetime, state, safety), and
the SCI4 line from the firmware to a card are done and checked.** What is not done
is the audio path - `mu2000::run_sample()` still does not call
`plg::host::run()` - and a card's own transmit timing. A card in a slot hears the
firmware; nothing yet hears the card.

## 1. What the hardware has

The MU2000 has three PLG card slots.

```
SH7043A (28MHz)
  |- 0xF00000  SCI4      talks to the PLG boards (three lines)
  |- slave SWP30 outputs 18,19  ->  PLG slots
     PLG card -> slave SWP30 inputs 10..15 (two per card)
```

Sources: MAME's `ymmu2000.cpp:407-425` (kept in `src/mame/` for reference, not in
the build), `doc/design.md:52`, `doc/dump/hardware.md:26`. On the real machine you
plug a PLG150-DX into one of these. The panel has `MU / PLG-1 / PLG-2 / PLG-3`
lamps (`src/ui/panel.cpp:794`) and a `GM2 / XG / PLG` mode label, and the boot
sequence puts a **Checking PLG** screen on the LCD.

S-MU2000 keeps all three slots **empty**. SCI4 is emulated, and that is not
optional: the firmware pokes those registers and waits on the interrupt, so
without it the main loop never gets going (`doc/design.md:81`). The slots have
nothing behind them.

The goal of this work is to get **one module into one slot and prove the
mechanism**, before writing any card. Three slots exist because the hardware has
three; one working slot is worth exactly as much as three of the same thing, and
a second slot only adds ways for the audio to cross wires.

## 2. Two kinds of card

PLG boards carry their own CPU, and the CPU decides the shape of the interface.

| kind | who runs the CPU | real card | what the host already has |
|------|------------------|-----------|---------------------------|
| `PLG_KIND_BUS` | **the host.** The module supplies the chips: address space, reset line, audio wires, MIDI wires | PLG150-DX | an SH7043 (`src/mame/cpu/sh7042.h`), the same part `src/mu2000.cpp:72` already runs |
| `PLG_KIND_SELF` | the module, entirely. MIDI bytes in, samples out | none — no real card is this | an FSVR engine, say |

`BUS` is the faithful one. Insert such a card and **the MU2000's own firmware
drives it**, from `Checking PLG` through the PLG mode, and the host does nothing.
That is the only route that lands on `doc/design.md`'s "run the real firmware
unchanged", and it is what this design is built for.

`SELF` has no firmware, so from the MU2000's point of view no card was inserted.
It exists so a third party can drop in an engine of their own. FSVR is GPL-3 and
cannot be vendored into a BSD-3 project, so an FSVR-based card is a `SELF` card
that **the user** installs. S-MU2000 ships no GPL code, and this is the cut that
keeps that true.

## 3. The promises

### The host's

- **`run()` is called from the audio thread, one sample at a time, at 44100 Hz.**
  Realtime safe: no allocation, no locks, no I/O, no exceptions. One sample is
  22.7 microseconds, and other people's code is going to be inside that loop
  44100 times a second.
- Insert, eject, save and restore happen on another thread, holding the machine
  lock — the same shape SmartMedia already uses (`src/vst3/engine.h:191-194`).
- **No ROMs are shipped** (README). A card is handed a file the user dumped off
  their own card, exactly as the MU2000's own ROMs are handled.

### The card's

- C functions and C data layout. No STL, no exceptions, no RTTI across the line.
- Four entry points, unmangled: `plg1500_get_info`, `plg1500_create`,
  `plg1500_destroy`, `plg1500_ops`.
- `id` goes into the save state and is compared on load, so it must not change
  between versions. Something like `smu2000.stub`.
- `model_id` is where a card names a real card. A card that stays on
  `PLG_MODEL_ANY` must not have the panel's PLG lamps lit on its behalf.

### What the host enforces (`src/plg/host.cpp`)

- **A different ABI is not inserted.** And when one of the four entry points is
  missing, the refusal **names it** — a forgotten `PLG_EXPORT` is the mistake a
  card author makes first, so the message says so.
- **A card whose selftest fails is not inserted.** A slot the screen can explain
  beats a card that misbehaves on the first note.
- **A card slower than 8 microseconds per sample is counted.** A hundred in a row
  and the host **stops calling it** (`faulted()`), because calling it again only
  makes the next block later too. The reason lands in `message()`.
  `SMU2000_PLG_BUDGET_US` moves the threshold.
- **The RAM a card asked the host to hold is part of the state.** The host writes
  into it from bus cycles, so a project saved without it would come back with a
  card holding someone else's registers.
- **A project whose card is not installed still opens.** A section with the wrong
  version, a hash that does not match, a different card in the slot — each is a
  *warning*, never a failure. The state is simply not restored. A broken PLG
  section must never be the reason a project will not open; that would be the
  cost of having added the feature.
- **The library is closed after the card is gone.** The descriptor and the ops
  table point into it, so `eject()` destroys the card first and closes second.

## 4. Where the boundary goes

**The slave SWP30's sample domain**: s32 on the same scale as
`mu2000::run_sample()`, one sample at a time, both directions.

**The serial convention belongs to the card.** That is the point of this
decision. Nobody yet knows what a PLG card actually puts on the wires to the
slave SWP30. The whole of MAME's commitment is `plg1x0.h`'s comment — two
midi-rate serial lines each way and two stereo sample streams each way — and the
rest of that file is empty. The YMP706's CHOUT/CHIN loop to its neighbour is the
same kind of link, and FSVR's `docs/ymp706_registers.md` calls register 0x270
"the CHOUT/CHIN loop" without ever needing to know what the wire format is,
because the wire format is the chip's business.

So version 1 decides two things and no more:

1. **The boundary carries samples.** This side of it only moves 44100 Hz samples
   in and out, and the card's serial codec stays inside the card. Card research
   happens where the card is.
2. **`in[2]` and `out[2]`.** MAME wires two wires (outputs 18 and 19) and that
   is what the slots have, so that is what this has. **This is an expectation,
   not a measurement.** It is settled by reading the PLG150-DX schematic
   (`doc/plg150-dx-sm.pdf` in rgwan's repository). **Widening it means bumping
   `PLG_ABI_VERSION`** — `reserved` is there so it *can* be filled in.

For the same reason MIDI crosses as SCI4's **bit line**, and no baud clock is
handed over. MAME's connector has no clock signal at all - `plg1x0.h` carries
only `midi_tx` and `midi_rx` - while the card side sets its SCI to an **external
500 kHz** clock (`sci_set_external_clock_period(0|1, 500 kHz)` in
`plg150-ap.cpp`). So MAME has a card waiting for a clock that the connector does
not provide, and its PLG cards cannot actually exchange a byte with the host.
That is worth knowing twice over: it means the 129 bytes crossing the line here
are ahead of MAME rather than behind it, and it puts a number on the timing gap -
**4 µs per bit, 2 µs per half-bit, against an audio sample of 22.7 µs.** A card
has to be called five or six times per sample to hold that clock, which is why the
host offers a line to push but no timer to push it on.

## 5. What is done, and what is not

Done and checked:

- **The firmware to card direction.** `mu2000` wires SCI4's TX line into
  `plg::host::midi_rx()` and a card's TX line back into SCI4's RX, transcribed
  from MAME's `ymmu2000.cpp:409`; slot n is SCI4 port 30+n. The card host is in
  `SRCS` now, because the machine calls into it.
- **`boot --plg <card>`**, which inserts a card before the machine starts.
- **`tests/plg/echo.c`**, a card that needs no protocol knowledge at all: it
  counts the bytes the firmware sends and reports them. It exists to answer one
  question - does anything the firmware sends actually arrive - and it answers it
  with 129 bytes and six well-formed sysex frames, including the identity read
  `F0 43 10 4E 00 10 02 01 F7`.
- **`tools/run_tests.py` step 2b**, which runs the above and checks the frames.
  It needs ROMs but no renders, so it costs a fraction of the audio suite.

Not done:

- **The card's audio reaching the speaker.** The wire is done: `run_sample()`
  hands `melo 14,15` to the card and writes what comes back to `meli 10..15`, and a
  test proves it by having a card put a known constant on `out[0]` and reading it
  back off `meli 10`. What does *not* happen is the card becoming audible, and
  the reason is in the chip rather than in this code. MAME's SWP30 mixer loop
  (the same code this project vendored) reads its inputs as: taps below `0x40`
  are AWM2 voices, `0x40`-`0x4f` are MEG registers, and `0x50`-`0x5f` are the
  serial inputs, so `meli 10` is tap `0x5a`. **A tap contributes only if its route
  is non-zero** - the loop skips any mixer entry whose three route registers are
  all zero. Which routes exist is programmed by the firmware, and with no card
  recognised there is no reason for it to open the PLG tap, so the value arrives
  and stops.
  **That is expected to change on its own once a card is recognised**, because the
  firmware doing the programming is the real one - which is an argument for
  getting recognition right before building anything else.
- **A card's transmit timing**, which is why the card above answers nothing.
  SCI4 samples a bit every few microseconds and an audio sample is 22.7 of them,
  so a card that emits a byte from `run()` puts all ten edges inside one instant
  and the chip sees framing errors. A card has to be called at the bit rate, and
  the honest place to do that is where SCI4 samples rather than in a second copy
  of its divisor.
- **A real card** (`src/plg/plg150dx_device`): the SH7043, the two flashes, a
  YMP706 model. The connector and SCI4 are already there to hang it on.
- **Discovery** (a `plg.txt` beside the bundle, the way `roms.txt` works) and a
  screen. The panel is the firmware's; a `SELF` card needs a page of its own.
- **The second load line** for `PLG_F_HEAVY`, added to the existing
  `vst3::engine::publish_load` meter.

## 6. Where the card knowledge comes from

Three repositories were read for this. **No code was taken from any of them.**

### rgwan/fs1r_firmware_RE — the card itself

- `dmps/plg150-dx/PLG150-DX.BIN`, 1 MB, is the card firmware: the two MBM29F400B
  flashes (512 KB each, IC3 and IC4) concatenated. The CPU has **no internal
  ROM**, so one dump is the whole card.
- `doc/plg150-dx-sm.pdf` and `doc/plg150-an-sm.pdf` are service manuals. Their
  parts lists:

  | | PLG150-DX | PLG150-AN |
  |---|---|---|
  | CPU | HD6437043E00F (IC2) | HD6413002FP16 (IC1) |
  | tone generator | **YMP706-F (IC8)** | **YSS236-F (IC4), the VOP3** |
  | effects | the YMP706's digital filter | YSS233-F (IC5), MDSP |

  The DX's CPU is **the same SH7043 as the MU2000's**, which this repository
  already has. The AN's tone generator is **the same VOP3 as the FS1R's**, and
  the same chip as the AN1x's and AN200's.
- `tools/extract_presets.py` gives the card ROM's data layout: 256 native voices
  at 0x25C280, 384 performances at 0x20A000, 90 Fseqs at 0x300A00 and 0x283000,
  1012 DX7 VCEDs at 0x5461B. **The same formats as the FS1R's**, which follows
  from it being the same chip.
- `la/*.dsl` are DSLogic captures — of the FS1R's UART, not a PLG board, but the
  method transfers directly to the SCI4 lines.
- The README's own note: *"PLG150-DX actually use the same digital oscillator
  (tone generator) chip as the FS1R (FS, PN: YMP706)"*, and it runs on an SH2
  without internal ROM, "which is far better for reversing than FS1R". So a
  PLG150-DX card does not have to import any of FSVR's algorithms.

### musicastudio/FSVR — measurements of the chip, not code

- `docs/ymp706_registers.md` is the YMP706 register map: an 8-bit write-only bus,
  A0-A9, a PBUSY spin before every byte, channel select at 0x3FF, 8-byte operator
  stripes. **The PLG150-DX's YMP706 is this part.** `src/fs1r/chips/ymp706.cpp`
  and `cal.h` are its model.
- `docs/aeg.md`, `formant.md`, `skirt.md`, `noise.md`, `detune.md` are one
  measurement each. A PLG150-DX card needs the same chip characterised, and these
  are the characterisation. `docs/vop3_microcode.md` and `docs/vop3/` are the
  VOP3's microcode, which is the raw material for the AN.
- **`captures/requests/*.mid`, the capture kit.** This is the part worth copying.
  Standard MIDI Files that play a measurement into a real unit, each opening and
  closing with a marker so a recording aligns itself against them. Doing the same
  for a PLG card means writing the measurement as a MIDI file, having somebody
  play it into a real MU2000 with a card in it, and recording that. **It is the
  only route from a modelled chip to a measured one**, and it is the same
  discipline this project already uses against real hardware.
- `src/fs1r/hardware.h` (facts read off the board, never a knob) against
  `src/fsvr/tuning.h` (our own cost choices, where changing a value must not
  change the output) is the split to copy when `src/plg/plg150dx/hardware.h`
  eventually exists. So are `docs/Differences.md` (the places the model knowingly
  differs, each with the firmware address behind it) and the KNOWN / MEASURED /
  MODELLED / UNKNOWN lists in `STATUS.md`. **A card needs the same three-way
  split**, because a project whose value is verified fidelity cannot afford to
  present a guess with the same confidence as a measurement.

### MAME's plg1x0

- `src/devices/bus/plg1x0/plg1x0.h` is the connector's contract, and its comment
  is the only positive information anyone has about the interface. BSD-3-Clause
  (Olivier Galibert), so the same provenance rule as `swp30` and the SH-2 files
  (`doc/design.md:24-38`).
- The MU1000 has the same connectors: two of them, with `m_ext1` a
  `required_device` and `m_ext2` optional, both configured with `nullptr` as the
  default card. So a MAME MU1000 has an empty PLG slot 1 as well, and the SCI4
  port mapping is the same one this project uses - `write_tx<30/31/32>` to slots
  1, 2 and 3 - which is what `mu2000.cpp` transcribes.
- `plg100-vl.cpp` and `plg150-ap.cpp` show what is inside two cards: the
  PLG100-VL is an H83002 plus a YSS217 (DSP-V), the PLG150-AP is an SWX00.
- `sound/dspv.*` is the YSS217, and it is **almost nothing but a register map**:
  `execute_run()` is empty, `snd_w` and `snd_r` only logerror. It derives from
  `cpu_device`, so the host side is understood and only the core is blank. **That
  is the mountain, if VL is the target.**

## 7. Order of work

1. **One real card, in the tree.** `src/plg/plg150dx_device`: connector, SCI4 both
   ways, the SH7043, the two flashes, a YMP706 stub. `Checking PLG` lights up by
   itself at the end of it, which is the milestone.
2. **Wire it to `run_sample()`.** `in[2]` from melo 18,19 and `out[2]` to meli
   10, 12, 14. The card host is already in `SRCS`; this is just the call.
3. **Work out what a card has to answer.** The identity read is known -
   `F0 43 10 4E 00 10 02 01 F7`, two bytes from 0x0010 - and so is the reply
   opcode, since the MU2000 answers its own 4E requests with 4C. What the two
   bytes *mean* is not, and neither is which of the eighteen bracketed categories
   each value picks. The firmware's side is around `0x000c1d40`, reached through
   the pointer table at `0x000c1dd0`, and `build/sh2dis` will read it. The other
   half needs a capture from a real MU2000 with a board in it.
4. **AN** (VOP3; FSVR's VOP3 material is the raw material) and **VL** (YSS217;
   realistically a project of its own).
5. **Open it to card authors.** `plg.txt`, a screen, and the stub as the thing to
   copy. An FSVR-based `SELF` card becomes possible at that point and ships from
   whoever wants it.

DX first because it is the cheapest of the three by a wide margin, and the reason
is not the CPU: **FSVR has already spent a year measuring that chip**, so a card
built here starts from a characterised part instead of a blank one. VL starts
from nothing.

## 8. Deliberately not in this branch

- **Any PLG ROM.** The FS1R EPROM, the PLG150-DX firmware, the YMP706 tables.
  Data dumped out of a third party's card is not ours to redistribute; the user
  dumps it. This repository ships source.
- **FSVR's C++.** GPL-3, so not vendored. Using it means a `SELF` card the *user*
  installs, and the combination of this host with that card is GPL-3 — an
  obligation on whoever distributes the combination, not on this repository. That
  separation is the other reason to make cards loadable rather than built in.
- **User-facing strings.** Parameter names belong to the card and go out
  untranslated, so nothing is added to `src/ui/texts*`; adding card vocabulary
  there would put it into the localisation tables this project checks.
