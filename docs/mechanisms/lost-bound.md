# Lost bound

How damage to a turn log bounds a group's loss instead of losing the group, where the bound is
stored, and who may continue past it. The code is `TurnLog::recover` and `TurnLog::keep` in
`include/journal/turn_log.h`, `TurnDriver::keep`, and `Host::open`, `mark_bounds`, `status`,
`bound` and `lose` in `include/journal/host.h`. Damage detection itself is
[turn-log-damage.md](turn-log-damage.md).

## The bound

A damaged block costs the groups its manifest names (turn-log-damage rule 3). The manifest holds
each group's highest sequence in the block (`framing::Emitter::record`), so a group lost nothing
above that sequence there. Its bound is the highest such sequence over every damage that names it:
the damaged blocks' manifests, the records of turns dropped past the newest epoch's last kept turn
(rule 5), and a kept manifest naming more than the group holds. Every record the damage could have
taken is at or below the bound.

Recovery then sorts each named group three ways:

| The group's records after the bound | Recovery | Who can continue it |
|---|---|---|
| A checkpoint among them | Whole from its latest checkpoint, as if undamaged: the checkpoint covers every record before it (`stage` refuses one whose `covers` is below its last record) | Anyone |
| Contiguous from bound + 1, or none | Lost, and bounded (`TurnRecovery::bounded`): the bound and those records | A reader that holds the group through the bound |
| A gap | Lost | Nobody |

Damage no manifest names (`unknown`) still loses every group: no bound is known.

## Where it is stored

A bound must outlive the damage. Retention reclaims the damaged segment once the group's records
there are no longer live, and a repaired or re-partitioned log no longer shows the damage, so the
log alone forgets it. The host stores it in the group's open table entry (`GroupEntry::bound`; in
the shard, the state byte's `bounded_state` bit, then 8 bytes after the base) before anything
serves: `Host::open` collects every loop's bounded groups, commits their bounds (`mark_bounds`),
and only then keeps them in the log (`TurnDriver::keep`), so their records after the bound are
live again and the group continues after them. A re-partition stores them before its layout
commit and starts each bounded group in the new log after its bound (`TurnLog::keep`).

Each open passes the stored bounds back (`TurnLogGroups::bound`). Recovery treats a group's
records through its bound as superseded, as it treats those through a closed group's base: they
are not required, damage among them names nothing, and the group's records may start at
bound + 1. A second damage names the group only above the stored bound, so the bound only rises.
A crash before the commit leaves the damage in the log, and the next open finds the same bound.

The bound clears when the group's log keeps a checkpoint after it (the next open sees it first)
or when the group closes into the table, whose base then supersedes it.

## Who may replay past it

`Host::status` reports `GroupStatus::bounded`, and `Host::bound(group)` the bound. A reader whose
state already holds the group through the bound replays the records after its own position and
continues: strata's index, whose head records its last applied sequence. A reader whose state
ends below the bound calls `Host::lose(group)`, which marks the group lost durably, removes its
artifacts, and refuses its id, as an open does for a lost group. A `Journal` Store holds its group
only through its latest checkpoint record, and a bounded group has none after the bound, so its
recover loses a bounded group and throws `Corruption`.

## Limits

- Damage no manifest names loses every group, bounded ones too, as before.
- A group whose damaged block's manifest is itself damaged has no bound until a later source
  (a copy flag, a per-segment roster) gives one.
