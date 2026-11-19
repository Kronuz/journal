> Status: CURRENT (2026-11-18) which thread owns `Host::ranges_`, the table of the groups' views'
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
   any view exists, some before the tables are sized, so `publish` returns at once while the
   vector is empty. The tables are sized with the other per-loop state once the loop count is
   final.

One thread may run several loops (the tests drive every loop from one thread); the rule only
forbids two threads touching one loop's table.

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
holding every fourth so its table sweeps, and creates 100 groups, each create committing and
publishing to a view it took first. Under TSan (`-fsanitize=thread`) it reports no race, and every
held view of a created group lists the group's first artifact afterwards. A mutant that sends
every group to table 0 (one shared table) makes TSan report the race and the test exit nonzero.
`host_test view_sweep` checks the sweep bound on one loop's table.
