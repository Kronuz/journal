> Status: DESIGN (2026-11-20) for Detent quest journal-idle-groups (Xapiand D148 Q3; strata
> `docs/design/rocksdb-parity.md` D10 items 2, 3 and 5, at strata:0d93765c).
> Code cited at journal `1222e3b` (`main`). Answers the turn-log-parity design review's F2, F3 and
> F5 (Xapiand `.campaign/xapiand/reviews/turn-log-parity-design.md`).
> Format: `RJGT0001` entries and `RFTW0001` unchanged; the table's files are renamed and bounded
> (D9); one host API contract changes (Contracts). The lead's rulings on Q1-Q3 are D9-D11.

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
| Table shard `groups-<slot>-<node>` | 45 + payload: id 8, incarnation 8, state 1, mark 8, high 8, base 8, length 4, then the record |
| Heap: the shard index (D9) | 8 B per shard file, about 0.2 B per group: measured 0.24 B per group at 1M (job 261120-092916-3cb2bf at journal:48df21b) |
| Payload, journal `Store` | its manifest: 108 B, plus 28 per dependency, at most 332 (`journal.h:472-474`) |
| Payload, consensus | its checkpoint record, which carries consensus-quiescence A5's closed-state hard state inside it, opaque to journal |
| Artifacts | the group's own files, unchanged |

At 1M parked groups with 108-B payloads the table holds 153 MB. With 4,096 whole-slot shards that
was about 37 KB per shard, rewritten by every transition; D9 bounds each shard file at 4 KiB. The
A2 job's 17-byte test payloads measured 62.6 B per group on disk and the log back to one segment.
Reading a parked group back, through `status`, `entry` or a wake, reads its one shard file.

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
memory and mark the group dirty, with no commit. `TurnDriver` calls a host hook
(`TurnDriver::before_turn`) in `start()` once a turn has records and before `TurnLog::stage`.
The host's hook commits the shards of the loop's dirty groups (D9):

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
- A shard committed early (by a park, a close or a block reservation in the same shard) writes
  its deferred transitions too. Being ahead of the log is safe (crash table below).
- *Lost: transitions as records in the turn log.* A wake's evidence would sit in the same blocks
  as its data. A damaged block with no readable manifest would then hide both, and recovery
  would read the stale `closed` and silently drop acknowledged records (review F3's second
  half).
- *Lost, for now: a separate table log* (RocksDB's MANIFEST shape: append each transition and
  compact the shards off the turn). It writes about 150 B per transition instead of one shard
  file, at the price of a new file, format and recovery path; D9's bounded shards bring a
  transition to one 4 KiB file without one.

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
reports the group lost rather than reading it as parked. `idle_damage` checks both halves on
damage a manifest attributes; `idle_unmanifested` checks them when the block header holding that
manifest is damaged too, which recovery reports since journal-double-damage (journal:178030b).
A damaged shard file costs only its own groups (D11).

**D8. Forward copies stay (review F5).** `forward_slice` (`turn_log.h:411-432`) still copies the
oldest segment's live records past `retain_segments`. It is the fallback for every record that no
checkpoint or park releases: held idle groups, and released groups with batches after their
checkpoint. Parking removes a group's records from the live set, so its segment is reclaimed
without a copy (`turn_log.h:436-450`). The A2 test checks that the log returns to its current
segment once its groups park.

`forward_slice` walks the log's index and its evicted set on each turn whose oldest segment, past
retention, still holds live records. That makes the walk grow with the groups the log holds,
and quadratically over a setup that adds them. A parked group is in neither the index nor the
evicted set, so parking takes it out of the walk. `TurnLogStats::forward_scanned` counts the
records visited. Over 1M parked groups the walk visits 0.02 records per turn (261120-113445-21c05a,
strict), and 0.01 over 20,000; with the `park-keeps-log` mutant, which keeps parked groups in the
log, it visits 193 per turn over 20,000 (261120-113449-d2ae31). Held and evicted groups that are not
parked still cost the walk; indexing live records by segment would remove that, outside this
quest.

**D9. Bounded table shards: a transition writes one file of at most 4 KiB (lead ruling Q1).** A
slot's table is a binary trie whose leaves are shard files, `groups-<slot:3x>-<node:9x>`: node 1
is the root, `2n` and `2n + 1` are `n`'s children, and a group's path is its mixed id's bits below
the 12 slot bits (`host_detail::node_at`). A commit rewrites only the shards its groups fall in.
One whose entries pass `leaf_bytes` (4 KiB) splits by the next bit until each part fits, holds one
entry, or sits at depth 32. Both children of a split are written, so a slot's files always cover
every path. The host keeps each slot's nodes, 8 B per file, prefix-free and in path order.

- A split renames its children, syncs the directory, then removes the split node. The removal is
  durable with a later directory sync; until then `open()` drops a node whose children cover it.
  Children that do not cover their node are an unfinished split's: the node stands, or, under a
  slot's unstored root, nothing there was durable (`host_detail::resolve`).
- A commit that fails after renaming children leaves orphans. The next commit of their slot
  removes them and syncs the directory before it renames anything, so no orphan shadows a later
  version of its node.
- Shards never merge: table entries are never removed (lost and closed ids stay).

Before and after at 1M parked groups (`journal_test park_count`, strict jobs). Each sample is a
wake (one turn) then a park (one call). The memory disk samples 2,000 transitions. The real
filesystem (`JOURNAL_DISK_DIR`: the host's directory copied to APFS, PosixIO, real syncs) samples
200. Two pairs ran. The second pair's before is journal:a0944ca (7129c53 with 893fd77's test) and
its after is journal:893fd77.

| | Before | After |
|---|---|---|
| Table bytes per transition | 15,192 (memory), 15,132 (APFS) | 3,143 (memory), 3,131 (APFS) |
| Table files per transition | 1 | 1 |
| APFS time per transition | 59.6 ms (261120-095112-ec7b19) | 61.7 ms (261120-095115-033c33) |
| Memory disk time per transition | 646 us, 1,268 us | 1,749 us, 596 us |
| Create and park 1M (batches of 10k) | 73.8 s, 126.7 s | 84.8 s, 48.5 s |
| Host open over 1M parked (memory disk) | 1.13 s, 1.70 s | 3.72 s, 1.70 s |
| Host open over 1M parked (APFS, warm cache, three opens) | 3.65, 3.32, 2.96 s over 4,096 files (261120-100752-65e1de) | 1.92, 1.88, 1.72 s over 20,958 files (261120-100755-5948a1) |
| Heap per parked group | -0.0006 B | 0.24 B |

First pair: 261120-092423-3fc8cc at journal:7129c53 and 261120-092916-3cb2bf at journal:48df21b.
Second pair: the APFS jobs.

The bytes a transition writes fall by a factor of 4.8, and they no longer grow with the host's
groups. On a real disk a transition costs its syncs, and their count does not change: the hook
commit's file and directory syncs and the turn's barrier for a wake, and one file sync and one
directory sync for a park. So the time is the same within noise, about 30 ms per transition
under macOS's full syncs. The memory-disk times move both ways between pairs, by more than the
change. faultfs walks every inode at each ordering point, so its per-operation cost grows with the
file count, about 20,000 files after (62.6 MB at 3.1 KB each) against 4,096 before. Those times measure the model, not the
mechanism.

Opening is no slower with five times the files. Over APFS with the files in the page cache, the
open is 1.7-1.9 s against 3.0-3.6 s before (journal:0c56d21 against journal:61d2a42, which is
7129c53 with the same test). These jobs do not isolate why the whole-slot open is slower. A cold
cache adds about 17,000 more file reads than before; this machine cannot drop its page cache
without root, so that cost is not measured. Open still reads every entry. Reading only the
entries the open needs (open and lost ones, and the bases of groups the log still holds) is the
next step if a host's open time matters.

- *Lost: the transition carried in the turn record that precedes the group's first record.* It
  writes no table bytes and adds no sync. But the wake's only evidence would sit in the blocks
  that carry the group's records, which is D5's lost alternative. A turn whose blocks and
  manifests are all damaged would read back as the stale `closed` entry: acknowledged records
  lost without a report, unless recovery calls every parked group lost on any damage (F2's blast
  radius). `host_test idle_unmanifested` is that case: recovery reports the damage
  (journal-double-damage), and with D5 the woken group is `open` in the table and reported lost.
- *Lost: 65,536 fixed shards.* That gives 16 times fewer bytes than 4,096, but it is not bounded:
  the bytes still grow with the host's groups.
- *Lost: an epoch per shard file, the newest winning at each hash point at open.* It orders
  versions after a failed commit as the orphan rule does, but `open()` must then read every file
  before it can tell which stand. The cover rule decides from the names alone.

**D10. The park call contract (lead ruling Q2).** The mechanism is journal's and the policy the
consumer's: Xapiand calls `Host::park(loop)` from its idle policy, as strata does from its own.

- **Thread:** the loop's thread, the one that runs the loop's turns and its groups' `group_io`
  (`docs/mechanisms/host-ranges.md`).
- **Cost:** one table commit for every group it parks: each touched shard file rewritten (at most
  4 KiB, D9), one multi-file sync and one directory sync. Finding the groups walks the loop's log
  index once (`TurnLog::idle`), O(groups the log holds).
- **Mid-turn:** between a turn's `begin()` and `completed()`, or from a waiter that turn resumes,
  `park` is safe. A group with records in the turn in flight is staged and not parked. A forwarded
  copy of a parked group's checkpoint riding that turn is dropped at `completed()`, because the
  log no longer indexes the group, and recovery ignores it, because the table's `base` supersedes
  it. `host_test park_mid_turn` checks the first half across a restart.
- A failed commit leaves each group it would have parked `open` and in doubt, as a failed close
  does (D2).

**D11. A damaged shard fails only its own groups (lead ruling Q3).** `open()` reads each shard
file; one that fails its CRC or its checks is damaged, and the host still opens:

- Every id the shard could hold (the ids on its path) is `lost`, parked, open or never created,
  because the shard's ids are unknown. `create_group` refuses them.
- The shard is never rewritten: a commit that touches it throws `Corruption`, and `mark_lost`
  skips its ids, so the damage stays visible across restarts.
- The log forgets the groups it holds of that shard: their incarnation and serials, which name
  their artifacts, were the shard's.
- Other shards' groups recover as before. The blast radius is one shard file, at most 4 KiB of
  entries (D9), about 45 groups at 1M with 17-B payloads, where it was the whole host.

`host_test table_damage` damages one shard and checks all of this across two restarts.

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
| Split: children renamed, before the directory sync | the node, and none, some or all of its children | children that cover the node win; otherwise the node, and the commit was never acknowledged |
| Split: node removed, before a later directory sync | the node and all its children | the children: the node is dropped at open |

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
- New: `Host::park(loop)`, which returns how many groups it parked (call contract in D10), and
  `TurnDriver::before_turn(std::function<void()>)`.
- The table's files are `groups-<slot:3x>-<node:9x>`, each at most 4 KiB unless it holds one
  entry (D9). A damaged one no longer fails the host's open (D11).
- A create's first serial block moves from the create to the first `next_artifact`.
- No byte changes in an `RJGT0001` entry or in `RFTW0001`.

## Checks

| Criterion | Check |
|---|---|
| A1 | This note |
| A2 | `host_test park_count`: N groups are created, checkpointed, released and parked (`JOURNAL_PARKED_GROUPS`, 1,000,000 in the job). It checks the heap slope per parked group (at most 1 B), that the log holds none of them and is back to its current segment, and a sample that wakes exactly |
| A3 | `host_test idle_crash`: power cut after every mutation of create, wake, park and a re-partition, each followed by three restarts. Every acknowledged record must come back, and each group must be at an attempted state no older than its acknowledged one. `host_test idle_damage`: two damaged blocks of a woken group's batch; parked groups open, and the woken group, whose only post-wake records were there, reports lost. `host_test split_crash`: power cut after every mutation of commits that split one slot's shard. Mutants in `test/mutants.py` (`parked-as-open`, `wake-skips-commit`, `create-with-block`, `park-keeps-log`, `split-keeps-parent`, `split-removes-early`), each `--fails-without` |
| Q2 | `host_test park_mid_turn` |
| Q3 | `host_test table_damage`; mutant `damaged-shard-fails-open` |
| F3, unmanifested damage | `host_test idle_unmanifested` |
| A4 | The suite and CI |

## Rulings

The lead ruled on the three questions this note raised:

- **Q1.** A whole-shard rewrite per transition is not acceptable at 1M groups: D9.
- **Q2.** The policy is the consumer's; the call contract is D10.
- **Q3.** A damaged shard fails only its own groups: D11.

## First buildable steps

1. `TurnDriver::before_turn` and D5's deferred `create_group` and `reopen_group`, with one batched
   commit (`sync_files`, then one directory sync). Fix `host_test failed_reopen` to the fail-stop
   contract.
2. `TurnLog::peek` (read a live record without restoring an evicted group), then `Host::park(loop)` through the
   close path, and `host_test park_count`.
3. D6's streaming re-partition (`placement_test remap` unchanged).
4. `host_test idle_crash` and `idle_damage`, each with its mutants.
5. D9's bounded shards and D11's damaged shard, with `split_crash`, `table_damage` and
   `park_mid_turn`.
