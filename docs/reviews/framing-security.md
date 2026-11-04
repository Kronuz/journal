# Security review: the block framing (`include/journal/framing.h`)

> Reviewer: security reviewer, 2026-11-04. Subject: journal `phase/manifest-log-steps12` at
> `journal:5336512` (draft PR #2), equal to `origin`. Design: strata `design/manifest-log`,
> `docs/design/manifest-log.md` revision 1 (D2, D6, Limits), with its review
> `docs/reviews/manifest-log-design.md`.

**Verdict: APPROVE WITH CONDITIONS.** No blocker. The reader is memory-safe on every input I
built or fuzzed, and its allocations are bounded by input bytes. Its time is linear. The torn
rule drops data silently only in the cases the design already lists as known residuals. Two
low findings are conditions for PR #2's merge. Neither one affects strata's 512-byte stream.

## Threat model

The design's threat model is a trusted cluster whose on-disk bytes can be damaged (a crash, a bad
disk) or crafted (a hostile copy). The attacker is **a malicious input file**: the scanner and the
reader take bytes that anyone with local file access, a restore, or a bad disk can produce. A
crafted file can always truncate a log at a record boundary, which reads clean. So the
torn-versus-damage boundary matters for honest faults. Against a crafted file, the properties that
matter are memory safety, bounded resources, and that the verified bindings (magic, identity,
segment, block number, epoch) really are verified.

## What was attacked, and held

- **Lengths and offsets.** `parse_header` bounds the header by `block_bytes` before the extent,
  and by the bytes present before reading the CRC. `parse_fragment` checks
  `fragment_header + length <= room` before the extent, so a length never reaches past its block.
  It checks the bytes present before the CRC. Every `substr` in `scan` and in
  `detail::whole_record_after` stays within `bytes`: positions advance only by verified sizes,
  and the `position >= eof` test comes before each fragment read. No 64-bit offset arithmetic
  can wrap within a file's size.
- **Allocation.** `scan` holds the input plus one copy of each record's bytes (about 2× the
  input). `read_file` allocates `file.size()`. `read_record`'s payload buffer is at most 65,535
  bytes per fragment, and the records it assembles are bounded by the file.
- **Time.** `scan` is one pass. `stop` runs once, and its `whole_record_after` makes one more pass
  over the blocks after the failure. Nothing recurses.
- **Header semantics.** The fuzz finding fixed at `journal:5336512` holds: a `0xffff` count with
  nonzero manifest fields is refused (with no manifests, the fields must be zero).
- **`record_end` against the emitter.** At ten block sizes, 256, 257, 270, 333, 511, 512, 1000,
  4096, 32768 and 65559 (the developer's test covers 256 and 512), 24,000 random
  (position, size) cases give 0 differences. It wraps only for sizes near 2^64
  (`record_end(0, 2^64-1)` returns less than the size). Callers pass the sizes of in-memory
  records, so no action is needed (probe `j2_record_end`).
- **Fuzz.** `journal_fuzz_framing` ran for 3 minutes under ASan+UBSan (Homebrew LLVM), from the
  fixture seeds for both streams, with a dictionary (both magics, the counts `0xffff` and
  `0xfffe`, the fragment types, the stream's segment, epoch and identity). It executed 138,871
  inputs, peak RSS 469 MB, with no finding.

## Findings

### SR1 (low): a header exactly one block long lets the next block skip its header check

`parse_header` accepts `header.size == stream.block_bytes` (`framing.h:154`). `scan` then sets
`position` to the next block's start. It computes `room` as a whole block and parses the bytes
where that block's header belongs as a fragment (`framing.h:273-278`), so that block's magic,
identity, segment, block number and epoch are never checked. `whole_record_after` has the same
shape. Attacker: a malicious input file. Gain: a block that carries no header reads as valid. This
widens what a crafted file can forge only slightly, since the CRCs are unkeyed. The cost is that
the verified bindings stop being an invariant.

This is reachable only when `block_bytes ≡ 14 (mod 16)` on a stream with manifests: 62 + 16·count
must equal the block size. `valid_block_bytes` admits such sizes (for example 270). Strata's
512-byte stream has no manifests, and the turn log's 32 KiB is not of that form. The emitter caps
a manifest at half a block, so honest bytes never do this.

- Probe: `.scratch/secprobe/j1_header_fills_block.cc`. On a 270-byte block with a header of
  count 13, a fragment planted where block 1's header belongs reads as a whole record, with
  tail `clean`.
- Fix: refuse a header that leaves no room for a fragment, `header.size + fragment_header + 1 >
  block_bytes`. That matches the comment at `framing.h:48` and the emitter's own cap. The
  alternative is to make `valid_block_bytes` refuse sizes a header can fill exactly. Add a
  regression test that forges the probe's bytes and expects damage.

### SR2 (low): `read_record` checks less than the scan it trusts

`read_record` reads the count at each block start and steps over `header_bytes(count)` bytes
without validating them (`framing.h:182-186`). It never compares a fragment's segment and epoch
with a stream's: it takes no `Stream`, and the CRC covers the fields but nothing compares them.
Attacker: a malicious input file, or a file that changed after the scan that produced the offset.
Gain: a record from another segment or epoch reads back as this stream's. A count of `0xfffe`
steps over about 1 MiB of "header" to a fragment anywhere.

- Probe: `.scratch/secprobe/j3_read_record.cc`. A record that segment 99, epoch 42 emitted reads
  back through the segment-3 reader. A record spans a block whose header claims count `0xfffe`
  and is assembled from a fragment 1 MiB later, while `scan` of the same bytes says `damage`.
- Strata's manifest log does not call `read_record` (it scans the whole file). The turn log does.
- Fix (either): take the `Stream`, run `parse_header` at each block start, and compare each
  fragment's segment and epoch. Or state in the comment that the offset must come from a `scan`
  of the same unchanged file, and that nothing beyond the fragment CRC is checked.

### SR3 (info): damage the torn rule reads as torn

These cases match the design's Limits (ML-R10, ML-R17). I record them so a re-check does not
rediscover them as new.

- Zeros from a record's start to its block's end drop every record after that point in the block.
  In probe `j4_torn`, zeroing edit 2 of 3 reads `torn` with 1 record: a lost write of an
  acknowledged edit costs up to the edits in that sector.
- A whole multi-block last record whose first piece reads as zeros is torn. A crash and a lost
  write cannot be told apart without an independent record.
- `whole_record_after` ignores blocks whose header has another epoch. Zeros followed by such a
  block holding a whole record read `torn` (probe `j4_torn`, case 4). The manifest log's epoch is
  always 1 and a log is never recycled, so only a crafted file does this, and a crafted file can
  truncate anyway. A mid-log zero run with whole same-epoch records after it reads `damage`, as
  D6 requires.

No action: the classifier does what D6's table says, and the residuals are the design's.

## Conditions

1. SR1: the header-leaves-room bound plus its regression test, before PR #2 merges.
2. SR2: the stream checks in `read_record`, or the documented precondition, before PR #2 merges.

Neither blocks strata's step 3: strata's stream is unaffected by SR1, and strata does not call
`read_record`.

## Probes (kept in `.scratch/secprobe/`, ignored by git)

- `common.h`: a header and fragment forger.
- `j1_header_fills_block.cc` (SR1), `j2_record_end.cc` (record_end against the emitter at ten
  block sizes, and its range), `j3_read_record.cc` (SR2), `j4_torn.cc` (SR3).
- `framing.dict`, `corpus/`, `fuzz-framing.log`: the 3-minute fuzz run.
- Built by strata's `.scratch/secprobe/build.sh` (ASan+UBSan, `-O1`, Homebrew LLVM, journal's
  `build-ml/_deps` headers).

**Not run:** TSan (nothing is concurrent); Linux, FreeBSD and arm64 (left to PR #2's CI, green on
nine jobs per the ledger); the turn log's own `scan_segment` (out of scope); a fuzz run longer
than 3 minutes; a structure-aware mutator.
