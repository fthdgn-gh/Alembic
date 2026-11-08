#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# The engine payload's process checks beside the Probe Bench app's (a game
# process): what differs between the two on the console.
#   tools/probe/compare.py CONSOLE_IP [--fresh] [--all]
#   tools/probe/compare.py ENGINE.json APP.json [--all]
# CONSOLE_IP: reads GET /api/probe/app (the bench's last report) and the
# engine's GET /api/probe (?io=0 unless --fresh, which runs every check again).
# --all also prints the rows that match.
import json, sys, urllib.request


def flatten(v, prefix="", out=None):
    out = {} if out is None else out
    if isinstance(v, dict):
        for k, f in v.items():
            flatten(f, f"{prefix}.{k}" if prefix else k, out)
    elif isinstance(v, float):
        out[prefix] = f"{v:.2f}"
    elif isinstance(v, bool):
        out[prefix] = "yes" if v else "no"
    else:
        out[prefix] = str(v)
    return out


def fetch(url, timeout=300):
    with urllib.request.urlopen(url, timeout=timeout) as r:
        return json.load(r)


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    every = "--all" in sys.argv
    if len(args) == 2:
        engine, app = (json.load(open(p)) for p in args)
    elif len(args) == 1:
        base = f"http://{args[0]}:5940"
        app = fetch(base + "/api/probe/app", 10)
        engine = fetch(base + "/api/probe" + ("" if "--fresh" in sys.argv else "?io=0"))
    else:
        sys.exit(__doc__ or "usage: compare.py CONSOLE_IP | ENGINE.json APP.json")
    e, a = flatten(engine), flatten(app)
    keys = list(a) + [k for k in e if k not in a]
    width = max(len(k) for k in keys)
    print(f"{'check':<{width}}  {'engine (payload)':<34}  app (game process)")
    for k in keys:
        ev, av = e.get(k, "-"), a.get(k, "-")
        if ev == av and not every:
            continue
        mark = " " if ev == av else "*"
        print(f"{k:<{width}}{mark} {ev:<34}  {av}")


if __name__ == "__main__":
    main()
