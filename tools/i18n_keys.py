#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Lists the texts the engine page translates, and checks i18n.js against them:
#   tools/i18n_keys.py          -> every key, one per line
#   tools/i18n_keys.py --check  -> missing or unused keys and {placeholder} mismatches per language
import json, re, subprocess, sys
from pathlib import Path

web = Path(__file__).resolve().parent.parent / "src/engine/web"
# The page's sources: the HTML shell and everything under src/.
page = "\n".join(f.read_text() for f in [web / "index.html"] + sorted(p for p in (web / "src").rglob("*") if p.suffix in (".ts", ".tsx", ".js")))

keys = set(re.findall(r'\bt\("((?:[^"\\]|\\.)*)"', page))
keys |= set(re.findall(r'data-i18n(?:-title)?="([^"]*)"', page))
keys |= {k for pair in re.findall(r'plural\([^,]+, "([^"]+)", "([^"]+)"\)', page) for k in pair}
# templates chosen inside t(a ? "..." : "...")
# (not the text between two JSX attributes: no text has <, > or =)
keys |= {k for k in re.findall(r'"([^"\n\\]*\{\w+\}[^"\n\\]*)"', page) if k == k.strip() and not re.search(r'[<>=]', k)}
# Texts that reach t() through a variable: list tables in the page and what the engine sends.
keys |= set(re.findall(r'hint: "([^"]+)"', page))                            # FORMATS
keys |= set(re.findall(r'\{ value: [0-9.]+, label: "([^"]+)" \}', page))     # SCALES
dep_state = page[page.index("const DEP_STATE"):]
keys |= {m for pair in re.findall(r'\["(\w+)", "([^"]+)"\]', dep_state[:dep_state.index(";")]) for m in pair[1:]}
ENGINE = [
    # job states and stages
    "queued", "running", "done", "failed", "cancelled", "interrupted", "paused",
    "Starting", "Done", "Paused", "Pausing", "Continues from its checkpoint", "Continues when the engine starts",
    "Paused; starts over when resumed", "Building exFAT", "Building the inner image", "Checking the package", "Checking the package (on the other engine)",
    "Choosing LZ4 profile", "Comparing", "Compressing PFSC", "Compressing", "Packing dump", "Copying image", "Copying",
    "Decoding PFSC", "Deleting the source", "Extracting", "Handing it to the system installer", "LZ4 pass 1",
    "LZ4 planning", "Measuring", "Checking eboot.bin", "Writing the emulator", "Verifying LZ4 packs", "Verifying", "Writing PFS image", "Writing UFS2 image",
    # how a conversion was done
    "LZ4-packed and {rest}", "LZ4-unpacked and {rest}", "added the AMPR emulator and {rest}",
    "added the AMPR emulator to the game folder", "added the AMPR emulator as a ShadowMount+ backport",
    "nothing to do: the emulator is already in place",
    "built a debug package from the file tree", "built a new exFAT image from the file tree",
    "built a new exFAT image from the file tree and compressed it", "built a new exFAT image from the folder and compressed it",
    "built a new PFS image from the file tree", "built a new UFS2 image from the file tree", "extracted the file tree",
    "unwrapped the exFAT image unchanged", "unwrapped the PFS image unchanged", "unwrapped the UFS2 image unchanged",
    "another queued job uses it",
    # components (deps.cpp)
    "AMPR emulator (ampr_emu)", "FPKG key files",
    "Stands in for the console's APR file reader (fakelib/libSceAmpr.sprx), so games that use it start from an image or folder; also reads LZ4 packs. Added to such games when converting, or to an existing game.",
    "The public RSA-3072 moduli a debug FPKG is built with (passcode key wraps, image key wrap, metadata header). Needed for PKG output.",
    # shortcut status (main.cpp), the simple ones
    "not tried", "registering…", "registered", "removed", "turned off in Settings", "not available",
    # place names (platform_ps5.cpp)
    "Internal storage", "Internal", "USB {n}", "Extended {n}",
]
keys |= set(ENGINE)
# The engine's notifications (l10n::t in main.cpp) and the messages it sends (the patterns in i18n.js).
engine_src = (web.parent / "main.cpp").read_text()
keys |= set(re.findall(r'l10n::t\(\s*"((?:[^"\\]|\\.)*)"', engine_src))
keys |= set(re.findall(r'\? "((?:[^"\\]|\\.)*\{name\})"', engine_src))  # l10n::t(a ? "Copied {name}" : ...)
# Pairing's notifications go through its own t() (pairing.cpp).
keys |= set(re.findall(r'\bt\(\s*"((?:[^"\\]|\\.)*)"', (web.parent / "pairing.cpp").read_text()))
# The console app (app/src/app): t("...") and plural(n, "...", "...") in C++,
# with split literals joined and \xHH escapes decoded.
def _cpp_text(lit):
    return re.sub(rb"\\x([0-9A-Fa-f]{2})", lambda m: bytes([int(m.group(1), 16)]), lit.encode()).decode("utf-8")
app_src = "\n".join(f.read_text() for f in sorted((web.parent.parent.parent / "app/src/app").glob("*.cpp")))
app_src = re.sub(r'"\s+"', "", app_src)
app_keys = set(re.findall(r'\bt\(\s*"((?:[^"\\]|\\.)*)"', app_src))
app_keys |= {k for pair in re.findall(r'plural\([^,]+, "([^"]+)", "([^"]+)"\)', app_src) for k in pair}
app_keys |= set(re.findall(r'\{"\w+", "[^"]+", "([^"]* [^"]*)"\}', app_src))  # the Convert formats
keys |= {_cpp_text(k).replace('\\"', '"') for k in app_keys}
_js = (web / "i18n.js").read_text()
keys |= set(json.loads(_js[_js.index("{", _js.index("var I18N =")):_js.rindex("}") + 1]).get("messages", []))
keys = sorted(k.encode().decode("unicode_escape") if "\\" in k else k for k in keys)

if "--check" not in sys.argv:
    print("\n".join(keys)); sys.exit(0)

js = (web / "i18n.js").read_text()
out = subprocess.run(["node", "-e", js + "\nprocess.stdout.write(JSON.stringify(I18N));"], capture_output=True, text=True, check=True).stdout
tables = json.loads(out)
ph = lambda s: sorted(re.findall(r"\{(\w+)\}", s))
bad = 0
for lang, table in tables.items():
    if lang == "messages": continue
    missing = [k for k in keys if k not in table]
    unused = [k for k in table if k not in keys]
    wrong = [k for k in keys if k in table and ph(k) != ph(table[k])]
    for k in missing: print(f"{lang}: missing {k!r}")
    for k in unused: print(f"{lang}: unused {k!r}")
    for k in wrong: print(f"{lang}: placeholders differ in {k!r}: {table[k]!r}")
    bad += len(missing) + len(unused) + len(wrong)
print(f"{len(keys)} keys, {len([l for l in tables if l != "messages"])} languages, {bad} problems")
sys.exit(1 if bad else 0)
