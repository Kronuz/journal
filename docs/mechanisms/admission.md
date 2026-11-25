# Admission pools

How storage admission funds its two protected pools, and how a ledger rebuilt from a census after
a crash funds them. The code is `Admission` and `StoragePermit` in `include/journal/admission.h`;
the overview is [ARCHITECTURE.md](../../ARCHITECTURE.md), Inventory and admission.

## Pools and refill

`hard` bounds what the store holds. Two pools inside it, Control (`control_pool`) and Replacement
(`replacement_pool`), are set aside from what `used` and the outstanding permits leave; Normal
permits spend only what remains after both (`free`), and only while both pools are whole
(`normal_ready`). A Control permit spends the control pool and a Replacement permit the
replacement pool. A settlement refunds a permit's unused peak to its own pool; a cancellation
before IO refunds the whole peak.

`refill` runs after every settlement, cancellation and `credit_durable_reclaim`, and on
construction. It tops control up first, then replacement, from `free`. At the limit `free` is 0, so
a pool spent there stays short until a durable deletion credits bytes back.

## Recovery

A ledger built from a census (`Admission(inventory, limits)`) knows only `used`. The pool funding
it can give is `T = hard - used`, and that total is the same whatever split the ledger had before
the crash: an owned store whose Control appends drained x bytes at its limit held (C - x, R), one
whose last checkpoint grew it by y held (C, R - y), and with x = y the census cannot tell them
apart. Construction funds control first, as `refill` does, so every store opens as it did before
this rule: the same control share, the same pre-Start Control admission. Replacement keeps a
claim on control's available bytes for what replacement-first funding would have given it:

    claim = min(replacement_pool, T) - replacement_available     (per resource)

| Event | Effect on the claim |
|---|---|
| Construction | Set as above; 0 when `T` funds both pools whole, and 0 over the limit |
| `reserve(Replacement, peak)` | The permit spends `replacement_available` first, then takes `min(claim, control_available, the rest of peak)` from control; the claim drops by what it took, which the permit records |
| `grow_replacement` | The same, for the growth |
| Settlement of that permit | Nothing returns: the taken bytes are replacement's from then on |
| Cancellation before IO | What it took returns to control, as far as control lacks it (`control_pool - control_held - control_available`), and to the claim; the rest stays replacement's |
| Every `refill` | The claim is capped at the replacement pool's deficit (`replacement_pool - replacement_held - replacement_available`), so it ends once replacement is whole and never lets a permit pass the pool |
| Control and Normal permits | No effect; they never spend the claim |

A ledger that was never short after its census has claim 0 and behaves exactly as before. After a
crash a Replacement permit is admitted whenever either split would admit it: the store that held
(C - x, R) reopens with (C, R - x) and a claim of x, so a whole replacement cycle leaves
(C - x, R), the pre-crash split, and its checkpoint and reclaim make both pools whole again.
Without the claim, that store was refused any checkpoint above R - x, Normal work waited for whole
pools, and nothing refilled at the limit: it never compacted again.

The rule spends control's bytes only when a replacement cycle asks, so a group that needs a Control
pack before it can start still gets the control share it got before. Funding replacement first at
construction would have refused that pack to every store with `T` below `replacement_pool` plus
the pack.

The trade: while a claimed cycle runs, control is short by what the cycle took, until the cycle's
reclaim refills control first. Before this rule that Replacement was refused and control stayed
whole; now the Replacement is admitted and a later Control permit can be refused. Consensus's
install cutover is one: `request_install` reserves its Control cutover permit after the
snapshot's Replacement, so it can return Pressure where the snapshot's Replacement used to be
refused. No store loses an operation it has today: the claim is spent only by a Replacement the
old split refused.

Tests: `test/admission_test.cc` `recovery` (the review probe's two drained stores: Replacement(R)
admitted after the census, the checkpoint and reclaim leave both pools whole), `claim` (each
event above by value, and the claim's formula on a census short of control) and
`short_schedules` (random schedules over short censuses: each pool's bound, the hard limit and
the cancel round trip after every step). `test/admission_mutants.py` holds the claim's mutants
these cases kill.
