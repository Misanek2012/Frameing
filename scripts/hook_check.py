#!/usr/bin/env python3
"""Verify every Geode $modify hook in src/ will actually hook on Windows.

Geode silently treats a $modify method whose signature doesn't exactly match a
bound function as a brand-new method: no compile error, the hook never fires.
This script checks every method in every $modify class against the codegen
data (CodegenData.json) generated for the target platform:

  * a method whose name matches a bound function on the class (or its bases)
    must match one binding's argument types exactly and its return type, and
  * that binding must be hookable on the platform (a real address, or a
    cocos2d DLL export: "link"), not "inline" / "missing".

Methods whose names match nothing in the bindings are reported as helpers.

Usage:
  hook_check.py <CodegenData.json> <src-dir> [--platform win]
  hook_check.py --self-test <CodegenData.json>
"""
from __future__ import annotations

import json
import re
import sys
from pathlib import Path

BUILTINS = {"int", "float", "double", "bool", "char", "short", "long", "unsigned", "signed", "void"}
ALIASES = {"_ccColor4F": "ccColor4F", "_ccColor3B": "ccColor3B", "_ccColor4B": "ccColor4B"}


def norm_type(t: str) -> str:
    t = re.sub(r"\b(cocos2d|geode|gd)::(?=\w)", lambda m: "" if m.group(1) != "gd" else "gd::", t)
    t = re.sub(r"\bcocos2d::", "", t)
    for a, b in ALIASES.items():
        t = re.sub(rf"\b{a}\b", b, t)
    t = " ".join(t.split())
    # "const T&" / "const T*" -> "T const&" / "T const*"
    m = re.fullmatch(r"const\s+(.+?)\s*([*&]+)", t)
    if m:
        t = f"{m.group(1)} const{m.group(2)}"
    t = re.sub(r"\s*([*&])", r"\1", t)
    t = re.sub(r"const([*&])", r"const\1", t)
    return t.strip()


def split_params(s: str) -> list[str]:
    out, depth, cur = [], 0, ""
    for ch in s:
        if ch in "<([{":
            depth += 1
        elif ch in ">)]}":
            depth -= 1
        if ch == "," and depth == 0:
            out.append(cur)
            cur = ""
        else:
            cur += ch
    if cur.strip():
        out.append(cur)
    return [p.strip() for p in out if p.strip() and p.strip() != "void"]


def param_type(p: str) -> str:
    p = p.split("=")[0].strip()
    m = re.fullmatch(r"(.*?[\s*&])([A-Za-z_]\w*)", p)
    if m and m.group(1).strip() and m.group(2) not in BUILTINS:
        p = m.group(1)
    return norm_type(p)


def strip_comments(src: str) -> str:
    src = re.sub(r"//[^\n]*", "", src)
    src = re.sub(r"/\*.*?\*/", "", src, flags=re.S)
    return src


def find_block(src: str, open_idx: int) -> int:
    """Return index just past the brace block starting at src[open_idx] == '{'."""
    depth, i, n = 0, open_idx, len(src)
    while i < n:
        c = src[i]
        if c in "\"'":
            q = c
            i += 1
            while i < n and src[i] != q:
                i += 2 if src[i] == "\\" else 1
        elif c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                return i + 1
        i += 1
    raise ValueError("unbalanced braces")


MODIFY_RE = re.compile(r"(?:class|struct)\s+\$modify\s*\(\s*(\w+)\s*,\s*([\w:]+)\s*\)\s*\{")
METHOD_RE = re.compile(
    r"(?:^|[;}{])\s*((?:static\s+|virtual\s+|inline\s+)*)"
    r"([\w:<>]+(?:\s*[*&]+)?(?:\s+const)?\s*[*&]*)\s+(\w+)\s*\(([^()]*(?:\([^()]*\)[^()]*)*)\)\s*(?:const\s*)?(?:override\s*)?\{",
    re.M,
)


def modify_methods(src: str):
    """Yield (modify_name, target_class, return_type, name, [param types], is_static)."""
    src = strip_comments(src)
    for m in MODIFY_RE.finditer(src):
        start = m.end() - 1
        end = find_block(src, start)
        body = src[start + 1 : end - 1]
        # only consider top-level members of the $modify class
        i, depth, top = 0, 0, []
        while i < len(body):
            c = body[i]
            if c == "{":
                if depth == 0:
                    top.append(i)
                depth += 1
            elif c == "}":
                depth -= 1
            i += 1
        for mm in METHOD_RE.finditer(body):
            brace = mm.end() - 1
            # skip anything nested inside another member's body
            if _depth_at(body, brace) != 0:
                continue
            quals, ret, name, params = mm.group(1), mm.group(2), mm.group(3), mm.group(4)
            if name in ("if", "for", "while", "switch", "return"):
                continue
            yield (m.group(1), m.group(2), norm_type(ret), name,
                   [param_type(p) for p in split_params(params)], "static" in quals)


def _depth_at(s: str, idx: int) -> int:
    d = 0
    for c in s[:idx]:
        if c == "{":
            d += 1
        elif c == "}":
            d -= 1
    return d


class Bindings:
    def __init__(self, data: dict, platform: str):
        self.platform = platform
        self.classes = {c["name"]: c for c in data["classes"]}

    def resolve_class(self, name: str):
        for cand in (name, f"cocos2d::{name}", f"cocos2d::extension::{name}"):
            if cand in self.classes:
                return self.classes[cand]
        return None

    def candidates(self, cls: dict, fname: str, seen=None):
        seen = seen or set()
        if cls["name"] in seen:
            return []
        seen.add(cls["name"])
        found = [(cls["name"], f) for f in cls["functions"] if f["name"] == fname]
        if found:
            return found
        for b in cls.get("bases", []):
            bc = self.resolve_class(b)
            if bc:
                r = self.candidates(bc, fname, seen)
                if r:
                    return r
        return []

    def hookable(self, f: dict) -> bool:
        b = f["bindings"].get(self.platform)
        return isinstance(b, int) or b == "link"


def check(bind: Bindings, items) -> tuple[list[str], list[str], list[str]]:
    ok, errors, helpers = [], [], []
    for mod, target, ret, name, params, is_static in items:
        if name == "onModify" and is_static:
            continue
        cls = bind.resolve_class(target)
        if cls is None:
            errors.append(f"{mod}: unknown class {target}")
            continue
        cands = bind.candidates(cls, name)
        sig = f"{ret} {target}::{name}({', '.join(params)})"
        if not cands:
            helpers.append(f"{mod}: {sig} (no binding with this name: helper, not a hook)")
            continue
        exact = [(owner, f) for owner, f in cands
                 if [norm_type(a["type"]) for a in f["args"]] == params]
        if not exact:
            avail = "; ".join(
                f"{norm_type(f['return'])} {owner}::{name}({', '.join(norm_type(a['type']) for a in f['args'])})"
                for owner, f in cands)
            errors.append(f"{mod}: {sig} matches no binding, so it would silently NOT hook. Bound: {avail}")
            continue
        hookable = [(o, f) for o, f in exact if bind.hookable(f)]
        if not hookable:
            states = ", ".join(str(f["bindings"].get(bind.platform)) for _, f in exact)
            errors.append(f"{mod}: {sig} is not hookable on {bind.platform} ({states})")
            continue
        owner, f = hookable[0]
        if norm_type(f["return"]) != ret:
            errors.append(f"{mod}: {sig} returns {ret} but binding returns {norm_type(f['return'])}")
            continue
        b = f["bindings"][bind.platform]
        where = hex(b) if isinstance(b, int) else b
        ok.append(f"{mod}: {sig} -> {owner}::{name} @ {bind.platform} {where}")
    return ok, errors, helpers


SELF_TEST = r'''
class $modify(GoodA, PlayLayer) {
    static void onModify(auto& self) {}
    void postUpdate(float dt) { PlayLayer::postUpdate(dt); }
    void myHelper(int x) {}
};
class $modify(BadArgs, PlayLayer) {
    void postUpdate(double dt) {}
};
class $modify(BadRet, GJBaseGameLayer) {
    float getModifiedDelta(float dt) { return 0; }
};
class $modify(GoodCocos, CCDrawNode) {
    bool drawPolygon(CCPoint* v, unsigned int n, const ccColor4F& f, float w, cocos2d::_ccColor4F const& b, BorderAlignment a) { return true; }
};
class $modify(Inherited, PlayLayer) {
    double getModifiedDelta(float dt) { return 0; }
};
'''


def self_test(data_path: str) -> int:
    bind = Bindings(json.loads(Path(data_path).read_text()), "win")
    ok, errors, helpers = check(bind, list(modify_methods(SELF_TEST)))
    want_ok = {"GoodA", "GoodCocos", "Inherited"}
    want_err = {"BadArgs", "BadRet"}
    got_ok = {s.split(":")[0] for s in ok}
    got_err = {s.split(":")[0] for s in errors}
    got_help = {s.split(":")[0] for s in helpers}
    passed = got_ok == want_ok and got_err == want_err and got_help == {"GoodA"}
    for line in ok + errors + helpers:
        print("  ", line)
    print("self-test", "passed" if passed else f"FAILED ok={got_ok} err={got_err} help={got_help}")
    return 0 if passed else 1


def main(argv: list[str]) -> int:
    if len(argv) >= 3 and argv[1] == "--self-test":
        return self_test(argv[2])
    if len(argv) < 3:
        print(__doc__)
        return 2
    platform = argv[argv.index("--platform") + 1] if "--platform" in argv else "win"
    bind = Bindings(json.loads(Path(argv[1]).read_text()), platform)
    items = []
    for f in sorted(Path(argv[2]).rglob("*.cpp")) + sorted(Path(argv[2]).rglob("*.hpp")):
        items += list(modify_methods(f.read_text()))
    if not items:
        print("error: found no $modify methods (parser broken?)")
        return 1
    ok, errors, helpers = check(bind, items)
    for line in ok:
        print("hook   ", line)
    for line in helpers:
        print("helper ", line)
    for line in errors:
        print("ERROR  ", line)
    print(f"{len(ok)} hooks verified for {platform}, {len(helpers)} helpers, {len(errors)} errors")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
