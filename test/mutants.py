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
        "journal_driver_test", ["entry_threads_busy"]),
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
