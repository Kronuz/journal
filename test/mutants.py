#!/usr/bin/env python3
# Applies one named mutant to the checkout, builds the test that must kill it, and runs it:
# exit 0 when the test fails (killed), 1 when it passes (survived). `mutants.py NAME [cmake flags]`.
import os, subprocess, sys

MUTANTS = {
    # A turn acknowledged after its write alone: power loss then loses acknowledged batches.
    "no-sync-data": ("include/journal/turn_driver.h",
        "request_.kind = PrimitiveKind::SyncData; request_.token.step = 2; request_.bytes = {}; request_.offset = 0;\n\t\t\t\treturn true;",
        "done_ = true;\n\t\t\t\treturn true;",
        "journal_shared_store_test", ["1", "tasks"]),
    # A shared append that does not wait for its barrier.
    "no-barrier-wait": ("include/journal/store.h",
        "if (shared()) { co_await journal_->turns()->durable(journal_->group(), sequence, std::move(token)); }",
        "if (shared()) { co_return; }",
        "journal_shared_store_test", ["1", "tasks"]),
    # The entry returns once the task finishes, before committing what it staged.
    "entry-skips-commit": ("include/journal/blocking.h",
        "\t\tif (!turns.idle()) {\n\t\t\ttry { turns.commit(); }",
        "\t\tif (!result.finished && !turns.idle()) {\n\t\t\ttry { turns.commit(); }",
        "journal_driver_test", ["entry_staged_cancelled"]),
    # The entry sleeps with no deadline armed: a lost wake hangs instead of throwing.
    "entry-waits-without-deadline": ("include/journal/blocking.h",
        "scheduler.drive([&] { return result.finished || !turns.idle(); });",
        "scheduler.drive([&] { return result.finished || !turns.idle(); }, [] { return true; });",
        "journal_driver_test", ["entry_live_token"]),
    # stalled names no group.
    "stalled-without-groups": ("include/journal/blocking.h",
        "throw stalled(turns.waiting_groups());", "throw stalled({});",
        "journal_driver_test", ["entry_lost_wake"]),
    # The barrier resumes a waiter whose claim the cancel won: a second resume.
    "wake-ignores-claim": ("include/journal/turn_driver.h",
        "if (waiter->claim(detail::TurnWaiter::completed)) { waiter->home.post(waiter->handle); }",
        "waiter->claim(detail::TurnWaiter::completed); waiter->home.post(waiter->handle);",
        "journal_driver_test", ["cancel_races_barrier"]),
    # The cancel callback unlinks on the requesting thread instead of hopping to the loop (TSan).
    "cancel-without-hop": ("include/journal/turn_driver.h",
        "if (!waiter->home.post_call(&hop, waiter)) { hop(waiter); }", "hop(waiter);",
        "journal_driver_test", ["cancel_hops"]),
    # The table's lost ids never reach the logs: a lost id appends again once the damage is gone.
    "remap-forget": ("include/journal/host.h",
        "if (entry.state == GroupState::lost) { loop.lost.insert(id); continue; }", "if (false) { loop.lost.insert(id); continue; }",
        "journal_placement_test", ["repartition_lost"]),
    # A re-partition that does not mark recovery's lost ids before it writes the new generation.
    "repartition-skips-mark": ("include/journal/host.h",
        "\t\tmark_lost(lost);\n\t\tconst auto next = generation_ + 1;", "\t\tconst auto next = generation_ + 1;",
        "journal_placement_test", ["repartition_damaged"]),
    # An open that does not mark the lost ids: retention then forgets them.
    "open-skips-mark": ("include/journal/host.h",
        "\t\tfor (const auto& turns : drivers_) { lost.insert(turns->recovered().lost.begin(), turns->recovered().lost.end()); }\n\t\tmark_lost(lost);",
        "\t\tfor (const auto& turns : drivers_) { lost.insert(turns->recovered().lost.begin(), turns->recovered().lost.end()); }",
        "journal_placement_test", ["retention"]),
    # Artifact names that drop the group's id: two groups collide.
    "group-blind-names": ("include/journal/host.h",
        'inline std::string group_prefix(std::uint64_t group) { return "g" + hex(group, 16) + "-"; }',
        'inline std::string group_prefix(std::uint64_t) { return "g-"; }',
        "journal_host_test", ["reclaim_isolation"]),
    # A group's census and reclamation list the directory instead of probing its names.
    "group-scans": ("include/journal/host.h",
        "\t\t\tstd::uint64_t incarnation, serial, high;\n",
        "\t\t\tif (group) { return io->scan_directory(); }\n\t\t\tstd::uint64_t incarnation, serial, high;\n",
        "journal_host_test", ["census"]),
    # A closed group recovered from its loop's log, not the table.
    "reopen-from-log": ("include/journal/journal.h",
        "\t\t\tif (!turns_->has_group(group_)) {\n\t\t\t\tif (turns_->holds(group_))",
        "\t\t\tif (false) {\n\t\t\t\tif (turns_->holds(group_))",
        "journal_host_test", ["closed_reopen"]),
    # A group created again keeps its first incarnation's name space.
    "same-incarnation": ("include/journal/host.h",
        "entry = GroupEntry{entry.incarnation + 1, GroupState::open,", "entry = GroupEntry{1, GroupState::open,",
        "journal_host_test", ["recreate"]),
    # A damaged log keeps quiet about an open group it holds nothing of: create takes its id (F1).
    "damaged-keeps-absent": ("include/journal/turn_log.h",
        "if (damaged) { for (auto group : known.open)", "if (false) { for (auto group : known.open)",
        "journal_placement_test", ["damaged_absent"]),
    # The log forgets a closing group before its table write is durable (F2).
    "close-forgets-first": ("include/journal/host.h",
        "\t\tupdate(group, [&](GroupEntry& entry) {\n\t\t\tsettle_mark",
        "\t\tdriver_of(group).forget(group);\n\t\tupdate(group, [&](GroupEntry& entry) {\n\t\t\tsettle_mark",
        "journal_host_test", ["failed_close"]),
    # A failed table write keeps its change in memory (F4).
    "update-keeps-change": ("include/journal/host.h",
        "catch (...) { if (before) { entry = std::move(*before); } else { shard.erase(group); } throw; }", "catch (...) { throw; }",
        "journal_host_test", ["failed_close"]),
    # A failed close restores the group to open, not in doubt: a live recovery appends past a
    # close the disk may hold.
    "close-restores-open": ("include/journal/host.h",
        "} catch (...) { shards_[host_detail::slot(group)].at(group).in_doubt = true; throw; }", "} catch (...) { throw; }",
        "journal_host_test", ["close_in_doubt"]),
    # The host holds a group whose close is in doubt.
    "hold-ignores-doubt": ("include/journal/host.h",
        "if (const auto* found = resident(group); found && found->in_doubt) {", "if (const auto* found = resident(group); found && false) {",
        "journal_host_test", ["close_in_doubt"]),
    # A second recovery on one host takes the table's stale checkpoint (F3).
    "recover-twice-from-table": ("include/journal/journal.h",
        "if (turns_->holds(group_)) { throw", "if (false) { throw",
        "journal_host_test", ["second_recovery"]),
}

def main():
    name, flags = sys.argv[1], sys.argv[2:]
    path, old, new, target, args = MUTANTS[name]
    text = open(path).read()
    if text.count(old) != 1:
        print(f"mutant {name}: the original text occurs {text.count(old)} times"); return 2
    open(path, "w").write(text.replace(old, new))
    build = f".scratch/m-{name}"
    os.makedirs(".scratch", exist_ok=True)
    with open(build + ".log", "w") as log:
        subprocess.run(["cmake", "-S", ".", "-B", build, "-DCMAKE_BUILD_TYPE=Debug", *flags], check=True, stdout=log, stderr=log)
        subprocess.run(["cmake", "--build", build, "--target", target, "-j4"], check=True, stdout=log, stderr=log)
    try:
        result = subprocess.run([f"{build}/{target}", *args], timeout=60, capture_output=True, text=True)
        print(result.stdout[-2000:], result.stderr[-2000:])
        killed = result.returncode != 0
    except subprocess.TimeoutExpired:
        print("timed out: the test hangs"); killed = True
    print("KILLED" if killed else "SURVIVED")
    return 0 if killed else 1

sys.exit(main())
