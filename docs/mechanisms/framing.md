> Status: CURRENT (2026-11-04) how `include/journal/framing.h` works, on branch
> `phase/manifest-log-steps12`; design: strata `docs/design/manifest-log.md` revision 1 (D2, D6),
> strata:1c8ff50. Maintainer: the manifest-log workstream (strata `docs/status/manifest-log-steps12.md`).

# Block framing and the growing-log tail rule

One framing serves both durable logs: the turn log (`RFTW0001`, 32 KiB blocks, block manifests,
preallocated segments) and strata's manifest log (`STRLOG01`, 512-byte blocks, no manifests, a file
that grows by appends).

## Bytes

- A **stream** (`framing::Stream`) names what every byte is bound to: an 8-byte magic, a 16-byte
  identity, the block size, the segment number, the epoch, and whether headers carry manifests.
- A **block** opens with a header: magic, identity, segment (8), block number (4), epoch (8), the
  manifest's segment (8), block (4) and count (2), 16 bytes per manifest entry, CRC-32C (4). Without a
  manifest the count is `0xffff` and the header is 62 bytes.
- A **record** is opaque bytes split into fragments: CRC-32C (4), length (2), type (1: full, 2:
  first, 3: middle, 4: last), a zero byte, segment (8), epoch (8), then the payload; 24 bytes of
  header. The CRC covers everything after itself. A fragment never crosses a block; a block's tail
  shorter than 25 bytes is zero padding that no reader reads.
- `Emitter` writes records from a stream offset, entering blocks as it reaches them. With manifests
  it notes each nonzero group's highest sequence for the next block's header (the turn log's use).
  The turn log wraps its own 25-byte record header (kind, turn, group, sequence) inside the record
  (`turn_log_detail::logical`); the framing never looks inside a record.
- `record_end(stream, position, size)` says where the emitter would end a record, in constant time,
  for a stream without manifests (a manifest's size depends on the groups written): the header and
  padding of the block it starts in, then whole blocks of `block - 62 - 24` payload bytes, then the
  last fragment. strata's writer sizes a snapshot with it (D7's S); `framing_test` checks it against
  the emitter at every record start in a block.

## Reading

- `parse_header` and `parse_fragment` check one header or fragment and answer ok, bad (bytes there
  that fail a check) or cut (the end of file comes first). A fragment's structure (length within
  its block, type, zero byte, segment, epoch) is checked before its extent against the end of file,
  so a length that leaves its block is bad, not cut.
- The turn log's `scan_segment` reads whole preallocated blocks with them and keeps its own gap and
  epoch logic. `read_record` reads one record back by offset (the turn log's forwarding).
- `read_file` reads a growing file whole, its partial last block included. `scan` reads it from the
  start and returns the whole records, each with its first fragment's offset and its end, and how
  the log ends.

## The tail rule (`scan`)

At the first header or fragment that does not verify, the verdict comes from that point alone:

| Found | Verdict |
|---|---|
| Nothing: the last record ends at the end of file | clean |
| A record whose fragments verify but has no last one, the file ending at a 512-byte multiple | torn |
| A header or fragment cut by the end of file | damage |
| A header or fragment whose bytes are zeros to its block's end (or the end of file) | torn |
| Anything else | damage |

A torn verdict becomes damage when a whole record starts in a later block (`whole_record_after`,
which resynchronizes at block starts): one record is in flight at a time, so an unwritten piece
followed by a whole record is not a crash's work. The rule reads the failing piece only, never later
pieces, so a later never-written piece cannot excuse earlier damage.

Why it is exact at 512-byte blocks: every block is one 512-byte piece starting with its header, a
crash leaves each piece old or new, and an appended piece the crash never wrote reads as zeros from
where the append began in it, while a header or fragment header is never all zeros. One write per
append leaves the file's size at the old end, the new end, or a page multiple, all 512-byte
multiples at a block start, so a cut is never a crash's work. Sizes come from the writer (strata's
D4); the framing assumes them. At larger blocks the rule is the same but no longer exact: a zero run
in a payload fills whole sectors.

What the caller adds: the manifest log reads damage when its first record (the snapshot) is not
whole, and drops a torn tail.

## Evidence

- `test/framing_test.cc` replays strata's tail model (`bench/manifest-log/tail_model.py`) over its
  four logs, exported to `test/fixtures/framing/` by `export.py`: the emitter rebuilds each log byte
  for byte; 1,020 honest crash images, 672 torn, none misread; 219,672 single-bit flips, none silent,
  584 in padding; 7,504 two-fault images, none silent; two appends in flight, 33 of 76 images damage;
  a short write left in place, 21,846 of 21,962 sizes damage. Then one case per row of the rule.
- `test/crc_fixture_test.cc` checks that the turn log still writes its `RFTW0001` fixture byte for byte.
- `fuzz/fuzz_framing.cc` (libFuzzer, `JOURNAL_FUZZ=ON`) and its replay test check the reader's
  claims on arbitrary bytes.

## Design vs code

- The design names the component "the emitter, the scanner, the growing-file read and D6's tail
  classifier". The turn log's whole-block scanner (`scan_segment`) stays in `turn_log.h`: its gaps,
  epochs and block manifests are the turn log's recovery model; it shares the header and fragment
  checks.
- "Torn at a 512-byte multiple" uses the sector size (512) whatever the block size, since page sizes
  are multiples of 512.
