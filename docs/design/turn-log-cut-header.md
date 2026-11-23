# Turn log: telling a damaged sector from a torn one

> Status: DESIGN (2026-11-22), quest journal-tail-cut-header-design, for review (lens: durability).
> The recovery rules it measures are journal main's (journal:a3eecb5), described in
> [turn-log-damage.md](../mechanisms/turn-log-damage.md). Prototype: journal branch
> `scratch/cut-header-proto` at journal:8485149, off journal:e912a28, never to land.

## Abstract

The turn log (`RFTW0001`) writes one turn per write and makes it durable with one data barrier. A
crash leaves each 512-byte sector old or new. When a fragment of the newest turn fails its
checksum, recovery must decide whether a crash left one of its sectors old (a tear: drop the
unacknowledged turn) or the bytes changed after they were written (damage: report it). Today it
decides from content: a sector counts as new only if it holds a whole fragment header of this
segment (`damaged_fragment`, `include/journal/turn_log.h`). Four shapes hold no such header, and in
the newest tail each of them drops an acknowledged turn without a report: 36 of 201 damage cases
in the probe below.

The cut-header shapes the quest asked about are two of the four, 3 of the 36. A layout where no
fragment header straddles a sector removes both for 0.15-0.18% more bytes, and leaves 30 of 189.
Only a check of the sector itself tells a damaged sector from an old one in every shape.

**Recommendation: per-sector checks (O3b).** Each sector carries an 8-byte trailer, its segment and
a CRC-32C over its data, and fragment headers shrink from 24 bytes to 4. Every damaged sector the
scan reaches is then reported, whatever the shape: 0 of 201 silent in the model. `damaged_fragment`,
`damaged_header` and their documented Limits go, F3's group-id argument with them. On the turn
log's workloads it writes 5.6-6.2% fewer bytes per turn for records up to 700 bytes and 1.0% more
for 4 KiB records, with no latency change the barrier lets a benchmark see. Until it lands, accept
the Limit (O0); do not build the header-alignment layout (O1) as a step, since O3b replaces
everything it adds.

## The unproven shapes

A fragment F crosses from sector s into s+1, and a byte in its part of s+1 is damaged. Rule 2 of
turn-log-damage.md proves both sectors only when a whole header of F's write proves s and the next
fragment G verifies in s+1 with its header whole there. The first condition that fails names the
shape:

| Shape | What s+1 lacks | Under the current rules |
|---|---|---|
| U3 | F spans 3 or more sectors: a sector it fills holds nothing else | unproven |
| U4 | F is the last fragment starting in s+1: no G there (the write's last fragment, or padding to the block's end) | unproven |
| U2 | G's header straddles the next boundary, so it proves nothing | unproven |
| U1 | F's own header straddles s and s+1, and no header of its write starts in s before it | unproven |
| proven | | damage reported |

In the newest tail an unproven failure drops the acknowledged turn W as a torn tail.

## Options

**O0, accept the Limit.** Journal main as it is. No cost.

**O1, a fragment header never straddles a sector.** The emitter pads to the sector's end when fewer
than 24 bytes remain there before a fragment header, and every reader skips the same bytes. Closes
U1 and U2, since every header is whole in its sector; leaves U3 and U4. F's header then proves s by
naming the segment and epoch after a writer's type and reserved byte, so F3 bears more weight: a
stale record whose host-chosen group id has byte 6 in 1-4 and byte 7 zero, followed by a sequence
equal to the segment and a payload starting with the epoch, reads as F's header and turns a tear
into reported damage. The probe's forged `f3` case reads as damage at every layout. A header-only
check (epoch cut to 32 bits, a CRC-32C of the header in the other 4) would close F3 at no space
cost; it is analysed, not prototyped.

**O2, a turn's write ends at a sector boundary.** No two writes share a sector. Closes U1 only where
F's write starts in s; costs 28-31% more bytes on T9's workload, because each turn pads half a
sector on average.

**O3, per-sector checks.** Each 512-byte sector is 504 data bytes and a trailer: the segment's low
32 bits and a CRC-32C over the data, those 4 bytes and the sector's index in its segment (not
stored). A sector then reads four ways, decided by the sector alone:

| The sector | Means |
|---|---|
| verifies, names this segment | a version this segment wrote |
| verifies, names another | a stale sector of the file's earlier use |
| all zeros | never written |
| anything else | damage: a crash leaves each sector old or new, and both verify |

A write covers whole sectors: it rewrites its first sector's earlier bytes (the log keeps the last
partial sector in memory) and zero-fills its last sector up to the trailer. A failing fragment needs
no proof: if every sector it spans verifies, one is an older version, so it is a tear; if one fails,
that is damage. **O3a** keeps fragments as they are. **O3b** also drops what the sector check makes
redundant: the fragment's CRC, segment and epoch (one open writes one segment, so the block
header's epoch serves), and the block header's CRC. A fragment header becomes length (2), type (1),
reserved (1).

**O4, a per-sector write stamp.** A trailer naming the last write that touched the sector, under a
check of the stamp alone. It closes the same cases as O3 for one damaged byte at O3a's space, but
keeps every fragment checksum and needs each failing fragment's write identified. O3b closes them
with less code and fewer bytes.

## What each option closes

Probe `test/cut_header_probe.cc`, built once per layout (`-DJOURNAL_LAYOUT=0`, 1, 2). Newest tail:
24 seeds at 2048 and 4096-byte blocks; the log opens three times (a power cut mid-turn, a clean
close, then 0-21 turns and W); a byte is flipped at 3 offsets in each later sector of each W
fragment that crosses a sector, and the log is recovered. Every subset of W's changed sectors left
old (1024 random subsets past 10 sectors) must read as a tear. One run of 48, seed 20 at 4096-byte
blocks, has W open a new segment, which has no old sectors to leave; it is counted and skipped. O3
is a model: a damaged sector equals none of its written versions, so its check fails there. Each
layout places fragments differently, so each counts its own cases.

| Newest-tail damage, by shape | O0 (layout 0) | O1 (layout 1) | O2 (layout 2) | O3 (model, layout 0's cases) |
|---|---|---|---|---|
| U1 own header cut | 3 silent | none occur | none occur | 3 flagged |
| U2 next header cut | none occur | none occur | none occur | none occur |
| U3 fills a sector | 18 silent | 18 silent | 24 silent | 18 flagged |
| U4 last fragment in its sector | 15 silent | 12 silent | 12 silent | 15 flagged |
| proven | 165 reported | 159 reported | 123 reported | 165 flagged |
| **silent of all cases** | **36 of 201** | **30 of 189** | **36 of 159** | **0 of 201** |
| tears read as damage or loss | 0 of 427 | 0 of 417 | 0 of 313 | 0 (a tear leaves each sector a version) |

Jobs: layout 0 PROBE_L0, layout 1 PROBE_L1, layout 2 PROBE_L2 at journal:8485149; layout 0 at
journal main (journal:a3eecb5 plus the probe, journal:b335b69) PROBE_MAIN, which gives layout 0's
counts on the landed code. The durability review's probe counted 36 of 210 on its own seeds.

BLOCKS

Layout 1 places no fragment header across a sector in any block of the probe's 80 clean logs (its
`fr.cut` check). T9, the turn log's power-loss test, run per layout at 40 seeds (crash points,
corrupted blocks, damaged crossing fragments, simulated tears): T9_L1. T9_L2.

## Cost

Bytes per turn, the probe's `layout_bytes`: faultfs, 8 seeds of 400 turns, T9's workload (1-3
records of 1-700 bytes per turn, 684.8 payload bytes per turn).

| Layout | 2 KiB blocks | 4 KiB | 32 KiB | Against O0 |
|---|---|---|---|---|
| O0 | 897.2 | 869.1 | 838.8 | |
| O1 | 898.5 | 870.5 | 840.3 | +0.15 to +0.18% |
| O2 | 1145.9 | 1123.4 | 1096.2 | +27.7 to +30.7% |

O3's bytes, the probe's `sector_check_bytes`, are derived from layout 0's turns (2000 per row):
data times 512/504 for O3a, and for O3b minus 20 bytes per fragment and 4 per block header first.

| Workload, block | Payload | O0 framed | Fragments | O3a | O3b | Whole-sector write covers |
|---|---|---|---|---|---|---|
| T9's, 2 KiB | 687.5 | 900.0 | 3.36 | 914.3 (+1.59%) | 844.3 (-6.19%) | 1409.5 |
| T9's, 32 KiB | 687.5 | 841.5 | 2.99 | 854.8 (+1.59%) | 794.0 (-5.65%) | 1352.2 |
| turn_bench, 16 groups, half active, 256 B | 2046.6 | 2517.4 | 9.06 | 2557.3 (+1.59%) | 2372.9 (-5.74%) | 3027.5 |
| the same, 4 KiB records | 32745.5 | 33556.5 | 10.04 | 34089.1 (+1.59%) | 33880.9 (+0.97%) | 34062.8 |

Write latency: `bench/turn_bench.cc` per layout, interleaved, one `exclusive` job (261121-132716-d4b057,
journal:9b4a5a5), macOS APFS with a full flush per barrier, one core, 16 groups, half active per
turn, 5 s per run, 3 rounds; the median of the 3 rounds. Layout 3 emulates O3's write: whole sectors
from the first sector's start to the last one's end, a CRC-32C per sector, one extra copy of the
turn (which O3b's in-place emitter avoids), no trailers.

| Layout | 256 B: turns/s | p50 | 4 KiB: turns/s | p50 |
|---|---|---|---|---|
| 0 | 48.0 | 20.92 ms | 47.6 | 20.95 ms |
| 1 | 48.0 | 20.91 ms | 47.6 | 20.98 ms |
| 2 | 48.0 | 20.91 ms | 47.8 | 20.95 ms |
| 3 (O3's write) | 48.0 | 20.94 ms | 47.4 | 20.97 ms |

The barrier, 21 ms, dominates each turn, and the rounds spread 0.1-0.2 ms (p50) and 1-2 turns/s, so
the bench cannot see a CPU cost under about 0.1 ms; p99 ranges 22-42 ms in every layout, from the
flush. O3's whole-sector write covers 500-600 bytes more than the turn (table above); both ends lie
in pages the turn already dirties, so the pages written back are the same.

## Decisions

- **D1.** Recovery decides damage per sector (O3b), not from fragment content. Alternatives: O0
  (an acknowledged turn drops silently in four shapes), O1 (closes the two smaller shapes, and D1
  replaces it whole), O2 (28-31% more bytes, and 36 silent cases still),
  O4 (same closure, more bytes and code).
- **D2.** The bytes on disk, `RFTW0002`: a sector is 504 data bytes, the segment's low 32 bits and
  a CRC-32C over data, those bits and the 32-bit sector index. A fragment header is length (2),
  type (1), reserved (1). A block header keeps magic, identity, segment, block, epoch and manifest,
  without its CRC. Blocks are a whole number of sectors: 256-byte and unaligned blocks go, and the
  Limit on unaligned blocks with them. Positions stay logical (data bytes); a file offset is
  `logical / 504 * 512 + logical % 504`.
- **D3.** A write covers whole sectors; the log keeps its last partial sector to rewrite it; the
  zero-filled rest of a sector is where the next write starts, and a zero length is the end of data.
- **D4.** Recovery rules replace rules 1 and 2 of turn-log-damage.md: a sector that fails is damage;
  a fragment whose sectors all verify but which does not parse, or ends in a stale, zero or older
  sector, is the segment's end. Rules 3-5 (what damage costs) are unchanged, so journal-lost-bound's
  bounds apply as they are.
- **D5.** The sector layer is framing's, so strata's manifest log (`framing::scan`) takes it too and
  its tail rule reduces to the sector verdicts. One shape for both streams.

## Interactions

- **journal-turn-buffers** (landed). It frames each record in place, in pieces, into the turn's
  buffer, and reads forward copies back through `framing::RecordReader`. O3b keeps both: the
  emitter's append splits each piece at a sector's 504th byte and leaves the trailer's 8 bytes,
  sealing each sector's CRC when the turn is complete, so a body is still copied once;
  `Emitter::measure` works in logical positions unchanged; `RecordReader` reads whole sectors and
  verifies each before handing its data on. Its `bytes` test compares against a reference emitter
  byte for byte, so its reference changes with the format.
- **journal-lost-bound** (landed). It consumes damage reports and block manifests (rules 3-5) and
  never inspects how damage was found, so no option conflicts with it. Under O3 the shapes that now
  drop the newest turn silently are reported, so their groups are bounded or lost loudly instead.
- **The turn bound** (`turn_bound`, `turn_room`, docs/mechanisms/turn-seal.md) must scale by 512/504
  and drop the 20 fragment bytes under O3b; under O1 each fragment costs up to 23 more bytes.

## Limits

- A tear inside a sector (pmem without BTT) stays outside the model; under O3 it reads as damage.
- Under O3, a rotted sector past the newest write's end that a torn write's later sectors make the
  scan read is reported as damage, and costs the torn turn's groups under rule 5. That needs real
  damage on the device in an unwritten sector.
- A CRC-32C collision lets a damaged sector verify: 2^-32 per damaged sector, as it is per fragment
  today.
- Until O3 lands (O0), damage in U1-U4 of the newest turn drops it without a report. **This is
  silent.**

## Status and next steps

Built only as the scratch prototype: layouts 0-2 run under the probe and T9 at journal:e912a28's
code, whose rules journal main keeps unchanged (`damaged_header`, `damaged_fragment` and the
`in_write` tracking are identical at journal:a3eecb5); layout 3 is a write-cost emulation without
trailers or a reader. Nothing of O3 is built. First steps:

1. framing: the sector layer (D2, D3) in the emitter, `RecordReader`, `scan`; fixtures for
   `RFTW0002`; the framing fuzzer.
2. turn log: D4's rules in `scan_segment` and `recover`; T9 rewritten around sector verdicts, every
   shape of the probe asserted reported.
3. strata's manifest log on the new framing (D5).

Questions for the owner:

1. **Q1.** Take O3b now as a format quest (recommended), or accept the Limit for good? O1 is the
   third choice, only if a format rewrite is off the table.
2. **Q2.** D5: both streams at once (recommended), or the turn log first?

From review:

1. Whether D4's four sector verdicts are complete under the crash model.
2. Whether the trailer should carry all 64 bits of the segment (12 bytes) rather than 32.
