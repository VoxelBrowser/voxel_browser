#!/usr/bin/env python3
"""Generate the Lua quick reference from the LuaCATS stubs in sdk/lua/library/.

The stubs are the single source of truth (architecture_spec/dev-experience.md
section 3.1). This script writes:

  sdk/lua/api_index.txt        sorted dotted names of every documented member
  docs/lua-reference/README.md cheat sheet: one line per function
  docs/lua-reference/<group>.md per-table reference (signature, params, example)
  docs/lua-reference/events.md  vb.on event table
  docs/lua-reference/types.md   every ---@class / ---@alias
  src/server/check_pack_data.inc what `--check-pack` knows about the API: each member's
                               `---@vb since` and the keys of every ---@class (definition tables)

It also enforces the stub conventions: every function has a one-line summary, a
---@param for each argument, a ---@vb since <version> and at least one ```lua example.

Usage: scripts/gen_lua_docs.py [--check]
  --check  regenerate into memory and fail (exit 1) if the committed output differs.
Python 3, standard library only.
"""
import argparse
import re
import sys
from collections import OrderedDict
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
LIB = ROOT / "sdk" / "lua" / "library"
INDEX = ROOT / "sdk" / "lua" / "api_index.txt"
OUT = ROOT / "docs" / "lua-reference"
CHECK_DATA = ROOT / "src" / "server" / "check_pack_data.inc"

FUNC_RE = re.compile(r"^function\s+([A-Za-z_][\w.]*)([.:])([A-Za-z_]\w*)\s*\((.*)\)\s*end\s*$")
ASSIGN_RE = re.compile(r"^((?:vb|ui|client)(?:\.[A-Za-z_]\w*)+)\s*=")


class Entry:
    def __init__(self, name, group, sep, params):
        self.name = name  # dotted/colon name, e.g. vb.world.get_block or Player:get_pos
        self.group = group
        self.sep = sep
        self.arg_names = params
        self.context = ""
        self.since = ""
        self.summary = []
        self.prose = []
        self.params = []  # (name, type, desc)
        self.returns = []  # (type, desc)
        self.examples = []
        self.overloads = []
        self.is_member = False
        self.file = ""
        self.line = 0


def split_args(s):
    return [a.strip() for a in s.split(",") if a.strip()]


def parse_doc(doc, entry):
    """Fill entry from a list of raw doc lines (without the leading ---)."""
    in_code = False
    code = []
    text = []
    for raw in doc:
        line = raw[1:] if raw.startswith(" ") else raw
        if in_code:
            if line.strip().startswith("```"):
                entry.examples.append("\n".join(code))
                code = []
                in_code = False
            else:
                code.append(line)
            continue
        if line.strip().startswith("```lua"):
            in_code = True
            continue
        m = re.match(r"@vb\s+(\w+)\s*(.*)", line)
        if m:
            if m.group(1) == "context":
                entry.context = m.group(2).strip()
            elif m.group(1) == "since":
                entry.since = m.group(2).strip()
            elif m.group(1) == "member":
                entry.is_member = True
            continue
        m = re.match(r"@param\s+(\w+\??)\s+(\S+)\s*(.*)", line)
        if m:
            entry.params.append((m.group(1), m.group(2), m.group(3)))
            continue
        m = re.match(r"@return\s+(\S+)\s*(.*)", line)
        if m:
            entry.returns.append((m.group(1), m.group(2)))
            continue
        m = re.match(r"@overload\s+(.*)", line)
        if m:
            entry.overloads.append(m.group(1))
            continue
        if line.startswith("@"):
            continue
        text.append(line)
    # text lines: first paragraph is the summary.
    para = []
    for t in text:
        if t.strip() == "" and para:
            break
        if t.strip():
            para.append(t.strip())
    entry.summary = para
    entry.prose = [t for t in text[len(para):] if True]
    return in_code


def parse_library():
    entries = OrderedDict()
    classes = []  # (kind, name, doc lines, fields)
    errors = []
    for path in sorted(LIB.glob("*.lua")):
        lines = path.read_text(encoding="utf-8").split("\n")
        doc = []
        for i, raw in enumerate(lines, 1):
            stripped = raw.rstrip()
            if stripped.startswith("---"):
                doc.append(stripped[3:])
                continue
            m = FUNC_RE.match(stripped)
            a = ASSIGN_RE.match(stripped)
            if m:
                base, sep, member, args = m.groups()
                full = base + sep + member
                e = Entry(full, base, sep, split_args(args))
                e.file, e.line = path.name, i
                parse_doc(doc, e)
                if full in entries:
                    # A second declaration may only add overloads/examples.
                    prev = entries[full]
                    prev.overloads += e.overloads
                else:
                    entries[full] = e
            elif a and any(d.strip().startswith("@vb member") for d in doc):
                full = a.group(1)
                e = Entry(full, full.rsplit(".", 1)[0], ".", [])
                e.file, e.line = path.name, i
                parse_doc(doc, e)
                entries[full] = e
            if doc:
                # Collect class/alias declarations from this doc block.
                collect_types(doc, classes, path.name)
            doc = []
    return entries, classes, errors


def collect_types(doc, classes, fname):
    cur = None
    for raw in doc:
        line = raw[1:] if raw.startswith(" ") else raw
        m = re.match(r"@class\s+(\S+)", line)
        if m:
            cur = {"kind": "class", "name": m.group(1), "desc": [], "fields": [], "file": fname}
            classes.append(cur)
            continue
        m = re.match(r"@alias\s+(\S+)\s*(.*)", line)
        if m:
            cur = None
            classes.append({"kind": "alias", "name": m.group(1), "desc": [m.group(2)] if m.group(2) else [],
                            "fields": [], "file": fname})
            classes[-1]["variants"] = []
            continue
        m = re.match(r"\|\s*(\S.*)", line)
        if m and classes and classes[-1]["kind"] == "alias":
            classes[-1]["variants"].append(m.group(1))
            continue
        m = re.match(r"@field\s+(\[?\"?[\w\"\]]+\??)\s+(\S+)\s*(.*)", line)
        if m and cur is not None:
            cur["fields"].append((m.group(1).replace('["', "").replace('"]', ""), m.group(2), m.group(3)))
            continue
        if cur is not None and not line.startswith("@") and line.strip():
            cur["desc"].append(line.strip())


def validate(entries):
    errs = []
    for e in entries.values():
        if e.is_member:
            if not e.summary:
                errs.append(f"{e.file}:{e.line}: {e.name}: missing summary")
            if not e.examples:
                errs.append(f"{e.file}:{e.line}: {e.name}: missing ```lua example")
            continue
        loc = f"{e.file}:{e.line}: {e.name}"
        if not e.summary:
            errs.append(f"{loc}: missing summary")
        if not e.examples:
            errs.append(f"{loc}: missing ```lua example")
        if not e.context:
            errs.append(f"{loc}: missing ---@vb context")
        if not re.fullmatch(r"\d+\.\d+\.\d+", e.since):
            errs.append(f"{loc}: missing or malformed ---@vb since <x.y.z> (the first release that has it)")
        documented = [p[0].rstrip("?") for p in e.params]
        for a in e.arg_names:
            if a not in documented:
                errs.append(f"{loc}: argument '{a}' has no ---@param")
        for d in documented:
            if d not in e.arg_names:
                errs.append(f"{loc}: ---@param '{d}' is not an argument")
    return errs


def sig(e):
    args = ", ".join(e.arg_names)
    ret = ""
    if e.returns:
        ret = " -> " + ", ".join(r[0] for r in e.returns)
    if e.is_member:
        return e.name
    return f"{e.name}({args}){ret}"


def anchor(name):
    return re.sub(r"[^a-z0-9]+", "-", name.lower()).strip("-")


def group_of(e):
    if e.sep == ":":
        return e.group  # Player / Entity
    return e.group if e.group != "vb" else "vb"


def fence(s, lang="lua"):
    return "```" + lang + "\n" + s + "\n```"


def render(entries, classes):
    files = {}
    groups = OrderedDict()
    for e in entries.values():
        groups.setdefault(group_of(e), []).append(e)

    # Cheat sheet.
    cs = ["# Lua API quick reference", "",
          "<!-- Generated by scripts/gen_lua_docs.py from sdk/lua/library/*.lua. Do not edit. -->", "",
          "One line per function. Click a name for its parameters and example. "
          "Narrative/design notes: [`../lua-api.md`](../lua-api.md). "
          "Files in `ui/` run in the client UI VM (`ui`, `client`); every other pack file runs in the server VM (`vb`).",
          ""]
    for g, es in groups.items():
        cs.append(f"## `{g}`")
        cs.append("")
        cs.append(f"[Full reference]({g}.md)")
        cs.append("")
        for e in es:
            cs.append(f"- [`{sig(e)}`]({g}.md#{anchor(e.name)}) — {' '.join(e.summary)}")
        cs.append("")
    cs.append("## Other pages")
    cs.append("")
    cs.append("- [`events.md`](events.md) — every `vb.on` event and its handler signature")
    cs.append("- [`types.md`](types.md) — definition tables (`BlockDef`, ...) and other types")
    cs.append("")
    files["README.md"] = "\n".join(cs)

    for g, es in groups.items():
        out = [f"# `{g}`", "",
               "<!-- Generated by scripts/gen_lua_docs.py from sdk/lua/library/*.lua. Do not edit. -->", ""]
        for e in es:
            out.append(f"## {e.name}")
            out.append("")
            out.append(f"`{sig(e)}`")
            out.append("")
            badges = []
            if e.context:
                badges.append(f"context: **{e.context}**")
            if e.since:
                badges.append(f"since: **{e.since}**")
            if badges:
                out.append(" · ".join(badges))
                out.append("")
            out.append(" ".join(e.summary))
            out.append("")
            extra = [t.strip() for t in e.prose if t.strip()]
            if extra:
                out.append(" ".join(extra))
                out.append("")
            if e.params:
                out.append("| Parameter | Type | Description |")
                out.append("| --- | --- | --- |")
                for n, t, d in e.params:
                    out.append(f"| `{n}` | `{t}` | {d} |")
                out.append("")
            if e.returns:
                for t, d in e.returns:
                    out.append(f"Returns `{t}`{(' — ' + d) if d else ''}")
                out.append("")
            for ex in e.examples:
                out.append(fence(ex))
                out.append("")
        files[f"{g}.md"] = "\n".join(out)

    # events.md
    on = entries.get("vb.on")
    ev = ["# Events (`vb.on`)", "",
          "<!-- Generated by scripts/gen_lua_docs.py from sdk/lua/library/*.lua. Do not edit. -->", "",
          "`vb.on(event, handler)` — handler signatures per event. Returning `false` vetoes where the "
          "event is vetoable (`player_join`, `block_break`, `block_place`, `chat`, `block_break_begin`, "
          "`player_input`).", "",
          "| Event | Handler |", "| --- | --- |"]
    if on:
        for o in on.overloads:
            m = re.match(r'fun\(event: "(\w+)", handler: (.*)\)$', o)
            if m:
                ev.append(f"| `{m.group(1)}` | `{m.group(2)}` |")
    ev.append("")
    files["events.md"] = "\n".join(ev)

    # types.md
    ty = ["# Types", "",
          "<!-- Generated by scripts/gen_lua_docs.py from sdk/lua/library/*.lua. Do not edit. -->", ""]
    for c in sorted(classes, key=lambda c: c["name"]):
        if c["name"] in groups or c["name"].startswith("vb.") or c["name"] in ("vb", "ui", "client"):
            continue
        ty.append(f"## {c['name']}")
        ty.append("")
        if c["desc"]:
            ty.append(" ".join(c["desc"]))
            ty.append("")
        if c["kind"] == "alias" and c.get("variants"):
            for v in c["variants"]:
                ty.append(f"- `{v}`")
            ty.append("")
        if c["fields"]:
            ty.append("| Field | Type | Description |")
            ty.append("| --- | --- | --- |")
            for n, t, d in c["fields"]:
                ty.append(f"| `{n}` | `{t}` | {d} |")
            ty.append("")
    files["types.md"] = "\n".join(ty)

    index = "\n".join(sorted(entries.keys())) + "\n"
    return files, index


def render_check_data(entries, classes):
    """C++ tables for src/server/check_pack.cpp (an .inc so the clang-format lint skips it)."""
    out = ["// Generated by scripts/gen_lua_docs.py from sdk/lua/library -- do not edit.",
           "// The first release with each API member (---@vb since), for --check-pack's",
           "// \"newer than your engine_version_req\" warning.",
           "static const ApiSince kApiSince[] = {"]
    for name in sorted(entries):
        out.append(f'\t{{ "{name}", "{entries[name].since}" }},')
    out.append("};")
    out.append("")
    out.append("// Keys of every ---@class (space-separated), for the unknown-key warning on definition tables.")
    out.append("static const ClassKeys kClassKeys[] = {")
    seen = set()
    for c in classes:
        if c["kind"] != "class" or c["name"] in seen:
            continue
        seen.add(c["name"])
        keys = [f[0].rstrip("?") for f in c["fields"] if not f[0].startswith("[")]
        out.append(f'\t{{ "{c["name"]}", "{" ".join(keys)}" }},')
    out.append("};")
    return "\n".join(out) + "\n"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true")
    args = ap.parse_args()
    entries, classes, _ = parse_library()
    errs = validate(entries)
    if errs:
        print("\n".join(errs), file=sys.stderr)
        print(f"error: {len(errs)} stub convention violation(s)", file=sys.stderr)
        return 1
    files, index = render(entries, classes)
    files = {k: v.rstrip("\n") + "\n" for k, v in files.items()}
    check_data = render_check_data(entries, classes)
    if args.check:
        bad = []
        if not INDEX.exists() or INDEX.read_text(encoding="utf-8") != index:
            bad.append(str(INDEX.relative_to(ROOT)))
        if not CHECK_DATA.exists() or CHECK_DATA.read_text(encoding="utf-8") != check_data:
            bad.append(str(CHECK_DATA.relative_to(ROOT)))
        for name, text in files.items():
            p = OUT / name
            if not p.exists() or p.read_text(encoding="utf-8") != text:
                bad.append(str(p.relative_to(ROOT)))
        existing = {p.name for p in OUT.glob("*.md")} if OUT.exists() else set()
        for stale in sorted(existing - set(files)):
            bad.append(f"docs/lua-reference/{stale} (stale)")
        if bad:
            print("error: generated Lua docs are out of date:\n  " + "\n  ".join(bad), file=sys.stderr)
            print("hint: run scripts/gen_lua_docs.py and commit the result", file=sys.stderr)
            return 1
        print(f"ok: {len(entries)} documented members, docs up to date")
        return 0
    OUT.mkdir(parents=True, exist_ok=True)
    for stale in OUT.glob("*.md"):
        if stale.name not in files:
            stale.unlink()
    for name, text in files.items():
        (OUT / name).write_text(text, encoding="utf-8")
    INDEX.write_text(index, encoding="utf-8")
    CHECK_DATA.write_text(check_data, encoding="utf-8")
    print(f"wrote {len(entries)} members to {INDEX.relative_to(ROOT)} and {len(files)} pages to docs/lua-reference/")
    return 0


if __name__ == "__main__":
    sys.exit(main())
