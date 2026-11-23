# Turn log damage

How `TurnLog` recovery tells damage from a crash, so a damaged log reports its damage instead of
dropping acknowledged records as a torn tail. The code is `scan_segment`, `damaged_header`,
`damaged_fragment` and `TurnLog::recover` in `include/journal/turn_log.h`.

## The crash model

A crash leaves each 512-byte sector old or new. Every open writes a fresh segment number, and
segment numbers are never reused, so no stale sector holds bytes bound to this segment. The log
writes each segment once, in order, one write per turn and one for the open record, so an old
sector holds nothing of a write that was not durable. A sector is therefore proven, for the bytes
of a fragment in it, when it holds any of these:

- A later fragment of this segment that verifies, its header whole in the sector. The sector
  holds as written everything the log wrote in it before that fragment.
- The fragment's own header, naming this segment and epoch after a type and a reserved byte that a
  writer emits. The sector is new, or the fragment was durable. The two numbers alone prove
  nothing: an old sector can hold them as data, as a stale record's group and sequence, two
  adjacent 64-bit integers. The eight bytes before those hold the record's turn, a small integer
  whose high bytes, read as the type and the reserved byte, are zero.
- A header of the fragment's own write that starts in the sector before it and verifies: the
  block's header, or the fragment before it, when that one does not end its write. Only the last
  fragment of a turn's end-of-turn record, or of the open record, ends a write. That header names
  this segment there, so the sector is new or the write was durable.

Bytes that verify prove a sector only through a header the sector holds whole. A payload byte
proves nothing, since the old sector may hold the same byte: zeros, or a stale record's payload.
Nor does a header the sector boundary cuts: as few as one checksum byte of it may lie in the
sector, and an old byte equals it one time in 256.
A fragment is as written in every sector proven this way, so if it fails there, that is damage,
which a crash cannot leave. A sector the fragment fills from start to end holds nothing else that
could prove it, so that sector may be old.

## The rules

1. **A header that fails** is damage when a fragment of this segment verifies in the header's
   sector, its header whole there.
2. **A fragment that fails** is damage in any of these cases:
   - A later fragment of this segment verifies in the same sector, its header whole there.
   - Its header names this segment and epoch after a writer's type and reserved byte, and either
     its length is zero or runs past its block, or its sector holds it whole.
   - Its header names this segment and epoch, and its checksum verifies with one field corrected:
     - A length its sector holds whole. A damaged length makes such a fragment look as if it
       crossed into the next sector.
     - Its type, or its reserved byte, when its length fits its block.
   - Its first sector is proven, by its header or by a header of its write before it, and a fragment
     of this segment verifies in the next sector with its header whole there, which proves that
     sector too. The fragment ends there, since one that ran on past it would fill it and leave no
     room for another to start. With a header of its write before it, this holds whatever its own
     header reads, so it covers a header that the sector boundary cuts.

   Otherwise, a fragment that crosses its sector reads as a tear, because a sector it spans may be
   old. Such a fragment fills the next sector, is the last fragment written there, is followed
   there by a fragment whose header the next boundary cuts, or has a header that the sector
   boundary cuts and no header of its write before it in its sector.
3. **A gap that holds damage** costs the groups named by the manifest of every block from its first
   through the last block where a lost record may begin. Each block's manifest is in the next
   block's header. When one of those manifests does not read back, the gap costs every group, and
   the damage is reported.
4. **A segment's tail** is a gap at the segment's end, and an unwritten tail costs nothing. A tail
   that holds damage is a gap like any other. Damage in a segment's last block and in the next
   header merges into a single gap, and that gap is the segment's tail.
5. **Damage past the newest epoch's last kept turn** costs every group with a record in the turns
   dropped there.

## Limits

- A block whose size is not a multiple of 512 can start mid-sector. The rules then cannot see many
  of its headers.
- A damaged payload byte in a fragment that crosses its sector, in the last turn, reads as a torn
  tail when rule 2 cannot prove the sectors it spans: it fills the next sector, it is the last
  fragment written there, the fragment after it there has a header that the next boundary cuts,
  or it has a header that the sector boundary cuts and no header of its write before it in its
  sector.
  Only a per-sector checksum could tell this damage from a tear.
- A tear inside a sector, as on pmem without BTT, is outside the model: it reads as damage.
- Old bytes that hold this segment and epoch after a type byte of 1 to 4 and a zero byte, as a
  stale payload could, read as the fragment's header, so a tear there reads as damage. The log's
  own old headers name older segments, and the turn or sequence its records and manifests hold
  before such a pair stays far below the 2^48 that type byte needs.

## Evidence

`test/turn_log_power_test.cc` runs crash points and damage patterns over six block sizes: 512,
4096, 2048, 1000, 768 and 256 bytes. It takes `SEEDS FIRST_SEED BLOCK_BYTES`, so each seed below
replays alone. `test/turn_log_power_vm.sh` runs the same test in a local Linux or FreeBSD VM. At 200
seeds the test makes 1,228,757 checks with 0 failures, on macOS and in the FreeBSD VM. Its 242,242
crash points and 155,754 simulated tears report no damage, and its 18,365 damaged crossing
fragments at 2048 and 4096 bytes report damage; 676 more, whose header the sector boundary cuts,
are only counted.

Each seed below pins one rule:

| Case | Damage | Rule |
|---|---|---|
| seeds 91 and 140, block_bytes 512, 'both' | The oldest segment's last two blocks, before the first kept turn: a byte past block 1's header and one in block 2's header | 4: unreported otherwise, and a group holding records only there would vanish |
| seed 122, block_bytes 256, 'both' | Segment 42's last blocks, before the incomplete acknowledged turn 57 | 4: otherwise an earlier segment's empty tail takes the charge, and group 6 is kept with 2 of 3 records |
| seeds 145 and 199, block_bytes 2048, 'last' | A flipped low length byte in the last turn's fragment, length 29 read as 71, which crosses its sector | 2, the checksum at a length the sector holds whole: otherwise the acknowledged last turn drops as a torn tail |
| seeds 29 and 136 at block_bytes 2048, 189 at 4096, simulated tears | A tear leaves a fragment's first sector old, holding another segment's record where the fragment starts: its turn, group 2 and sequence 1 read as length 0, type 0, segment 2 and epoch 1 | 2, a writer's type and reserved byte before the segment and epoch: otherwise the tear reads as damage. `stale_record_tears` replays the three |
| `cut_header_tears`, block_bytes 2048 | A tear leaves a sector old, holding the first 1 to 23 bytes of the fragment that its end boundary cuts as written: the next sector of a crossing fragment whose header names its segment and epoch, a fragment's own sector, and a block header's | 1 and 2, a header the sector holds whole: otherwise each reads as damage, one stale byte in 256 at one byte |
