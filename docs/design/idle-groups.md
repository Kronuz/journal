> Status: DESIGN (2026-11-20) for Detent quest journal-idle-groups (Xapiand D148 Q3; strata
> `docs/design/rocksdb-parity.md` D10 items 2, 3 and 5, at strata:0d93765c).
> Code cited at journal `1222e3b` (`main`). Answers the turn-log-parity design review's F2, F3 and
> F5 (Xapiand `.campaign/xapiand/reviews/turn-log-parity-design.md`).
> Format: `RJGT0001` and `RFTW0001` unchanged; one host API contract changes (Contracts).

# Idle groups leave the log

## Names

Two steps, two words, one owner: this quest's mechanism, `journal:idle-park`, is the only park
mechanism; strata consumes it and defines none of its own.

- **Evicted** (in memory): a released group's records stay live in the log, which keeps only
  their positions (`TurnDriver::evict`, `TurnLog::evict`, run by `Host::settle`). Nothing on disk
  changes; `live()` restores it.
- **Parked** (durable): the table state that holds the group's last checkpoint record, with
  nothing in the log or the heap (`Host::park`, `Host::close_group`). On disk it is the `closed`
  entry state (D1); a parked group wakes by `reopen_group`.

A host must hold millions of open indexes. Today every open group keeps its latest checkpoint
record live in its loop's turn log until it closes (`turn_log.h:399-404`, `note()`), and its table
entry, log index and sequence in memory. That record pins its segment until forwarded
(`turn_log.h:411-450`), and every re-partition copies it (`host.h:635-675`), so the log's live
bytes, its forward copies, the re-partition's copy and its damage blast radius all grow with
open groups, not with writes. Creating a group pays two barriers of its own
(`host.h:400-407` through `update`, `commit`, `replace_file`: the shard's sync, then the
directory's), and so does waking a closed one (`host.h:408-412`).

This design makes three changes:

1. An idle group is **parked**: its last checkpoint record moves into its table shard, and the
   log and the host's memory forget it.
2. Table transitions that precede a group's first record (create, wake) **join the turn**. They
   are written before the turn that carries that record, with one directory sync for every such
   transition in the turn.
3. A re-partition **streams** its copy one old log at a time.

## Decisions

**D1. Parked is closed: one table state.** A parked group is a `closed` table entry: `base`
holds its last record's sequence and `manifest` holds that record, a checkpoint covering every
batch. Close (by the holder, `Host::close_group`) and park (by the host, `Host::park`) both go
through the same commit-then-forget path (`host.h:419-432`): the shard is durable, then
`TurnDriver::forget` runs. Waking a parked group is reopening a closed one. The state is durable,
since it is the shard file, and recovery already exempts it from log damage. `TurnLogGroups::open`
lists only `open` entries (`host.h:555`), and records through `base` are superseded
(`turn_log.h:516`). This is the state review F3 asks for, and it already exists.

- *Lost: a new `parked` state beside `closed`.* It would be a second state with the same bytes,
  the same exemption and the same wake. The only difference would be who asked, and neither
  recovery nor the table needs to know that.
- *Lost: parking as an `open` entry with `base` set* (D10 item 3 as written). Recovery lists
  every open group as at risk from any damage (`turn_log.h:690`), which is review F3.

**D2. What parks.** `Host::park(loop)` runs on the loop and off the turn (as `prepare()` does).
It parks every group of the loop that:

- has been released (no `Store` holds it and its hold has settled, `host.h:382-396`);
- has nothing staged and no sealer registered;
- has exactly one live record in the log, a checkpoint.

One table commit covers every such group of the loop. Parking a held group is refused by
construction: its holder's next record follows sequences only the log knows. A released group
with batches after its checkpoint stays in the log, as it does today, until a holder recovers it
and checkpoints. strata and Xapiand park a group by closing it (`turn_seal.hh` `close()`). They
call no `hold`, so `park` never touches a group they own. How long a group must stay idle first
is the caller's policy: it decides when to call `park`.

- *Lost: parking inside `settle()`.* `settle` runs inside every `hold()` (`host.h:369`), so a hold
  would pay table I/O.
- *Lost: parking groups with batches after the checkpoint.* Journal cannot apply a batch. Only
  the group's owner can fold batches into a checkpoint.

**D3. A parked group holds no stream and no heap.** Once the commit is durable, the group leaves
memory as a closed entry does today ("Closing" in `docs/mechanisms/group-lifetime.md`):

- the shard map drops it (`host.h:507`);
- the log drops its index, sequence and released positions (`turn_log.h:217-228`);
- the driver drops its recovered copy and refusal. It keeps no retired floor, because
  `forget(group, base + 1)` keeps one only above the table's floor (`turn_driver.h:157-172`);
- `opened_` dropped it at release.

Its checkpoint's live count leaves its segment. Once no other live record remains there,
`reclaim()` releases the segment with no forward copy (review F5).

| Where | Bytes per parked group |
|---|---|
| Heap: host, driver, log | 0: measured -0.0006 B per group from 10k to 1M parked (`journal_test park_count`, job 261120-084841-834ac9 at journal:b55e34d) |
| Turn log | 0 live; its old records are superseded and reclaimed with their segments |
| Table shard `groups-<slot>` | 45 + payload: id 8, incarnation 8, state 1, mark 8, high 8, base 8, length 4, then the record |
| Payload, journal `Store` | its manifest: 108 B, plus 28 per dependency, at most 332 (`journal.h:472-474`) |
| Payload, consensus | its checkpoint record, which carries consensus-quiescence A5's closed-state hard state inside it, opaque to journal |
| Artifacts | the group's own files, unchanged |

At 1M parked groups with 108-B payloads the table holds 153 MB in 4,096 shards, about 37 KB
each. The A2 job's 17-byte test payloads measured 62.0 B per group on disk, and the log back to
one segment; creating and parking the million took 93 s on a memory disk, most of it rewriting
shards (Q1). Reading a parked group back, through `status`, `entry` or a wake, reads its one shard
(`host.h:448-457`).

**D4. Wake rejoins through the table record.** A `Store`'s `recover` of a parked group takes the
path a closed group takes today (`journal.h:599-608`): restore from the table's record, then stage
it again as the group's checkpoint at `max(base, retired) + 1`. The log then holds the group from
a checkpoint of its own, and recovery needs no rule for a stream that starts at a table base.
The flip from closed to open is no longer committed on its own (D5).

- *Lost: continuing at `base + 1` with no restaged checkpoint.* The retired floor
  (`host.h:458-467`) reserves `base + 1` for a closed incarnation's stale writer. The new stream
  would start with a gap, and recovery would need a second contiguity rule. The restaged record
  costs one record's bytes in a turn the group is writing anyway.

**D5. Create and wake join the turn.** `create_group` and `reopen_group` change the entry in
memory and mark its slot dirty, with no commit. `TurnDriver` calls a host hook
(`TurnDriver::before_turn`) in `start()` once a turn has records and before `TurnLog::stage`.
The host's hook commits the loop's dirty slots:

1. Each shard's `.pending` is written.
2. All of them are synced in one `sync_files` call, then renamed.
3. One directory sync follows.

So a transition is durable before the turn carrying its group's first record is written (review
F3: "in or before the turn"). The turn's barrier is unchanged. A turn with transitions pays one
multi-file sync and one directory sync, whatever the number of transitions, where today each
transition pays two of its own.

- A create's entry starts with no serial block (`high = 0`). `next_artifact` reserves the first
  block synchronously, as it reserves every later block (`host.h:434-440`). No artifact is
  therefore named by a block that the table does not hold durably.
- A failed hook commit fails the driver, as a failed turn does (fail-stop): the shards may hold
  either version, and nothing staged after them is written. The host recovers whichever state
  the disk kept when it reopens.
- A slot committed early (by a park, a close or a block reservation in the same slot) writes its
  deferred transitions too. Being ahead of the log is safe (crash table below).
- *Lost: transitions as records in the turn log.* A wake's evidence would sit in the same blocks
  as its data. A damaged block with no readable manifest would then hide both, and recovery
  would read the stale `closed` and silently drop acknowledged records (review F3's second
  half).
- *Lost, for now: a separate table log* (RocksDB's MANIFEST shape: append each transition and
  compact the shards off the turn). It writes about 150 B per transition instead of one shard
  (Q1). It is a new file, format and recovery path. This design keeps the shard rewrite and
  states its cost.

**D6. Re-partition streams one old log at a time.** `Host::repartition` (`host.h:635-675`) holds
every log's live records in one map. It changes to:

1. Open the `L_new` new logs of generation + 1.
2. For each old log in turn, read it (`TurnLog::read`), append each group's records to its new
   log's pending turn, and flush that turn when it reaches a quarter segment.
3. Commit through `layout`, as today.

A group lives in one old log, and its records are read in sequence order, so none is lost or
reordered. Memory is one old log's live records plus `L_new` pending turns. With idle groups
parked, a log's live records are bounded by its retention window (`retain_segments × segment_bytes`),
not by the number of open groups. Parked groups copy nothing: their state is in the table, and
the table is sharded by slot, not by loop. The commit point and every crash outcome are unchanged
(`placement_test remap` keeps cutting every step).

**D7. Damage reaches only groups with live log records (review F2).** A parked group's durable
state is its shard (CRC32C, replaced whole and synced) and its artifacts. Recovery ignores its
superseded log records, and no rule about `open` groups names it, so no log damage costs it,
manifested or not. Two kinds of group remain at risk from a damaged block:

- groups with live records that wrote nothing after it;
- `open` groups that the log holds nothing of.

With D2, those are the groups active within the park interval. With one log per device the set
grows from one loop's groups to the device's, as F2 says. With parking, that growth applies to
the device's active groups, not to its open indexes. A woken group is `open` in the table before
its first record is written (D5). If that record sat in an unmanifested damaged block, recovery
reports the group lost (`turn_log.h:690`) rather than reading it as parked. The A3 job's damage
case checks both halves on damage a manifest attributes. Damage that also takes the block header
holding that manifest loses acknowledged records silently on journal `main` (`1222e3b`), parked
groups or not: a pre-existing recovery gap outside this design, reported to Detent's lead. A damaged shard file stays what it is today: the host refuses to open
(`host.h:186`, Q3).

**D8. Forward copies stay (review F5).** `forward_slice` (`turn_log.h:411-432`) still copies the
oldest segment's live records past `retain_segments`. It is the fallback for every record that no
checkpoint or park releases: held idle groups, and released groups with batches after their
checkpoint. Parking removes a group's records from the live set, so its segment is reclaimed
without a copy (`turn_log.h:436-450`). The A2 test checks that the log returns to its current
segment once its groups park.

## Crash at each step

Acknowledged means the call returned: a turn's barrier, a close's commit or a park's commit.

| Step | Disk after a power cut there | Recovery |
|---|---|---|
| Create: entry in memory | nothing | `none`: never acknowledged |
| Create: hook commit (shard written, renamed, directory synced) | old shard, or new with `open` and no record | `none`: the "first turn never durable" path creates the next incarnation (`host.h:400-405`) |
| Create: the turn | the first record durable or torn | `stored` with it, or `none` as above |
| Wake: entry flipped in memory, record staged | shard `closed` | parked, with the same content |
| Wake: hook commit | shard `closed`, or `open` with `base` and no log record | parked, or `stored` from the table's record (`journal.h:599-608`): the same content |
| Wake: the turn | restaged checkpoint and first batch, durable or torn | from the log's checkpoint, or as above |
| Park: shard commit | old shard (open, checkpoint live in the log), or new (`closed`, `base`) | from the log's checkpoint, or from the table, which supersedes it: the same record |
| Park: forget, then reclamation | segment removed or not | superseded records ignored; the table wins |
| Re-partition | unchanged protocol: `layout` is the commit point | old generation authoritative until `layout` is replaced |

A failed park commit leaves every group in it `open` and in doubt, as a failed close does
(`host.h:430`). A failed hook commit fails the driver (D5).

## Assumptions about turn-log-parity

turn-log-parity's one-log-per-device shape (its task 10, D6 there) is not on journal `main`; this
design is written against its quest file. Nothing here depends on how many logs there are:

- **Parking needs no log.** A parked group is in no log under either shape. A changed loop count
  re-homes it for free, because placement is a hash and the table is per slot.
- **D5's rule names no loop.** It reads: a transition is durable before its group's first record
  enters any turn. With one log per device, each loop commits its own dirty slots before it hands
  its sealed records to the owner's turn. A slot stays owned by its loop's thread
  (`docs/mechanisms/host-ranges.md`).
- **D6's streaming is per old log.** With one log per device it runs only when the device count
  changes.

## Contracts

- `Host::create_group` and `Host::reopen_group` are durable once the group's first record's turn
  is durable, not when they return. Callers already wait on that record (`Store::create`,
  `recover_shared`). strata's `turn_seal.hh` says "each is durable when it returns" of `create`
  and `reopen`. That holds only at its journal pin, so moving the pin to this change moves the
  promise to "once the group's first record is durable". Xapiand's group-core consumes
  `journal:driver`: its lead hears this before landing.
- New: `Host::park(loop)`, which returns how many groups it parked, and
  `TurnDriver::before_turn(std::function<void()>)`.
- A create's first serial block moves from the create to the first `next_artifact`.
- No byte changes in `RJGT0001` or `RFTW0001`.

## Checks

| Criterion | Check |
|---|---|
| A1 | This note |
| A2 | `host_test park_count`: N groups are created, checkpointed, released and parked (`JOURNAL_PARKED_GROUPS`, 1,000,000 in the job). It checks the heap slope per parked group (at most 1 B), that the log holds none of them and is back to its current segment, and a sample that wakes exactly |
| A3 | `host_test idle_crash`: power cut after every mutation of create, wake, park and a re-partition, each followed by three restarts. Every acknowledged record must come back, and each group must be at an attempted state no older than its acknowledged one. `host_test idle_damage`: two damaged blocks of a woken group's batch; parked groups open, and the woken group, whose only post-wake records were there, reports lost. Mutants in `test/mutants.py` (`parked-as-open`, `wake-skips-commit`, `create-with-block`, `park-keeps-log`), each `--fails-without` |
| A4 | The suite and CI |

## Questions for the owner

- **Q1. Table transition cost.** A hook commit rewrites each dirty slot's whole shard. At N groups
  that is N/4,096 entries × (45 + payload) per slot: about 37 KB at 1M groups with 108-B payloads.
  Creating 1M groups in turns of k writes about N² × entry / (2k) bytes of shards. The options:
  1. Keep it, with the cost stated.
  2. Use 65,536 table shards, a different key from placement's 4,096 slots, for 16× fewer bytes
     per transition and 16× more files.
  3. Add a table log (MANIFEST shape) with off-turn compaction, about 150 B per transition.

  Recommendation: (1) now, then (3) if the many-indexes workload measures the bytes as a loss.
- **Q2. Park policy.** Who calls `Host::park` and how often. Recommendation: the loop's owner
  calls it when idle, as it prepares spares, with no idle window inside journal. Callers that
  want a window keep the group held.
- **Q3. A damaged shard.** Today it fails the host's open (`host.h:186`). With millions of parked
  groups that is the host's whole blast radius from one 37-KB file. The options:
  1. Keep it.
  2. Write two copies of each shard.
  3. Call that shard's groups lost.

  Recommendation: (3), as the log's damage rule does, in a later quest.

## First buildable steps

1. `TurnDriver::before_turn` and D5's deferred `create_group` and `reopen_group`, with one batched
   commit (`sync_files`, then one directory sync). Fix `host_test failed_reopen` to the fail-stop
   contract.
2. `TurnLog::peek` (read a live record without restoring an evicted group), then `Host::park(loop)` through the
   close path, and `host_test park_count`.
3. D6's streaming re-partition (`placement_test remap` unchanged).
4. `host_test idle_crash` and `idle_damage`, each with its mutants.
