#!/usr/bin/env python3
"""Every setting the code reads must exist in mod.json with a matching type
(a missing key fails silently in-game and returns a default value)."""
import json, re, sys
from pathlib import Path

root = Path(__file__).resolve().parent.parent
settings = json.loads((root / "mod.json").read_text())["settings"]
want = {"flag": "bool", "integer": "int", "real": "float", "rgba": "rgba"}
bad = 0
seen = set()
for f in sorted((root / "src").rglob("*.[ch]pp")):
    text = f.read_text()
    for m in re.finditer(r'cfg::(flag|integer|real|rgba)\(\s*"([^"]+)"', text):
        kind, key = m.groups()
        seen.add(key)
        s = settings.get(key)
        if s is None:
            print(f"ERROR {f.relative_to(root)}: setting '{key}' is not in mod.json"); bad += 1
        elif s["type"] != want[kind]:
            print(f"ERROR {f.relative_to(root)}: '{key}' read as {want[kind]} but mod.json says {s['type']}"); bad += 1
    # keys built at runtime, e.g. "hb-" + name
    for m in re.finditer(r'std::string\("([a-z-]+)"\)\s*\+', text):
        seen.add(m.group(1) + "*")
dynamic = [k[:-1] for k in seen if k.endswith("*")]
unused = [k for k, v in settings.items() if v["type"] != "title" and k not in seen
          and not any(k.startswith(d) for d in dynamic) and not k.startswith("sound-")]
for k in unused: print(f"note: setting '{k}' is never read")
print(f"{len([k for k in seen if not k.endswith('*')])} settings read, {bad} errors")
sys.exit(1 if bad else 0)
