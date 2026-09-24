# SH2 hot-spot hunt (sustained CPU per instance)

Goal: lower sustained CPU so a host runs more plugin instances. Throughput,
not latency or boot. Branch: `perf/sh2-hotspot`, on top of upstream/main.

## Method (all repeatable, no code changes so far)

- `SMU2000_PCPROF=15` — block-entry counts per 0x40 bucket, bailout
  reasons (`g_pc_prof_why`), slow-path memory counts (`g_slow_mem`).
  Does not stop the JIT.
- `SH2_JIT_TRACE=<cycle>,<count>,<file>` — per-instruction PC trace
  (note: `SH2_`, not `SMU2000_` prefix). Aggregate with
  `awk '{print $1}' | cut -d@ -f1 | sort | uniq -c | sort -rn`.
- `build/blocktime roms build/tests/dense.mid 256 2 1` throughout.
- ROM bytes read raw from `roms/mu2000_flash.bin` (no decryption:
  `m_decrypted_program` is the same space for MU2000).

## Measurements (dense.mid, steady song unless noted)

- 13.5M block entries, 5.1B instructions = **378 insns/entry**. Dispatch
  is negligible; block chaining is not the bottleneck.
- Slow-path memory: **96M calls** (61.8M reads + 34.8M writes, ~2% of
  insns, each a helper call + state traffic).
- Bailouts back to dispatcher: **2.6M** (~20% of entries: delay-slot
  1.3M + irq-flag 1.28M).
- Block-entry top buckets: `0x1277xx` cluster ≈ **37%** (mixer, see below).
- Trace windows: cycle 100M = **38% in one MMIO poll loop** (`0x115E0E`);
  150M = 4.5% in a compare loop (`0x1455F2`); 200M = 36% in an unrolled
  poll subroutine (`0xBD468`); 250M = mixer loop bodies. Run ends <300M
  cycles. Phases differ: polls dominate boot/transitions, mixer owns
  steady song.

## Findings

1. **Hot cluster `0x1277xx` = audio interrupt mixer.** RTE epilogue,
   MAC accumulation (`MUL.L` + `STS MACL`), endian handling, counted
   `cmp/ge` + `bf/s` loops, indirect `JSR` subroutines. 37% of SH2
   block entries in steady state.
2. **Poll loops are real but phase-bound.** `0x115E0E` spins on CMT0
   compare-match (`0xFFFF83D2`, `cmcsr0`) — up to ~100k consecutive
   iterations in boot windows. `0xBD468` is an unrolled poll subroutine.
   `0x1455xx` is a `memcmp`-shaped byte-compare loop. None dominate
   steady song; all cheapen boot/transitions if skipped.
3. **Fallback-coverage theory REFUTED.** Static suspicion (MAC.L,
   SWAP.B/W unhandled in the arm64 backend) died on measurement: a
   300k-steady-state opcode histogram is ~0% fallback ops. Cautionary
   tale inside: SH2 group-0 keys on the low **6** bits, so `0x0EA7` is
   `MUL.L R10,R14` (native), not an unknown — naive low-byte
   classifiers lie. The executed mix (branches, MOV, ADD/CMP, EXTU,
   MUL.L, PC-relative loads) is all natively JITted.
4. **LTO measured flat.** ThinLTO vs baseline: 0.79ms → 0.77ms/block,
   per-sample CPU within noise. Makes sense: the hot code is
   JIT-generated (LTO can't cross that boundary). Build dir removed.

## Still open (ranked)

Holding `icount` in W23 is IMPLEMENTED on this branch (commit
"Hold SH2 icount in W23 across the block"): one load in the prologue,
sub-only decrements, writeback before every helper call and on every
exit, reload after the `jit_exec` fallback (whose interpreter loop
consumes from the state count — found by inspection before it could
bite; the first version without the reload corrupted timing and failed
63 fingerprint rows). Full suite green serially; one dial-LCD row
flaked once under load (encoder pulse timing, passes in isolation).
1. **Measure it.** `blocktime` before/after on a cool machine.
2. **Bailout reduction.** 2.6M dispatcher round-trips (delay-slot +
   irq-flag paths). Batch irq checks; longer blocks.
3. **Wait-loop fast-forward.** The poll loops above, if they ever show
   up mid-song: recognize pure MMIO-test loops, jump `icount` to the
   awaited device event (CMT exposes `m_next_event`; pattern exists in
   `catch_up`). Needs a purity check (no stores/calls in the loop).
4. **MEG side untouched.** DSP numbers (≈700ns/sample/instance) were
   not investigated; NEON vectorization of the mixer is the big lever
   there, i.e. a rewrite, not a tune.

## Repro

```
SMU2000_PCPROF=15 ./build/blocktime roms build/tests/dense.mid 256 1 1
SH2_JIT_TRACE="250000000,300000,/tmp/jtrace.txt" ./build/blocktime roms build/tests/dense.mid 256 1 1
awk '{print $1}' /tmp/jtrace.txt | cut -d@ -f1 | sort | uniq -c | sort -rn | head
```
