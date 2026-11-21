# Turn log: telling a damaged sector from a torn one

> Status: DESIGN (2026-11-21), quest journal-tail-cut-header-design, for review (lens: design,
> durability). Builds on journal-tail-second-sector (journal:e912a28), whose rules are in
> [turn-log-damage.md](../mechanisms/turn-log-damage.md) on that branch. Prototype: journal branch
> `scratch/cut-header-proto` at journal:e0e9c37, never to land.

## Abstract

The turn log (`RFTW0001`) writes one turn per write and makes it durable with one data barrier. A
crash leaves each 512-byte sector old or new. When a fragment in the newest turn fails its
checksum, recovery must decide whether a crash left one of its sectors old (a tear: drop the
unacknowledged turn) or the bytes changed after they were written (damage: report it). Today it
decides from content: a sector counts as new only if it holds a whole fragment header of this
segment (rules in `damaged_fragment`, `include/journal/turn_log.h:177`). Four shapes hold no such
header, and in the newest tail each of them drops an acknowledged turn without a report.

The cut-header shapes this quest was asked about are two of the four. A layout where no fragment
header straddles a sector removes them both, measured, for 0.15-0.2% more bytes, and leaves the
other two, which are the larger share (table below). Only a check of the sector itself tells a
damaged sector from an old one in every shape.

**Recommendation: per-sector checks (O3b).** Each sector carries an 8-byte trailer, its segment and
a CRC-32C over its data, and fragments shrink from 24 header bytes to 4. Every damaged byte the scan
reaches is then reported, whatever the shape. `damaged_fragment`, `damaged_header` and their three
documented Limits go, F3's group-id argument with them. On the turn log's workloads it writes 5.6 to
6.2% fewer bytes per turn for small records and 1.0% more for 4 KiB records, with no measured
latency change. Until it lands, accept the Limit (O0); do not build the header-alignment layout (O1)
as a step, since O3b replaces everything it adds.

## The unproven shapes

A fragment F crosses from sector s into s+1, and a byte in its part of s+1 is damaged. Rule 2 of
turn-log-damage.md proves both sectors only when a whole header of F's write proves s and the next
fragment G verifies in s+1 with its header whole there. The first condition that fails names the
shape:

| Shape | What s+1 lacks | Under the current rules |
|---|---|---|
| U3 | F spans 3 or more sectors: a sector it fills holds nothing else | unproven |
| U4 | F is the last fragment starting in s+1: no G there (W's own last fragment, padding to the block's end) | unproven |
| U2 | G's header straddles the next boundary, so it proves nothing (F1's fix) | unproven |
| U1 | F's own header straddles s and s+1, and no header of its write starts in s before it | unproven |
| proven | | damage reported |

Outside the newest tail an unproven failure still costs nothing silently, because the turns after
it prove the gap was written (probe `blocks` rows). In the newest tail it drops the acknowledged
turn W as a torn tail.

## Options

**O0, accept the Limit.** As journal:e912a28. No cost.

**O1, a fragment header never straddles a sector.** The emitter pads to the sector's end when fewer
than 24 bytes remain there before a fragment header, and every reader skips the same bytes, as
padding before a block's end already works. Closes U1 and U2, since every header is whole in its
sector. Leaves U3 and U4. F's header now proves s by naming the segment and epoch after a writer's
type and reserved byte (`named`, `turn_log.h:186`), so F3 holds with more weight on it: a stale
record whose host-chosen group id has byte 6 in 1-4 and byte 7 zero, followed by a sequence equal to
the segment and a payload starting with the epoch, reads as F's header and turns a tear into damage.
The probe's `f3` case reads as damage at every layout. A header-only check (epoch cut to 32 bits, a
CRC-32C of the header in the other 4) would close F3 at no space cost; it is not prototyped.

**O2, a turn's write ends at a sector boundary.** No two writes share a sector. Closes U1 only where
F's write starts in s, and the probe shows the rest unchanged; costs 28-31% more bytes on T9's
workload (685 payload bytes per turn) because each turn pads half a sector on average.

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
partial sector in memory) and zero-fills its last sector to the trailer. A failing fragment no
longer needs a proof: if every sector it spans verifies, one of them is an older version, so it is
a tear; if one fails, that is damage. **O3a** keeps fragments as they are. **O3b** also drops what
the sector check makes redundant: the fragment's CRC, segment and epoch (one open writes one
segment, so the block header's epoch serves), and the block header's CRC. A fragment header becomes
length (2), type (1), reserved (1).

**O4, a per-sector write stamp.** A trailer naming the last write that touched the sector (its start
offset) under a check of the stamp alone, not the data. Closes every shape for one damaged byte, as
O3 does, at O3a's space, but keeps every fragment checksum and needs each failing fragment's write
identified. O3b closes the same cases with less code and fewer bytes; O4 loses.

## What each option closes

Probe `test/cut_header_probe.cc`, built once per layout (`-DJOURNAL_LAYOUT=0`, 1, 2). `newest`:
24 seeds at 2048 and 4096-byte blocks; the log opens three times (a power cut mid-turn, a clean
close, then 0-21 turns and W), a byte is flipped at 3 offsets in each later sector of each W
fragment crossing a sector, and the log is recovered; every subset of W's changed sectors left old
(1024 random ones past 10 sectors) must read as a tear. O3 is a model column: the damaged sector
equals none of its written versions, so its check fails.

TABLE

## Cost

Bytes per turn: the probe's `layout_bytes` and `sector_check_bytes`, faultfs, 2000-3200 turns.
O3's bytes are derived from layout 0's turns: data times 512/504, and for O3b minus 20 bytes per
fragment and 4 per block header.

BYTES

Write latency: `bench/turn_bench.cc` per layout, interleaved, one `exclusive` job (JOB), macOS
APFS, one core, 16 groups, half active per turn, 5 s per run, 3 rounds. Layout 3 emulates O3's
write: whole sectors from the first sector's start to the last one's end, a CRC-32C per sector, one
extra copy of the turn (which O3b's in-place emitter avoids), no trailers.

LATENCY

The barrier dominates each turn, so no layout moves latency outside the rounds' spread. O3's
whole-sector write rewrites up to 504 bytes before the turn and zero-fills up to 504 after it; both
lie in pages the turn already dirties, so the pages written back are the same.

## Decisions

- **D1.** Recovery decides damage per sector (O3b), not from fragment content. Alternatives: O0
  (lost: an acknowledged turn drops silently in 4 shapes), O1 (lost: closes the two smaller shapes
  and is replaced whole by D1), O2 (lost: costs 28-31% bytes and closes little), O4 (lost: same
  closure, more bytes and code).
- **D2.** The bytes on disk, `RFTW0002`: a sector is 504 data bytes, the segment's low 32 bits and
  a CRC-32C over data, those bits and the 32-bit sector index. A fragment header is length (2),
  type (1), reserved (1). A block header keeps magic, identity, segment, block, epoch and manifest,
  without its CRC. Blocks are a whole number of sectors: 256-byte and unaligned blocks go (the
  Limit on unaligned blocks with them). Positions stay logical (data bytes); a file offset is
  `logical / 504 * 512 + logical % 504`.
- **D3.** A write covers whole sectors; the log keeps its last partial sector to rewrite it; the
  zero-filled rest of a sector is where the next write starts, and a zero length is the end of data.
- **D4.** Recovery rules replace rules 1 and 2 of turn-log-damage.md: a sector that fails is damage;
  a fragment whose sectors all verify but which does not parse, or ends in a stale, zero or older
  sector, is the segment's end. Rules 3-5 (what damage costs) are unchanged, so lost-bound's
  bounds apply as they are.
- **D5.** The sector layer is framing's, so strata's manifest log (`framing::scan`) takes it too and
  its tail rule reduces to the sector verdicts. One shape for both streams.

## Interactions

- **journal-turn-buffers** (xapiand's, landing first). It frames each record in place, in pieces,
  into the turn's buffer, and reads forward copies back through `framing::RecordReader`. O3b keeps
  both: the emitter's append splits each piece at a sector's 504th byte and leaves the trailer's 8
  bytes, sealing each sector's CRC when the turn is complete, so a body is still copied once;
  `Emitter::measure` works in logical positions unchanged; `RecordReader` reads whole sectors and
  verifies each before handing its data on, through the same stack buffer. Its `bytes` test compares
  against main's emitter byte for byte, so its reference changes with the format. O1 would touch the
  same three places with a padding rule instead.
- **journal-lost-bound** (xapiand's). It consumes damage reports and block manifests (rules 3-5) and
  never inspects how damage was found, so no option conflicts with it. Under O3 the shapes that now
  drop the newest turn silently are reported, so their groups are bounded or lost loudly instead.
- **The turn bound** (`turn_bound`, `turn_room`, docs/mechanisms/turn-seal.md) must scale by 512/504
  and drop the 20 fragment bytes under O3b; under O1 each fragment costs up to 23 more bytes.

## Limits

- A tear inside a sector (pmem without BTT) stays outside the model; under O3 it reads as damage.
- Under O3, a rotted sector past the newest write's end that a torn write's later sectors make the
  scan read is reported as damage, and costs the torn turn's groups under rule 5. That needs real
  damage on the device in an unwritten sector.
- A CRC-32C collision lets a damaged sector verify: 2^-32 per damaged sector. Today the same holds per
  fragment.
- Until O3 lands (O0), damage in U1-U4 of the newest turn drops it without a report. **This is
  silent.**

## Status and next steps

Built only as the scratch prototype: layouts 0-2 run under the probe and T9; layout 3 is a write
cost emulation without trailers or a reader. Nothing of O3 is built. First steps, after
journal-turn-buffers and journal-tail-second-sector land:

1. framing: the sector layer (D2, D3) in the emitter, `RecordReader`, `scan`; fixtures for
   `RFTW0002`; the framing fuzzer.
2. turn log: D4's rules in `scan_segment` and `recover`; T9 rewritten around sector verdicts, every
   shape of the probe asserted reported.
3. strata's manifest log on the new framing (D5).

Questions for the owner:

- **Q1.** Take O3b now as a format quest (recommended), or accept the Limit for good? O1 is the
  third choice only if a format rewrite is off the table.
- **Q2.** D5: both streams at once (recommended), or the turn log first?

From review: whether D4's four sector verdicts are complete under the crash model, and whether the
trailer should carry all 64 bits of the segment (12 bytes) rather than 32.
