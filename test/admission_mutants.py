#!/usr/bin/env python3
# Applies one named mutant of the recovery claim to include/journal/admission.h in this checkout,
# builds journal_admission_test and runs it, exiting with its status: a killed mutant exits nonzero
# (a fails-without job), 126 when it cannot be built. `admission_mutants.py NAME [ARGS]`, `--list`.
import os
import pathlib
import subprocess
import sys

HEADER = "include/journal/admission.h"
MUTANTS = {
    # Refill caps the claim at replacement's available deficit, ignoring what it holds.
    "cap-ignores-held": (
        "claim = resources_min(claim, resources_subtract(limits.replacement_pool, resources_add(replacement_held, replacement_available)));",
        "claim = resources_min(claim, resources_subtract(limits.replacement_pool, replacement_available));",
    ),
    # The claim's take is not bounded by control's available bytes.
    "take-ignores-control": (
        "return resources_min(resources_min(claim, control_available),",
        "return resources_min(claim,",
    ),
    # Growth takes from the claim but the permit does not record it.
    "grow-unrecorded": (
        "permit.claimed_ = detail::resources_add(permit.claimed_, taken);",
        "",
    ),
    # What control lacks ignores control's held permits.
    "lacking-ignores-held": (
        "detail::resources_add(value.control_held, value.control_available)",
        "value.control_available",
    ),
    # The census claim is replacement's whole deficit, not min(R, T) - ra.
    "claim-is-deficit": (
        "detail::resources_min(limits.replacement_pool, funding)",
        "limits.replacement_pool",
    ),
}


def main() -> int:
    if sys.argv[1:] == ["--list"]:
        print(" ".join(MUTANTS))
        return 0
    name, args = sys.argv[1], sys.argv[2:]
    old, new = MUTANTS[name]
    path = pathlib.Path(HEADER)
    text = path.read_text(encoding="utf-8")
    if text.count(old) != 1:
        print(f"mutant {name}: the original text occurs {text.count(old)} times")
        return 126
    path.write_text(text.replace(old, new), encoding="utf-8")
    build = f".scratch/admission-{name}"
    os.makedirs(".scratch", exist_ok=True)
    with open(build + ".log", "w") as log:
        for step in (
            [
                "cmake",
                "-S",
                ".",
                "-B",
                build,
                "-G",
                "Ninja",
                "-DCMAKE_BUILD_TYPE=Debug",
            ],
            ["cmake", "--build", build, "--target", "journal_admission_test", "-j4"],
        ):
            if subprocess.run(step, stdout=log, stderr=log, check=False).returncode:
                print(f"mutant {name}: build failed, see {build}.log")
                return 126
    result = subprocess.run(
        [f"{build}/journal_admission_test", *args],
        timeout=120,
        check=False,
        capture_output=True,
        text=True,
    )
    print(result.stdout[-2000:], result.stderr[-2000:])
    print(f"mutant {name}: {'KILLED' if result.returncode else 'SURVIVED'}")
    return result.returncode


sys.exit(main())
