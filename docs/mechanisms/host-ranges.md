> Status: CURRENT (2026-11-20) which thread owns `Host::ranges_`, the table of the groups' views'
> serial ranges, in `include/journal/host.h`.
> Consumers: every shared Store (`Journal(Host&, group)` takes its view through `group_io`), and
> reclamation scans that run a view's scan primitive on a completion worker.
> Format: unchanged; everything here is in memory.

# The range table: one per loop, owned by the loop's thread

A group's view (`Host::group_io`) lists the group's artifacts by probing the serials in the
group's `[mark, high)`. The view reads that range from a `host_detail::Range`, and the host keeps
a table from group id to the live `Range`s so that a commit of the group's table entry
(`commit` -> `publish`) updates every view at once. This note says who may touch that table.

## The rule

1. **One table per loop.** `ranges_` is a vector of `host_detail::Ranges`, one per loop, each a
   `std::multimap<group, std::weak_ptr<Range>>` with its own sweep threshold. Group `g`'s ranges
   live in table `loop(g)`.
2. **The loop's thread owns its table.** Table `l` is read and written only by the thread that
   runs loop `l`: `group_io(g)` (sweep, lookup, insert) and `publish(g)` (from `commit`, which
   `update` runs for every change to `g`'s table entry) both run on `loop(g)`'s thread, the
   thread that already owns `g`'s table shard. No lock guards a table, and no thread touches
   another loop's.
3. **A `Range` crosses threads under its own mutex.** A view's scan primitive may run on any
   thread (a completion worker running a reclamation). It reads only its `Range`, through
   `Range::mutex`, never the table; `publish` writes the `Range` under the same mutex.
4. **The open runs alone.** The constructor's commits (`mark_lost`, a re-partition's) run before
   any view exists, and the tables are sized last, so `publish` returns at once while the vector
   is empty and the constructor's thread owns no table.
5. **Debug builds check it.** `owned_ranges(g)`, the one way `group_io` and `publish` reach a
   table, binds table `loop(g)` to the first thread that touches it after the open, and asserts
   that every later caller is that thread.
6. **A handoff moves it.** `Host::adopt_loop(l)` makes the calling thread table `l`'s owner, and
   the check follows it: the new thread's calls pass, the old thread's abort.

One thread may run several loops (the tests drive every loop from one thread). A loop's table
stays with its first thread until a thread adopts it.

## The handoff

A caller that serializes a loop itself, rather than running it on one thread, calls
`adopt_loop(l)` under that serialization before it touches loop `l`'s table from the current
thread. strata's `turn_sealer` is that caller: it runs every table call (create, close, reopen,
attach) under its turn mutex, from whichever thread calls in. The mutex orders the handoff after
the old owner's last table call and before the new owner's first, which is what makes the table
safe without a lock of its own; the handoff only tells the debug check who holds it. It is the
caller's statement, not a guess: the host never infers an owner from a held lock, since it cannot
see which one serializes the loop. A release build keeps no owner, so the handoff costs nothing
there but the bounds check that refuses a loop the host lacks (`std::out_of_range`).

## The callers

`group_io` has one production caller, `Journal(Host&, group, ...)` (`include/journal/journal.h`),
which every shared `Store` runs at construction. Consensus constructs its Stores in
`Driver::open` (`consensus/include/consensus/driver.h`), a plain call on the loop (`on_loop()`
asserts it) that refuses a group placed on another loop (`host_.loop(id) != loop_`), then builds
the group's `Worker` and its `Store`. Detent, strata and Xapiand construct no shared Store and
call no `group_io`. The tests and `bench/group_io_bench.cc` call it on the thread that runs the
group's loop; `publish` runs from `update`, which `create_group`, `close_group`, `reopen_group` and
`next_artifact` run on the group's loop.

## Why not one table under a mutex

One shared table is what raced: `group_io` on loop A's thread inserted into and swept the
multimap while loop B's thread read it in `publish` (TSan, job `261118-144636-e96deb`, journal
main at `6a37f19`). A mutex over one shared table would order the accesses, but its hold is not
bounded: a sweep visits every entry, and a full sweep of 1,000,000 random-keyed entries takes
about 89 ms (the "before" column of job `261118-145320-05f3b3`, which ran `erase_if` over the whole
table on every call). For that long every other loop's commit would wait on its `publish`. The
per-loop tables cost nothing: each loop already owns its groups' table shards on its own thread,
and the range table follows the same partition.

## Test

`host_test loop_views` runs two loops' threads at once. Each takes 2,000 views of its own groups,
holding every fourth so its table sweeps, and creates 20 groups, each create committing and
publishing to a view it took first. Under TSan (`-fsanitize=thread`) it reports no race, and every
held view of a created group lists the group's first artifact afterwards. A mutant that sends
every group to table 0 (one shared table) makes TSan report the race and the test exit nonzero.
`host_test view_sweep` checks the sweep bound on one loop's table.

`host_test loop_handoff` adopts loop 0's table on a second thread, which takes a view and creates
a group; in a forked child, a view or a create from the old owner and a view from a third thread
each abort on the check, and adopting the table back makes the first thread's calls pass.
`host_test handoff_close` is strata's t6w case: a group created and checkpointed on the first
thread and closed on another aborts without the handoff and closes with it.
