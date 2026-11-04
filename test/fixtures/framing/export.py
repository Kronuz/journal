#!/usr/bin/env python3
"""Exports the four logs of strata's tail model (bench/manifest-log/tail_model.py at
strata:1c8ff50) as this directory's fixtures: 512-byte blocks, magic STRLOG01, identity bytes
1..16, segment 7, epoch 1, no block manifests; a snapshot then edits.

Usage: export.py STRATA/bench/manifest-log/tail_model.py OUTPUT-DIR
"""
import importlib.util
import os
import sys

spec = importlib.util.spec_from_file_location("tail_model", sys.argv[1])
model = importlib.util.module_from_spec(spec)
spec.loader.exec_module(model)
names = {"A (the review's)": "log-a", "B (block ends, padding)": "log-b", "C (multi-block edits)": "log-c",
         "D (random 20-400)": "log-d"}
for name, (log, ends) in model.logs().items():
    with open(os.path.join(sys.argv[2], names[name]), "wb") as out:
        out.write(log)
    with open(os.path.join(sys.argv[2], names[name] + ".ends"), "w") as out:
        out.write(" ".join(map(str, ends)) + "\n")
