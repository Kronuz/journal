# Turn log damage

How `TurnLog` recovery tells damage from a crash, so a damaged log reports its damage instead of
dropping acknowledged records as a torn tail. The format is `RFTW0002`, the cut-header design's O3b
([turn-log-cut-header.md](../design/turn-log-cut-header.md), D2-D4). The code is `verdict`,
`scan_segment` and `TurnLog::recover` in `include/journal/turn_log.h`.

## The crash model

A crash leaves each 512-byte sector old or new. Every open writes a fresh segment number, and
segment numbers are never reused. The log writes each segment once, in order, one write per turn
and one for the open record, and a write starts only after every earlier write's barrier returned.
Recycled segment files are not zeroed, so a sector a write did not reach holds what the file held
before: zeros in a new file, or a sector of the file's earlier segment.

## The format

Every sector is 504 data bytes, then the segment's low 32 bits, then a CRC-32C over the data, all
64 bits of the segment and the sector's 32-bit index in the segment. Positions are data bytes: a
file offset is `position / 504 * 512 + position % 504`. A block is a whole number of sectors and
opens with a header (magic, identity, segment, block, epoch, and the manifest of the block before
it). Records split into fragments of a length (2 bytes), a type and a reserved byte, which never
cross a block. A zero length ends the segment's data.

A write covers whole sectors: from the one holding the log's position, which it rewrites with the
data already there, through the one holding its end, zero-filled past it. The log keeps the data of
its last partial sector for the next write. While a turn is in flight, a read of an acknowledged
record takes that sector's bytes from this copy, not the file: the file's may be part way through
the write (`Host::park` mid-turn, a Store's `live()`), and its check fails until the write ends.

## The verdicts

Each sector decides alone:

| The sector | Verdict |
|---|---|
| verifies under this segment | current: a version this segment wrote |
| verifies under the nearest lower segment with its stored low bits (2^32 below when they are this segment's) | stale: the recycled file's earlier use |
| all zeros | never written |
| anything else, or a sector the device cannot read | damage |

A crash leaves each sector a version that verifies, so a crash never leaves damage. A sector that
the torn newest write did not reach holds its old version: the partial sector's earlier data, which
ends in a zero length, zeros, or a stale sector.

## The rules

1. **A sector that fails, or that the device cannot read,** is damage wherever the scan needs it:
   a block header, a fragment header or its payload. The scan reads a block whole, and a block that
   does not read whole a sector at a time, so only the sectors the device cannot read are damage.
2. **A stale or never-written sector, or a fragment that does not parse,** a zero length among
   them, is the segment's end there. A later turn read from a later block of the segment proves the
   end's write durable, which makes the end damage (rule 6).
3. **A gap that holds damage** costs the groups named by the manifest of every block from its first
   through the last block where a lost record may begin. Each block's manifest is in the next
   block's header. When one of those manifests does not read back, the gap costs every group, and
   the damage is reported.
4. **A segment's tail** is a gap at the segment's end, and an unwritten tail costs nothing. A tail
   that holds damage is a gap like any other.
5. **Damage past an epoch's last kept turn** costs every group with a record in the turns dropped
   there. In an epoch before the newest, that is damage past its last kept turn, such as the next
   open's record when it did not read back.
6. **A failure followed by a later write** is damage. A write starts only after every earlier
   write's barrier returned, so a record of a turn later than the end's proves the end's write
   durable. Past the newest epoch's last kept turn, a gap is damage when a later record names a
   later turn than the gap's end belongs to: at least the first turn not kept, and at least each
   record's before the gap (the next turn, past a turn's end).

## Cost

Bytes the log grows per turn against `RFTW0001` (`journal_turn_log_test cost`, 8 seeds of 400
turns):

| Workload | Block | Payload | `RFTW0001` | `RFTW0002` | Change |
|---|---|---|---|---|---|
| T9's (1-3 records up to 700 B) | 2 KiB | 684.8 | 897.3 | 838.2 | -6.59% |
| T9's | 4 KiB | 684.8 | 869.2 | 816.1 | -6.10% |
| T9's | 32 KiB | 684.8 | 838.8 | 790.8 | -5.72% |
| turn_bench, 16 groups, half active, 256 B | 32 KiB | 2042.7 | 2512.4 | 2366.7 | -5.80% |
| the same, 4 KiB records | 32 KiB | 32683.5 | 33350.7 | 33673.8 | +0.97% |

Shorter fragment headers pay for the trailers below about 700-byte records.

Each write covers whole sectors, so it hands the device the partial sector before it again and
zeros after it, about 510 bytes more than the log grows. `RFTW0001` hands the write only the
turn's bytes, but a device writes the sectors they touch and the page cache the pages, so the
same partial sectors go out under both formats. On those terms, from the same runs:

| Workload | Block | 512 B sectors touched | Change | 4 KiB pages touched | Change |
|---|---|---|---|---|---|
| T9's | 2 KiB | 1408.0 -> 1348.6 | -4.22% | 4985.6 -> 4930.6 | -1.10% |
| T9's | 4 KiB | 1380.0 -> 1325.6 | -3.94% | 4960.0 -> 4905.0 | -1.11% |
| T9's | 32 KiB | 1349.6 -> 1300.5 | -3.64% | 4930.6 -> 4879.4 | -1.04% |
| turn_bench, 256 B | 32 KiB | 3022.9 -> 2875.7 | -4.87% | 6603.5 -> 6451.2 | -2.31% |
| turn_bench, 4 KiB | 32 KiB | 33861.3 -> 34184.6 | +0.95% | 37442.6 -> 37762.6 | +0.85% |

An `RFTW0002` write is exactly the sectors it touches, with no sector past the turn's last, and a
turn starts in the sector the one before it ends in. `turn_bench`'s "written/appended" counts the
bytes handed to the write: 1.41 against 1.23 at 256 B and 1.047 against 1.020 at 4 KiB is the
whole-sector write against an unaligned one, not more sectors on the device.

## Limits

- A sector more than 2^32 segments older than the file's segment reads as damage, not as stale:
  the verdict tries only the nearest lower segment with the stored low bits (review F6). The log
  moves one segment per open or per filled segment, so a recycled file holds such a sector only
  after 2^32 of them.
- An all-zero sector is never written, not damage, and this is not "verifies under neither"
  (review F7). An acknowledged sector that reads back as zeros, or as an older version (a lost or
  misdirected write), is outside the crash model: it reads as the segment's end, and the turns
  after it in its block drop silently unless a later block of the segment holds a later turn
  (rule 6).
- The stored low bits only name the segment a stale sector is checked under, and the CRC does not
  cover them: damage to them in a current sector costs nothing and is not reported.
- A CRC-32C collision lets a damaged sector verify: 2^-32 per damaged sector.
- A tear inside a sector, as on pmem without BTT, is outside the model: it reads as damage. A tear
  inside a turn's first sector, which it shares with the turn before, fails that sector's check,
  and the sector lies in the newest block, whose manifest is not written yet: every group is lost,
  the turn before's included (review F3). Keeping the turn before's bytes out of the sector means
  ending each write at a sector boundary, 28-31% more bytes per turn (the cut-header design's O2).
- Damage in the first sector of a block no write reached, in the newest segment's unwritten blocks
  or a spare's, is damage in a gap no manifest names: every group is lost (review F2), one sector
  in 64 of that space at 32 KiB blocks. `RFTW0001` read a flipped header there as the end and an
  unreadable one as an error from recovery. Reading such damage as the end instead changes rule 1.
- A fragment never verifies on its own, so bytes inside a torn write's payload that once formed a
  valid fragment cannot forge a record: every sector the parse reads verifies under this segment,
  or the parse stops. Damage reads as damage, and a gap whose blocks have no manifest yet, the
  newest block's, reports every group lost (journal-earlier-turn-loss's review F1).

## Evidence

`test/turn_log_test.cc`, one case per name (`journal_turn_log_test [case]`):

| Case | What it pins |
|---|---|
| `sector_verdicts` | Each verdict by value on a newest turn W spanning four sectors. W read whole is current. W's rewritten first sector left old, a later sector left unwritten, its last sector all zeros, or a recycled file's stale sector (segment 1's, same index) where W's second belongs: each reads as a tear holding the acknowledged records. A byte flipped in W's first, second or last sector is reported, and W's groups are lost |
| `stale_alias` | A recycled file of segment S = 2^32 + 5 holds, where W's second sector belongs, the sector segment 5 wrote there with the same data and S's low 32 bits: it reads as stale and W as torn. A CRC that binds only 32 bits reads it as current |
| `newest_turn_damage` | The cut-header probe's newest tail, at 24 seeds and 2048- and 4096-byte blocks: a byte flipped at three places in every sector W changed is reported or harmless, never silent, and every subset of W's sectors left old reads as a tear. Under `RFTW0001` damage to W reads as a tear |
| `earlier_turn_damage` | A byte in each sector before the newest write of the newest segment (seeds 4 and 6 at 4096-byte blocks, 22 at 2048) is reported, and each group kept holds every acknowledged record |
| `midwrite_reads` | A turn's write half copied into the sector it shares with the turn before (2048- and 32768-byte blocks, 4 seeds of 100 turns): every group's `peek` returns its acknowledged records, and again after the barrier. Reading that sector from the file throws `Corruption` |
| `large_header_damage`, `large_header_tear` | A block header past its first sector: damage to its second sector is reported, and that sector left old by the torn newest turn is a tear |

`test/turn_log_power_test.cc` (T9) runs crash points at 512-, 4096-, 2048- and 1024-byte blocks,
damage in every written sector, at block boundaries and in the newest block, every tear of each
seed's newest write, and `unreadable`: a sector the device cannot read in an acknowledged block is
damage. `turn_log_power_test probe [SEEDS [BLOCK_SEEDS]]` runs the cut-header probe's sweeps on
this format.
