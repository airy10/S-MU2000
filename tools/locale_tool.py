#!/usr/bin/env python3
# license:BSD-3-Clause
"""The UI translations live in locale/<code>/*.json; this turns them into the
tables the program is built with.

  python tools/locale_tool.py gen       locale/ -> src/ui/texts_<code>.h and the three .inc lists
  python tools/locale_tool.py check     fail if `gen` would change anything (what CI runs)
  python tools/locale_tool.py export    src/ui/texts_<code>.h -> locale/ (to bring a header-only change back)
  python tools/locale_tool.py new <code> "<name>"
                                        start a language: an entry in locale/languages.json and
                                        a folder with every text still in English, ready to translate

What is where:

  locale/languages.json     the languages, in menu order: [{"code": "ja", "name": "日本語"}, ...]
  locale/<code>/*.json      {"key": "text", ...}. Lines starting with // are notes for translators.
                            A key that is missing falls back to English, so a translation can be partial.
  src/ui/texts.h            the list of keys (struct ui_texts) and their notes. Still written by hand:
                            a new text is a new member there plus its line in locale/en and locale/ja.

Generated, and committed so that building needs no Python:

  src/ui/texts_<code>.h     one table per language
  src/ui/lang_list.inc      UI_LANG_LIST, from languages.json
  src/ui/texts_tables.inc   the #include of every table
  src/ui/texts_fields.inc   every key by name, for the files read when the program starts

See locale/README.md for the translator's side of this.
"""
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
UI = ROOT / "src/ui"
LOCALE = ROOT / "locale"
TEXTS_H = UI / "texts.h"

# Which file a key goes to, by its prefix. Only for people: the program and the
# generator read every file of a language and do not care which one a key is in.
GROUPS = [
    ("settings", ["settings_"]),
    ("panel",    ["tab_", "hint_", "editor_", "effects_", "layout_", "engine_", "bootcache_", "status_", "help_", "audio_"]),
    ("menus",    ["bar_", "menu_", "dlg_", "note_"]),
    ("editor",   ["xgui_", "fxe_", "fxcat_", "fx_", "sys_", "ed_", "sxd_", "drum_", "cap_"]),
    ("voice",    ["ps_", "mx_"]),
    ("boards",   ["sb_", "fce_", "fme_"]),
    ("master",   ["me_"]),
    ("list",     ["ov_"]),
    ("player",   ["ply_"]),
    ("sampling", ["smp_", "lib_", "rw_", "pv_"]),
]
MISC = "misc"

ENTRY_RE = re.compile(r"\.(\w+)\s*=\s*((?:\"(?:[^\"\\]|\\.)*\"\s*)+)[,;]", re.S)
LITERAL_RE = re.compile(r"\"((?:[^\"\\]|\\.)*)\"")
SPEC_RE = re.compile(r"%(?:%%|[-+0 #'0-9]*(?:\.\d+)?[hljztL]*[diuoxXfFeEgGaAcspn])")
CODE_RE = re.compile(r"[a-z][a-z0-9_]*\Z")


def group_of(key):
    for name, prefixes in GROUPS:
        if any(key.startswith(p) for p in prefixes):
            return name
    return MISC


def c_unescape(s):
    simple = {"n": "\n", "t": "\t", "r": "\r", "\\": "\\", '"': '"', "'": "'"}
    out, i = [], 0
    while i < len(s):
        if s[i] == "\\" and i + 1 < len(s):
            if s[i + 1] not in simple:
                raise ValueError("escape \\%s is not supported in a text table" % s[i + 1])
            out.append(simple[s[i + 1]])
            i += 2
        else:
            out.append(s[i])
            i += 1
    return "".join(out)


def c_escape(s):
    out = []
    for ch in s:
        if ch == "\\":
            out.append("\\\\")
        elif ch == '"':
            out.append('\\"')
        elif ch == "\n":
            out.append("\\n")
        elif ch == "\t":
            out.append("\\t")
        elif ch == "\r":
            raise ValueError("carriage return inside a text")
        elif ord(ch) < 0x20:
            raise ValueError("control character U+%04X inside a text" % ord(ch))
        else:
            out.append(ch)
    # "??x" could be read as a trigraph by an old compiler mode; keep them apart
    return "".join(out).replace("??", "?\\?")


def struct_fields():
    """[(key, [notes])] in struct order. A note is the comment block above a key
    (kept for the keys that follow it, until the next block) or the comment after it."""
    text = TEXTS_H.read_text(encoding="utf-8")
    m = re.search(r"struct\s+ui_texts\s*\{(.*?)\n\};", text, re.S)
    if not m:
        sys.exit("FAIL: struct ui_texts not found in src/ui/texts.h")
    out, block, fresh = [], [], False
    for line in m.group(1).splitlines():
        s = line.strip()
        if s.startswith("//"):
            if not fresh:
                block, fresh = [], True
            block.append(s[2:].strip())
            continue
        f = re.match(r"const\s+char\s*\*\s*(\w+)\s*;\s*(?://\s*(.*))?$", s)
        if f:
            notes = list(block) if fresh else []
            if f.group(2):
                notes.append(f.group(2).strip())
            out.append((f.group(1), notes))
            fresh = False
    return out


def languages():
    path = LOCALE / "languages.json"
    langs = json.loads(path.read_text(encoding="utf-8"))
    for l in langs:
        if not CODE_RE.match(l.get("code", "")) or not l.get("name"):
            sys.exit("FAIL: locale/languages.json: every entry needs a code (lowercase letters) and a name: %r" % l)
    codes = [l["code"] for l in langs]
    if codes[:1] != ["ja"] or "en" not in codes:
        sys.exit("FAIL: locale/languages.json must start with ja and contain en (the tables fall back to them)")
    if len(set(codes)) != len(codes):
        sys.exit("FAIL: locale/languages.json lists a code twice")
    return langs


def read_jsonc(path):
    """A .json file whose lines may start with // (notes). Returns {key: text}."""
    lines = []
    for line in path.read_text(encoding="utf-8").splitlines():
        lines.append("" if line.lstrip().startswith("//") else line)      # keep the line numbers for error messages
    try:
        data = json.loads("\n".join(lines) or "{}")
    except json.JSONDecodeError as e:
        sys.exit("FAIL: %s: line %d: %s" % (path.relative_to(ROOT), e.lineno, e.msg))
    if not isinstance(data, dict) or not all(isinstance(v, str) for v in data.values()):
        sys.exit("FAIL: %s: must be one object of \"key\": \"text\" pairs" % path.relative_to(ROOT))
    return data


def read_locale(code):
    """{key: text} of one language, from every .json in its folder."""
    folder = LOCALE / code
    if not folder.is_dir():
        sys.exit("FAIL: locale/%s/ is missing (it is listed in locale/languages.json)" % code)
    table, where = {}, {}
    for path in sorted(folder.glob("*.json")):
        for k, v in read_jsonc(path).items():
            if k in table:
                sys.exit("FAIL: %s: \"%s\" is also in %s" % (path.relative_to(ROOT), k, where[k]))
            table[k] = v
            where[k] = path.name
    return table


def read_header(code):
    text = (UI / ("texts_%s.h" % code)).read_text(encoding="utf-8")
    return {m.group(1): c_unescape("".join(LITERAL_RE.findall(m.group(2)))) for m in ENTRY_RE.finditer(text)}


def specs(s):
    return [x for x in SPEC_RE.findall(s) if x != "%%"]


def problems(fields, tables):
    """What is wrong with the locale files, as a list of lines."""
    out = []
    keys = [k for k, _ in fields]
    known = set(keys)
    for code, table in tables.items():
        for k in table:
            if k not in known:
                out.append("locale/%s: \"%s\" is not a key (see struct ui_texts in src/ui/texts.h)" % (code, k))
        if code in ("ja", "en"):
            for k in keys:
                if k not in table:
                    out.append("locale/%s: \"%s\" is missing (ja and en must be complete)" % (code, k))
        for k in keys:
            if k in table and k in tables["ja"] and specs(table[k]) != specs(tables["ja"][k]):
                out.append("locale/%s: \"%s\" has formats %s, ja has %s" % (code, k, specs(table[k]), specs(tables["ja"][k])))
            if k in table:
                try:
                    c_escape(table[k])
                except ValueError as e:
                    out.append("locale/%s: \"%s\": %s" % (code, k, e))
    return out


def header_text(code, name, fields, table, en):
    missing = [k for k, _ in fields if k not in table]
    lines = [
        "// license:BSD-3-Clause",
        "//",
        "// %s panel strings. GENERATED by tools/locale_tool.py from locale/%s/*.json:" % (name, code),
        "// do not edit this file. Change the .json and run `python tools/locale_tool.py gen`.",
        "// Do not include directly; src/ui/texts.h pulls this in after declaring ui_texts.",
    ]
    if missing:
        lines.append("// %d of %d texts are not translated yet and show in English." % (len(missing), len(fields)))
    lines += ["", "inline const ui_texts &%s_texts()" % code, "{", "\tstatic const ui_texts t{"]
    for k, _ in fields:
        lines.append('\t\t.%s = "%s",' % (k, c_escape(table.get(k, en[k]))))
    lines += ["\t};", "\treturn t;", "}", ""]
    return "\n".join(lines)


def generated(fields, langs, tables):
    """{path: text} of everything `gen` writes."""
    out = {}
    for l in langs:
        out[UI / ("texts_%s.h" % l["code"])] = header_text(l["code"], l["name"], fields, tables[l["code"]], tables["en"])
    head = "// GENERATED by tools/locale_tool.py from %s: do not edit.\n"
    out[UI / "lang_list.inc"] = (head % "locale/languages.json"
        + "#define UI_LANG_LIST(X) \\\n"
        + " \\\n".join('\tX(%s, "%s")' % (l["code"], c_escape(l["name"])) for l in langs) + "\n")
    out[UI / "texts_tables.inc"] = (head % "locale/languages.json"
        + "".join('#include "ui/texts_%s.h"\n' % l["code"] for l in langs))
    out[UI / "texts_fields.inc"] = (head % "struct ui_texts in src/ui/texts.h"
        + "".join("X(%s)\n" % k for k, _ in fields))
    return out


def load_all():
    fields = struct_fields()
    langs = languages()
    tables = {l["code"]: read_locale(l["code"]) for l in langs}
    bad = problems(fields, tables)
    for b in bad:
        print("FAIL: " + b)
    if bad:
        sys.exit(1)
    return fields, langs, tables


def write_locale(code, fields, table, keep_missing_out=True):
    """Write locale/<code>/*.json from a table, with the notes from texts.h."""
    folder = LOCALE / code
    folder.mkdir(parents=True, exist_ok=True)
    by_file = {}
    for k, notes in fields:
        if k in table or not keep_missing_out:
            by_file.setdefault(group_of(k), []).append((k, notes))
    for old in folder.glob("*.json"):
        if old.stem not in by_file:
            old.unlink()
    for name, items in by_file.items():
        lines = ["{"]
        for i, (k, notes) in enumerate(items):
            if notes and i:
                lines.append("")
            for n in notes:
                lines.append("\t// " + n if n else "\t//")
            comma = "," if i + 1 < len(items) else ""
            lines.append("\t%s: %s%s" % (json.dumps(k), json.dumps(table[k], ensure_ascii=False), comma))
        lines.append("}")
        (folder / (name + ".json")).write_bytes(("\n".join(lines) + "\n").encode("utf-8"))


def cmd_gen(check):
    fields, langs, tables = load_all()
    stale = []
    for path, text in generated(fields, langs, tables).items():
        old = path.read_bytes().decode("utf-8").replace("\r\n", "\n") if path.exists() else None
        if old != text:
            stale.append(path)
            if not check:
                path.write_bytes(text.encode("utf-8"))
    for l in langs:
        missing = sum(1 for k, _ in fields if k not in tables[l["code"]])
        if missing:
            print("%s: %d of %d texts not translated yet (shown in English)" % (l["code"], missing, len(fields)))
    # a table left behind by a language that is no longer listed
    listed = {"texts_%s.h" % l["code"] for l in langs}
    for path in sorted(UI.glob("texts_*.h")):
        if path.name not in listed:
            print("FAIL: src/ui/%s has no entry in locale/languages.json" % path.name)
            return 1
    if check and stale:
        for p in stale:
            print("FAIL: %s is not what locale/ generates" % p.relative_to(ROOT))
        print("run: python tools/locale_tool.py gen   (or `export`, if the header is the one that was edited)")
        return 1
    print(("OK: locale/ and the generated tables agree" if check else "wrote %d file(s)" % len(stale))
          + " (%d languages, %d texts)" % (len(langs), len(fields)))
    return 0


def cmd_export():
    fields = struct_fields()
    langs = languages() if (LOCALE / "languages.json").exists() else None
    if langs is None:
        sys.exit("FAIL: locale/languages.json is missing")
    en = read_header("en")
    for l in langs:
        table = read_header(l["code"])
        if l["code"] not in ("ja", "en"):
            # a partial language keeps only what differs from English
            table = {k: v for k, v in table.items() if v != en.get(k)}
        write_locale(l["code"], fields, table)
        print("locale/%s: %d texts" % (l["code"], len(table)))
    return 0


def cmd_new(code, name):
    if not CODE_RE.match(code):
        sys.exit("FAIL: the code must be lowercase letters (pl, de, pt_br ...)")
    langs = languages()
    if any(l["code"] == code for l in langs):
        sys.exit("FAIL: %s is already listed" % code)
    fields = struct_fields()
    write_locale(code, fields, read_locale("en"))
    langs.append({"code": code, "name": name})
    (LOCALE / "languages.json").write_bytes((json.dumps(langs, ensure_ascii=False, indent="\t") + "\n").encode("utf-8"))
    print("locale/%s/ holds the English texts: translate them, delete the ones you leave for later, then run gen" % code)
    return 0


def main():
    args = sys.argv[1:]
    if args == ["gen"]:
        return cmd_gen(False)
    if args == ["check"]:
        return cmd_gen(True)
    if args == ["export"]:
        return cmd_export()
    if len(args) == 3 and args[0] == "new":
        return cmd_new(args[1], args[2])
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main())
