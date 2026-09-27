**Where it stands: a card is recognised.** PLG-1 lights, the firmware stops
polling for a board and starts configuring the one it found, and the whole
exchange is checked by `tools/run_tests.py` step 2b.

The card in question is `tests/plg/answer.c`: forty bytes of reply, no sound, no
data, built into the program so it can be part of `make check`. It exists to
prove the host, not to be a synthesiser. What is still missing is everything
below "a card is recognised" - a real card, and audio.

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
- **A card's transmit timing.** A card cannot emit a byte from `run()`: the chip
  samples a bit every 2 us and an audio sample is 22.7 of them, so all ten edges
  would land inside one instant. The card is now **stepped** instead, by SCI4's
  own bit clock - `set_line_source()` on the chip, a free-running poll at the
  period `wait()` already computes from the divisor the firmware programmed. The
  card hands back one bit per call and cannot drift from the chip. A card must
  also **idle its line high**, and so must an empty slot: SCI4's multiplexed
  receive line is the AND of the selected lines, so an empty slot holding its line
  low masks out the card in another slot. `host::midi_tx()` returns 1 for that
  reason, and it is the one thing here that was wrong before it was measured.
- **SCI4's transmit path, which is the real blocker and is host-side.** MAME, with
  a real PLG150-AP in the slot, is the ground truth for what a card should be
  handed, and it is precise: **nine per-bit events per byte, steps 0..8** (eight
  data bits LSB first, then the stop bit), with the target steady at 07
  throughout. It also shows why the edge-driven interface could never work — the
  card's line carries **7787 transitions for 3060 bit events**, two and a half
  times as many edges as bits. The per-bit `midi_rx` exists because of that
  measurement.
  Against it, our counters read: 159 `tx_start`, 1582 `tx_tick`, and **1280
  notifies per line — 8.05 per byte where it should be 9.** So we are still
  short about one bit per byte, and the card, which is handed 1280 of them,
  counts 9. Where the missing bit goes is the next thing to find; it is in the
  transmit chain, not in the card.
  The counters are in the tree for it: `mu2000::sci4_tx_debug()`,
  `plg::host::midi_rx_calls()`, printed by `boot`.
  firmware writes nine `0xf0` bytes to the card channel, and the card is handed
  **nine bits in total** instead of 81. So the chip shifts one byte out and then
  `tx_start()` is never re-entered for the next one, and everything downstream is
  downstream of that. This is why "the card's framing is a byte out" was the
  wrong diagnosis: the card was seeing one edge per byte (the start-of-byte edge
  from `tx_start`) and nothing else, so every "message" it decoded was one byte.
  A card cannot be recognised while the host is only sending a ninth of a
  message.
- **`midi_rx` is per bit now, and that is a real fix but not this one.** The
  interface hands a card the line and nothing else - no clock, no bit index -
  and MAME's cards cope only because they are clocked SCI peripherals that sample
  by phase. The firmware writes the target register often and `target_w()` drives
  the *disabled* multiplexed lines high, so every one of those writes puts an
  edge on the card's line that belongs to no byte. A card decoding that line on
  edges alone is lost before the host has said anything: twelve such edges, then
  a byte out for the rest of the run. The chip now hands the card one bit at a
  time with its place in the byte, as `tx_tick` shifts it, once per *selected
  line* - the three slots are one SCI4 channel told apart by the target
  register, which is why the firmware broadcasts and cannot tell which board
  answered.
- **Recognition now works, and what is left is the card's own protocol.** This
  bullet is kept because the path here was long and the shape of it is worth
  remembering. The card is stepped by SCI4's own bit clock, its replies reach the
  chip intact (zero framing errors, byte for byte what a real PLG150-AP sends),
  and **PLG-1 lights**. The 89-against-94 count quoted below was a symptom, not
  the fault: those extra host messages were the host retrying a scan whose answer
  never arrived intact. With the card framing its receive stream correctly the
  host's message count matches a real card's exactly - see 5b, which has the
  current measurement. What remains is that the card does not answer the host's
  37 follow-up reads, and until it does the boot stops where it stops.
- **A correction worth keeping.** "MU came on" was reported for a while as
  evidence that a card had been recognised. It is not: MU lights after about
  25 seconds **with no card at all** (`boot roms 700000000`, no `--plg`). The
  lamp print used to be gated on a card being present, so the baseline was never
  taken. `boot` now prints the lamps either way, because a signal with no
  baseline is not a signal. The only recognition observable is PLG-1.
- **Never start a card test from a snapshot.** The PLG scan happens *during* the
  boot sequence - `Checking PLG` is a boot screen - so anything that skips the
  boot never probes the slots. `boot` and `render` are cold by default
  (`--bootcache` is opt-in and must stay unused here), and neither loads NVRAM
  from disk, which is why the scan runs on every run.
### 5b. The panel does not answer with a card in the slot. That is faithful.

This was read as the last bug in the branch: with `--plg-builtin 0` the boot
lights PLG-1, then the button task stops scanning and no key does anything, and
that is also what the gui showed. It is not a bug here.

The button task's scan rate is the panel latch read at `0xc80000`. Counting it
in **MAME, with a real PLG150-AP in the slot**, over 600 emulated seconds:

| | panel latch reads | last read |
|---|---|---|
| MAME, no card | 8209, steady ~240/s | continues past 39 s |
| MAME, real PLG150-AP | **25** | **9.97 s, never again** |
| ours, `--plg-builtin 0` | 72 | stalls the same way |
| ours, no card | 48150 over 200 s, 240/s | continuous |

The AP in MAME is not a stub that prints canned replies - `plg150_ap_device` runs
the card's own `x5757b0.ic03` on a `swx00_device`, which delegates execution to
`h8s2000_device`. So the real card's firmware, on a real emulated CPU, leaves the
S-MU2000's button task dead after ten seconds, and our card leaves it dead the
same way.

Everything on the wire is identical to the real card, message for message. The
card hears all 39 (decoded offline from the card's own line at 32 us a bit, 340
bytes, zero framing errors) and answers two:

| | ours | real PLG150-AP in MAME |
|---|---|---|
| host -> card | 39 messages | 39 messages, **the same 39** |
| card -> host | 2 messages | 2 messages, **the same 33 bytes** |
| PLG-1 | lit | lit |

#### The 39 messages are two devices, and we only speak to one of them

Sorting the host's 39 messages by device id is what makes the remaining gap
legible:

| device id | messages | what they are |
|---|---|---|
| `F0 43 10 4E ...` | 22 | reads and polls - the control channel, the one we answer |
| `F0 43 10 4C ...` | 17 | **a second device the card cannot even recognise** |

The 17 are a `00 00 7e 00` and a sixteen-slot enumeration, `08 00 35 01` through
`08 0f 35 01` - one request per slot, which is the voice data phase. The card's
`is_poll` requires `msg[2] == 0x30` and `msg[3] == 0x4e`; a `0x4C` message is
`msg[2] == 0x10, msg[3] == 0x4c`, so **not one of the 17 can be answered by the
current card** and none is even counted as a poll.

That is also the best explanation for the AP's silence, and it fits the driver's
own comment. `plg150_ap_device` says **"Dual SWX00"** and instantiates **one**.
The first answers the `0x4E` control channel, which is why the identity exchange
works; the `0x4C` voice/data channel would be the second, which MAME does not
model, so those 17 requests go nowhere. `0x4E` and `0x4C` being two ids in the
same `F0 43 10 ..` envelope, and one of them handled, is what makes "the second
CPU owns the other channel" the reading rather than a guess.

**What is needed, then, is a CPU and memory map for a chip there is no
documentation for.** That is not a port; it is a new device, and it cannot be
inferred from the one CPU we can watch.

#### Porting the card is not the way out, and it is worth saying why

**Both MAME cards are incomplete:**

- **`plg150_ap_device`** is one SWX00 of the two it documents, and it loads a
  16 MB region named `swx00` that `map()` never references. The obvious
  hypothesis - that the firmware faults on unreachable tone data and gives up - is
  **refuted by measurement**: with catch-all read handlers over everything that
  is not the program ROM or the RAM, the firmware makes **zero** unmapped reads.
  Its profile is 97% at `0x446` for the first second and then a flat spread with
  a six-word table at `0x090a`-`0x0914` the hottest at ~1% each, running at 67% of
  its 16.9 MHz clock. It is busy, not deadlocked, and it is not reading the flash.
  So the 16 MB is dead weight in the driver, not the cause - and the cause is the
  missing second CPU, above.
- **`plg100_vl_device`** is better formed - an H83002 rather than a SWX00, and its
  DSP mapped at `0x400000` - but it **transmits nothing at all**. The host
  receives 0 bytes, so the firmware concludes NO BOARD and the panel is alive only
  because the machine is booting as if the slot were empty. Its audio routes are
  commented out in the driver.

Both are BSD-3, so licensing is not the obstacle; **completeness is**.

One thing the VL did settle: the device id is not the same for every card. The AP
is opened with `F0 43 10 4E ...` and the VL with `F0 43 08 6E ...`, so the
firmware picks its first move from the card model it expects to find.

#### What the hang actually is

Not a hardware wait, and not something the card's `0x4E` replies can influence:

| | hot PC region, 300k instructions sampled late |
|---|---|
| ours, no card | spread over `00127xxx` 58%, `00129xxx` 16%, `0011Dxxx` 10% |
| ours, with card | **`000BDxxx` 96.6%**, main loop never reached |

`000BD6xx` is MAC arithmetic (`MULS.W`, `STS MACL`, `CLRMAC`, `MOV.L R14,@R5`) -
a fixed-point interpolation routine in the host's own sound path. With a card
recognised the firmware ends up there and never comes back, so the main loop and
with it the button task never run. `SR=1` throughout, so interrupts are enabled
and the CPU is simply not returning.

Four things were varied and none of them moved it:

- **the card's replies** - answering every unrecorded address with 8 or 64 zero
  bytes (`SMU2000_CARD_UNKNOWN=zero|long`) makes the host receive 11 clean
  messages and ask for two more addresses (0x1011, 0x1013), and the host's own
  message stream still converges at exactly 39 over 100 s in every mode;
- **the card's audio** - `SMU2000_CARD_MUTE=1` changes nothing;
- **the category** - `00 07`, `00 00`, `00 01` and `00 0f` are all accepted and all
  hang identically, so it is not a data-dependent loop;
- **recognition itself** is the only thing that matters. There is a threshold:
  `00 ff` and `ff ff` are *rejected* (PLG-1 off) and then the machine boots
  normally at 390 latch reads - precisely the no-card baseline.

**So the boot is stalled in the host's firmware by recognition alone, and the card
cannot get past it on the `0x4E` channel however it answers.** Two real bugs did
come out of chasing this, both in `answer.c`:

- **The name reply was never sent.** The host asks for it with
  `F0 43 30 4E 01 00 00 F7`, which is eight bytes, and `is_name_poll` tested
  `msg_n == 7`. `msg_n` counts up to `sizeof(msg)` and `msg` is eight wide, so no
  message can ever have `msg_n == 7` - the test was unsatisfiable, and the card
  put one message on the wire per run instead of two. The identity read's test
  next to it, `msg_n == 8`, passes only by luck: the read is nine bytes, `msg_n`
  saturates at eight, and the eight bytes being compared happen to be the first
  eight.
- **The transmit mailbox was the transmit buffer.** One `tx_buf` served as both, so
  a reply queued while another was still shifting out overwrote the bytes being
  sent, and completing a message did `tx_next = tx_len`, discarding whatever was
  queued behind it. The mailbox and the wire buffer are now separate.

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
