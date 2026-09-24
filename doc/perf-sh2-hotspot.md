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
exit, reload after every call (a helper can abort the timeslice,
zeroing the state count, or consume from it as the interpreter
fallback does — trusting the register past a call overruns the slice
and corrupts accounting; found via a deterministic piano divergence
at one byte offset, after several stale-binary false alarms along
the way). Full serial suite green; one dial-LCD row flaked once
under load (encoder pulse timing, passes in isolation).

MEASURED, calm machine, dense.mid 256, best of interleaved runs.
Upstream: 0.700 ms / CPU 1064 ns / SH-2 1026 ns. Branch: 0.675 ms /
CPU 993 ns / SH-2 957 ns. The delta localizes entirely to SH-2
(MEG 661→666, slave 608→608: unchanged), which is exactly the
modified component — a real −7% on SH-2, −3.6% per block, worst case
1.67→1.47 ms. Earlier single runs in both directions were variance
(±6% run-to-run on identical code); interleaving settled it.
Rebased onto upstream past the MEG-stats and pitch-bend commits:
0.674 ms / SH-2 964 ns, same −6% — the gain survives. W23-only
(without plan C): SH-2 953 ns (−7.1%), block average barely moves
(MEG ~670 ns dominates the total) — the delta lives entirely in SH-2.
2. **Bailout reduction: attempted, reverted.** Ported opt/arm64-2's
   slow-checks two ways. Full version (no per-memop checks) audibly
   delays interrupts — `C_test` can be raised from another thread at
   any time (USB MIDI dropped bytes). Narrowed plan C (keep `C_test`,
   move only the pc-compare) was bit-exact but measured inside the
   noise band (0.696 vs 0.675 best — unresolvable ±6%), so reverted:
   extra machinery for no provable gain. Lesson: this machine resolves
   ~5%+, not ~1%.
3. **Wait-loop fast-forward.** The poll loops above, if they ever show
   up mid-song: recognize pure MMIO-test loops, jump `icount` to the
   awaited device event (CMT exposes `m_next_event`; pattern exists in
   `catch_up`). Needs a purity check (no stores/calls in the loop).
4. **MEG side untouched.** DSP numbers (≈700ns/sample/instance) were
   not investigated; NEON vectorization of the mixer is the big lever
   there, i.e. a rewrite, not a tune.
5. **Not retried (opt/arm64-2's own verdicts, folded here).** SR in w27:
   seven A/B runs dead even (~1051 vs ~1056, M1 hides L1 round-trips;
   retry as compare/branch fusion, not caching). Bake-again via
   smull64: master −2.7%, slave +1.5%, effects confirmed the loss;
   retry only as threshold-gated conditional spec. Bounds hoisting
   incl. the four-register RAM-only retry: flat both times (bounds fit
   one movz each; prologue eats the saving).

## Repro

```
SMU2000_PCPROF=15 ./build/blocktime roms build/tests/dense.mid 256 1 1
SH2_JIT_TRACE="250000000,300000,/tmp/jtrace.txt" ./build/blocktime roms build/tests/dense.mid 256 1 1
awk '{print $1}' /tmp/jtrace.txt | cut -d@ -f1 | sort | uniq -c | sort -rn | head
```
