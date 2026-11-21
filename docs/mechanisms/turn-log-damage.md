# Turn log damage

How `TurnLog` recovery tells damage from a crash, so a damaged log reports its damage instead of
dropping acknowledged records as a torn tail. The code is `scan_segment`, `damaged_header`,
`damaged_fragment` and `TurnLog::recover` in `include/journal/turn_log.h`.

## The crash model

A crash leaves each 512-byte sector old or new. Every open writes a fresh segment number, and
segment numbers are never reused, so no stale sector holds bytes bound to this segment. The log
writes each segment once, in order, so an old sector holds nothing of the segment past the last
durable turn. A sector that holds a fragment of this segment that verifies therefore holds as
written everything the log wrote in it up to that fragment, and whatever there fails to verify is
damage. A crash cannot leave that.

A header that names this segment and epoch proves its fragment's first sector the same way: the
sector is new, or the fragment was durable. A fragment is as written in every sector proven this
way. A sector the fragment fills from start to end holds nothing else that could prove it, so
that sector may be old.

## The rules

1. **A header that fails** is damage when a fragment of this segment verifies in the header's sector.
2. **A fragment that fails** is damage in any of these cases:
   - A later fragment of this segment verifies in the same sector.
   - Its header names this segment and epoch, and any of the following holds:
     - It has a header no writer emits: zero length, a bad type, or a nonzero reserved byte.
     - It runs past its block.
     - Its sector holds it whole.
     - Its checksum verifies at a length its sector holds whole. A damaged length makes such a
       fragment look as if it crossed into the next sector.
     - A fragment of this segment verifies in the next sector. Both sectors are then proven, so a
       fragment that ends in the next sector is as written there. One that runs on past it fills
       that sector, so no fragment starts there. This holds whatever length the header reads.

   Otherwise, a fragment that crosses its sector reads as a tear, because a sector after its first
   may be old. Such a fragment fills the next sector, or it is the last fragment written there.
3. **A gap that holds damage** costs the groups that its first block's manifest names. That manifest
   is in the next block's header. When no manifest reads back, the gap costs every group, and the
   damage is reported.
4. **A segment's tail** is a gap at the segment's end, and an unwritten tail costs nothing. A tail
   that holds damage is a gap like any other. Damage in a segment's last block and in the next
   header merges into a single gap, and that gap is the segment's tail.
5. **Damage past the newest epoch's last kept turn** costs every group with a record in the turns
   dropped there.

## Limits

- A block whose size is not a multiple of 512 can start mid-sector. The rules then cannot see many
  of its headers.
- A damaged payload byte in a fragment that crosses its sector, in the last turn, reads as a torn
  tail when no fragment of the segment verifies in the next sector. In that case the fragment
  fills the next sector, or it is the last fragment written there. Only a per-sector checksum
  could tell this damage from a tear.
- A tear inside a sector, as on pmem without BTT, is outside the model: it reads as damage.

## Evidence

`test/turn_log_power_test.cc` runs crash points and damage patterns over six block sizes: 512,
4096, 2048, 1000, 768 and 256 bytes. It takes `SEEDS FIRST_SEED BLOCK_BYTES`, so each seed below
replays alone. `test/turn_log_power_vm.sh` runs the same test in a local Linux or FreeBSD VM. At 200
seeds the test makes 1,036,264 checks with 0 failures. Its 241,462 crash points report no damage.

Each seed below pins one rule:

| Case | Damage | Rule |
|---|---|---|
| seeds 91 and 140, block_bytes 512, 'both' | The oldest segment's last two blocks, before the first kept turn: a byte past block 1's header and one in block 2's header | 4: unreported otherwise, and a group holding records only there would vanish |
| seed 122, block_bytes 256, 'both' | Segment 42's last blocks, before the incomplete acknowledged turn 57 | 4: otherwise an earlier segment's empty tail takes the charge, and group 6 is kept with 2 of 3 records |
| seeds 145 and 199, block_bytes 2048, 'last' | A flipped low length byte in the last turn's fragment, length 29 read as 71, which crosses its sector | 2, the checksum at a length the sector holds whole: otherwise the acknowledged last turn drops as a torn tail |
