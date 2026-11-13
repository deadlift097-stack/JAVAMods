#!/usr/bin/env python3
"""jar2mod: turn a Java Edition mod jar into a starting point for a Mod Host mod.

It reads the jar (no Minecraft needed) and writes
  - a Java project for the Mod Host (menu with the mod's settings, event stubs),
    plus a one-file GitHub workflow that builds it into a jar the host can load,
  - report.md: what was found and what could / could not be mapped,
  - bytecode_notes.txt: readable bytecode of the mod's hooks, to help port the logic,
  - assets/: the mod's language files and textures, for reference.

It does NOT translate the Java Edition logic. Bedrock has none of Java Minecraft's classes,
so the behaviour has to be written again against the Mod Host API.

Usage:  python3 jar2mod.py MOD.jar [-o OUTDIR] [--lang java|lua] [--api-dir DIR]
"""
import argparse
import json
import os
import re
import sys
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from jarread import ClassFile, disassemble, pretty_method_desc, pretty_type, simple_class  # noqa: E402

VERSION = "0.1"

# ------------------------------------------------------------------ metadata


def read_metadata(z):
    names = set(z.namelist())
    meta = {"id": None, "name": None, "version": None, "description": "", "authors": [],
            "format": "unknown", "entrypoints": {}, "mixin_configs": []}
    try:
        if "fabric.mod.json" in names:
            j = json.loads(z.read("fabric.mod.json").decode("utf-8", "replace"))
            meta.update(format="Fabric", id=j.get("id"), name=j.get("name"), version=j.get("version"),
                        description=j.get("description", ""))
            for a in j.get("authors", []):
                meta["authors"].append(a if isinstance(a, str) else a.get("name", ""))
            for kind, lst in (j.get("entrypoints") or {}).items():
                meta["entrypoints"][kind] = [e if isinstance(e, str) else e.get("value", "") for e in lst]
            for m in j.get("mixins", []):
                meta["mixin_configs"].append(m if isinstance(m, str) else m.get("config", ""))
        elif "quilt.mod.json" in names:
            j = json.loads(z.read("quilt.mod.json").decode("utf-8", "replace"))
            ql = j.get("quilt_loader", {})
            md = ql.get("metadata", {})
            meta.update(format="Quilt", id=ql.get("id"), version=ql.get("version"), name=md.get("name"),
                        description=md.get("description", ""))
            meta["authors"] = list((md.get("contributors") or {}).keys())
        else:
            toml = next((n for n in ("META-INF/neoforge.mods.toml", "META-INF/mods.toml") if n in names), None)
            if toml:
                t = z.read(toml).decode("utf-8", "replace")
                meta["format"] = "NeoForge" if "neoforge" in toml else "Forge"

                def grab(key):
                    m = re.search(rf'^\s*{key}\s*=\s*(?:\'\'\'(.*?)\'\'\'|"((?:[^"\\]|\\.)*)")', t, re.M | re.S)
                    return (m.group(1) or m.group(2) or "").strip() if m else None
                meta.update(id=grab("modId"), name=grab("displayName"), version=grab("version"),
                            description=grab("description") or "")
                a = grab("authors")
                if a:
                    meta["authors"] = [a]
            elif "mcmod.info" in names:
                j = json.loads(z.read("mcmod.info").decode("utf-8", "replace"))
                j = j.get("modList", [j])[0] if isinstance(j, dict) else j[0]
                meta.update(format="Forge (legacy)", id=j.get("modid"), name=j.get("name"),
                            version=j.get("version"), description=j.get("description", ""))
                meta["authors"] = j.get("authorList", [])
    except Exception as e:  # noqa: BLE001 - metadata is best effort
        meta["description"] = f"(metadata could not be read: {e})"
    return meta


# ------------------------------------------------------------------ classes

def load_classes(z):
    classes = {}
    for n in z.namelist():
        if n.endswith(".class") and not n.startswith("META-INF/"):
            try:
                cf = ClassFile(z.read(n))
                classes[cf.name] = cf
            except Exception:  # noqa: BLE001
                pass
    return classes


INJECTORS = {"Inject", "Redirect", "ModifyArg", "ModifyArgs", "ModifyVariable", "ModifyConstant",
             "ModifyReturnValue", "ModifyExpressionValue", "WrapOperation", "WrapWithCondition", "WrapMethod",
             "Overwrite", "Accessor", "Invoker"}


def ann_simple(a):
    return a["type"].rsplit("/", 1)[-1].rstrip(";").split("$")[-1]


def mixin_info(cf):
    mix = next((a for a in cf.annotations if a["type"].endswith("/Mixin;")), None)
    if not mix:
        return None
    targets = []
    for v in mix["values"].get("value", []) if isinstance(mix["values"].get("value"), list) else []:
        if isinstance(v, tuple):
            targets.append(v[1].lstrip("L").rstrip(";"))
    for t in mix["values"].get("targets", []) if isinstance(mix["values"].get("targets"), list) else []:
        targets.append(str(t).replace(".", "/"))
    handlers = []
    for m in cf.methods:
        kinds, wanted = [], []
        for a in m["annotations"]:
            k = ann_simple(a)
            if k not in INJECTORS:
                continue
            kinds.append(k)
            meth = a["values"].get("method")
            for x in (meth if isinstance(meth, list) else [meth] if meth else []):
                wanted.append(str(x).split("(")[0])
            if k in ("Overwrite", "Accessor", "Invoker") and not wanted:
                wanted.append(m["name"])
        if kinds:
            handlers.append({"name": m["name"], "desc": m["desc"], "kinds": kinds, "methods": wanted})
    return {"class": cf.name, "targets": targets, "handlers": handlers}


# ------------------------------------------------------------------ events

# (Mod Host event, class-name keywords, method-name keywords)
EVENT_RULES = [
    ("attack", ("GameMode", "PlayerInteraction", "Player", "Minecraft"), ("attack", "startattack", "doattack")),
    ("key", ("Keyboard", "KeyHandler", "GLFW"), ("key", "onkey", "keypress", "char")),
    ("mouse", ("Mouse",), ("press", "button", "scroll", "move", "mouse", "cursor")),
    ("tick", ("Minecraft", "ClientLevel", "ClientWorld"), ("tick", "runtick")),
]
UNSUPPORTED_TOPICS = [
    ("rendering", ("Render", "Gui", "Screen", "Hud", "Shader", "Model", "Glow", "Overlay", "Camera")),
    ("network packets", ("Packet", "Network", "Connection", "Listener")),
    ("sound", ("Sound",)),
    ("particles", ("Particle",)),
    ("world / chunks", ("Level", "World", "Chunk", "Block")),
    ("entities / players", ("Entity", "Player", "Mob", "Living", "Damage", "Armor")),
]


def classify(target, handler_methods):
    t = simple_class(target)
    for event, cws, mws in EVENT_RULES:
        if any(c.lower() in t.lower() for c in cws) and any(
                any(mw in (m or "").lower() for mw in mws) for m in handler_methods):
            return event
    for topic, words in UNSUPPORTED_TOPICS:
        if any(w.lower() in t.lower() for w in words):
            return "unsupported: " + topic
    return "unsupported: other"


# ------------------------------------------------------------------ enum settings

def const_of(cf, name, raw):
    if name.startswith("iconst_"):
        return (-1 if name.endswith("m1") else int(name[-1]),)
    if name in ("bipush", "sipush"):
        return (raw,)
    if name in ("ldc", "ldc_w", "ldc2_w"):
        e = cf.cp[raw]
        if e and e[0] == 8:
            return (cf.utf(e[1]),)
        if e and e[0] in (3, 4, 5, 6):
            return (e[1],)
        return (None,)
    if name.startswith("fconst_"):
        return (float(name[-1]),)
    if name.startswith(("lconst_", "dconst_")):
        return (float(name[-1]),)
    return None


PUSHERS = {"getstatic", "invokestatic", "invokevirtual", "invokeinterface", "aload", "aload_0", "aload_1",
           "aload_2", "aload_3", "aconst_null", "new", "iload", "getfield"}


def split_desc_args(desc):
    args, i = [], 0
    s = desc[1:desc.index(")")]
    while i < len(s):
        j = i
        while s[j] == "[":
            j += 1
        if s[j] == "L":
            j = s.index(";", j)
        args.append(s[i:j + 1])
        i = j + 1
    return args


def enum_constants(cf):
    clinit = next((m for m in cf.methods if m["name"] == "<clinit>" and m["code"]), None)
    if not clinit:
        return []
    out, pending = [], None
    for pc, name, text, raw in disassemble(cf, clinit["code"]):
        if name == "new":
            pending = {"cls": cf.cls(raw), "args": [], "dup": False, "desc": None}
            continue
        if pending is None:
            continue
        if name == "dup" and not pending["dup"]:
            pending["dup"] = True
            continue
        c = const_of(cf, name, raw)
        if c is not None:
            pending["args"].append(c[0])
            continue
        if name == "invokespecial":
            ref = cf.cp[raw]
            owner, mname = cf.cls(ref[1]), cf.nat(ref[2])
            if mname[0] == "<init>" and owner == pending["cls"]:
                pending["desc"] = mname[1]
            continue
        if name == "putstatic":
            ref = cf.cp[raw]
            fname = cf.nat(ref[2])[0]
            if pending["desc"] and cf.cls(ref[1]) == cf.name:
                out.append({"field": fname, "args": pending["args"], "desc": pending["desc"]})
            pending = None
            continue
        if name in PUSHERS:
            pending["args"].append(None)
    return out


def prettify(name):
    return " ".join(w.capitalize() for w in re.split(r"[_\s]+", re.sub(r"([a-z])([A-Z])", r"\1_\2", name)) if w)


def num(s):
    try:
        return float(s)
    except (TypeError, ValueError):
        return None


def derive_settings(cf):
    """Turn enum constants like TOGGLE(name, key, label, default) into menu settings."""
    result = []
    for c in enum_constants(cf):
        types = split_desc_args(c["desc"])
        args = c["args"]
        if len(args) != len(types) or len(args) < 3:
            continue
        rest_t, rest_a = types[2:], args[2:]  # skip synthetic name + ordinal
        strings = [a for t, a in zip(rest_t, rest_a) if t == "Ljava/lang/String;" and isinstance(a, str)]
        bools = [bool(a) for t, a in zip(rest_t, rest_a) if t == "Z" and a is not None]
        if not strings:
            continue
        if bools:
            key = strings[0]
            label = strings[1] if len(strings) > 1 and re.search(r"[A-Za-z]", strings[1]) else prettify(c["field"])
            result.append({"key": key, "label": label, "type": "toggle", "default": bools[0],
                           "source": f"{simple_class(cf.name)}.{c['field']}"})
        else:
            numeric = [s for s in strings if num(s) is not None]
            names = [s for s in strings if num(s) is None]
            if not numeric or not names:
                continue
            d = num(numeric[0])
            if 0.0 <= d <= 1.0 and "." in numeric[0]:
                s = {"type": "float", "default": d, "min": 0.0, "max": 1.0}
            else:
                s = {"type": "slider", "default": int(d), "min": 0, "max": max(10, int(abs(d) * 3) or 10)}
            s.update(key=names[0], label=prettify(c["field"]), source=f"{simple_class(cf.name)}.{c['field']}",
                     note="range guessed")
            result.append(s)
    return result


# ------------------------------------------------------------------ names & text helpers

def java_ident(s, upper_first=False):
    s = re.sub(r"[^A-Za-z0-9_]", "_", s or "mod")
    if not s or s[0].isdigit():
        s = "m" + s
    return (s[0].upper() + s[1:]) if upper_first else s


def jstr(s):
    s = (s or "").replace("\\", "\\\\").replace('"', '\\"').replace("\n", " ").replace("\r", " ")
    return '"' + s + '"'


def lstr(s):
    out = ['"']
    for ch in s or "":
        if ch == "\\":
            out.append("\\\\")
        elif ch == '"':
            out.append('\\"')
        elif ch == "\n":
            out.append("\\n")
        elif ord(ch) < 32:
            out.append("\\%d" % ord(ch))
        else:
            out.append(ch)
    out.append('"')
    return "".join(out)


def clean_key(k):
    k = re.sub(r"[^A-Za-z0-9_]", "_", k or "")
    return k if k else "setting"


# ------------------------------------------------------------------ bytecode notes

def method_notes(cf, m, maxlines=400):
    lines = [f"== {simple_class(cf.name)}.{m['name']} {pretty_method_desc(m['desc'])}"]
    for a in m["annotations"]:
        k = ann_simple(a)
        if k in INJECTORS:
            lines.append(f"   @{k} {json.dumps({kk: (vv if not isinstance(vv, dict) else vv.get('values')) for kk, vv in a['values'].items()}, default=str)}")
    if not m["code"]:
        lines.append("   (no code)")
        return lines
    for pc, name, text, raw in disassemble(cf, m["code"])[:maxlines]:
        if name.startswith(("invoke", "get", "put")) and "." in text:
            owner, rest = text.split(".", 1)
            text = simple_class(owner) + "." + rest
        lines.append(f"   {pc:4d}: {name} {text}".rstrip())
    return lines


# ------------------------------------------------------------------ generators

JAVA_MOD_TEMPLATE = """package {package};

import org.modhost.*;

/**
 * Converted from {title} by jar2mod.py {version}.
 *
 * The menu and event wiring below are generated. The behaviour of the original Java Edition
 * mod is NOT translated: Bedrock has none of Java Minecraft's classes. Port the logic by hand,
 * using report.md and bytecode_notes.txt as a guide.
 */
public class {cls} implements Mod {{
    @Override
    public void onLoad(ModContext ctx) throws Exception {{
{body}    }}
}}
"""


def gen_java(meta, settings, event_map, out_pkg):
    title = meta["name"] or meta["id"] or "the mod"
    b = []
    b.append(f"        Menu menu = new Menu({jstr(title)})\n")
    b.append(f"                .description({jstr(meta['description'] or 'Converted mod')})")
    for s in settings[:64]:
        if s["type"] == "toggle":
            b.append(f"\n                .toggle({jstr(clean_key(s['key']))}, {jstr(s['label'])}, {'true' if s['default'] else 'false'})")
        elif s["type"] == "slider":
            b.append(f"\n                .slider({jstr(clean_key(s['key']))}, {jstr(s['label'])}, {s['default']}, {s['min']}, {s['max']})")
        else:
            b.append(f"\n                .floatSlider({jstr(clean_key(s['key']))}, {jstr(s['label'])}, {s['default']}, {s['min']}, {s['max']})")
    b.append("\n                .onChange((key, value) -> {\n"
             "                    // TODO: react to a setting change (value is a Boolean, Integer or Double)\n"
             "                    ctx.log(\"setting \" + key + \" = \" + value);\n"
             "                });\n")
    b.append("        ctx.registerMenu(menu);\n\n")
    handlers = {"attack": "ctx.onAttack(timeMs -> {{\n{c}        }});",
                "key": "ctx.onKey((keyCode, down, unicodeChar) -> {{\n{c}            return false; // true consumes the key\n        }});",
                "mouse": "ctx.onMouse((button, down) -> {{\n{c}            return false;\n        }});",
                "tick": "ctx.onTick(dtMs -> {{\n{c}        }});"}
    for event, sources in event_map.items():
        if event not in handlers:
            continue
        comment = "".join(f"            // Java: {s}\n" for s in sources[:6])
        comment += "            // TODO: port the logic here (see bytecode_notes.txt)\n"
        b.append(f"        // Event '{event}' (from {len(sources)} Java hook(s))\n")
        b.append("        " + handlers[event].format(c=comment) + "\n\n")
    if not any(e in handlers for e in event_map):
        b.append("        // No Java hook mapped to a Mod Host event. Register handlers here with ctx.onKey, ctx.onTouch,\n"
                 "        // ctx.onAttack, ctx.onTick, ctx.after, ctx.every ...\n")
    return JAVA_MOD_TEMPLATE.format(package=out_pkg, title=title, version=VERSION, cls="ConvertedMod",
                                    body="".join(b))


def gen_lua(meta, settings, event_map):
    title = meta["name"] or meta["id"] or "Converted mod"
    L = [f"-- Converted from {title} by jar2mod.py {VERSION}.",
         "-- The menu and event wiring are generated. The behaviour of the Java Edition mod is NOT",
         "-- translated: port the logic by hand using report.md and bytecode_notes.txt.", ""]
    L.append("menu.register{")
    L.append(f"  id = {lstr(java_ident((meta['id'] or 'converted').lower()))},")
    L.append(f"  name = {lstr(title)},")
    L.append(f"  description = {lstr(meta['description'] or 'Converted mod')},")
    L.append("  settings = {")
    for s in settings[:64]:
        dv = ("true" if s["default"] else "false") if s["type"] == "toggle" else str(s["default"])
        extra = "" if s["type"] == "toggle" else f", min = {s['min']}, max = {s['max']}"
        L.append(f"    {{ key = {lstr(clean_key(s['key']))}, label = {lstr(s['label'])}, type = {lstr(s['type'])}, default = {dv}{extra} }},")
    L.append("  },")
    L.append("  on_change = function(key, value)\n    -- TODO: react to a setting change\n    log.info(\"setting \" .. key .. \" = \" .. tostring(value))\n  end,")
    L.append("}\n")
    sigs = {"attack": "function(info)", "key": "function(keyCode, down, unicodeChar)", "mouse": "function(button, down)",
            "tick": "function(dtMs)"}
    for event, sources in event_map.items():
        if event not in sigs:
            continue
        L.append(f"-- Event '{event}' (from {len(sources)} Java hook(s))")
        L += [f"-- Java: {s}" for s in sources[:6]]
        L.append(f'on("{event}", {sigs[event]}')
        L.append("  -- TODO: port the logic here (see bytecode_notes.txt)")
        if event in ("key", "mouse"):
            L.append("  return false -- true consumes the event")
        L.append("end)\n")
    return "\n".join(L) + "\n"


def properties_text(meta, package, cls, mod_id):
    def p(v):
        return (v or "").replace("\\", "\\\\").replace("\n", " ").replace("\r", " ")
    return ("main=%s.%s\nid=%s\nname=%s\nversion=%s\nauthor=%s\ndescription=%s\napi=1\n" % (
        package, cls, mod_id, p(meta["name"] or mod_id), p(meta["version"] or "0.1.0"),
        p(", ".join(meta["authors"])), p(meta["description"])))


WORKFLOW_BUILD = """
      - name: Build the mod jar
        run: |
          set -e
          PLATFORM_JAR="$(ls -d $ANDROID_HOME/platforms/android-*/android.jar | sort -V | tail -1)"
          D8="$(ls -d $ANDROID_HOME/build-tools/*/d8 | sort -V | tail -1)"
          mkdir -p build/api build/classes build/dex
          javac --release 8 -Xlint:-options -d build/api $(find api -name '*.java')
          javac --release 8 -Xlint:-options -cp build/api -d build/classes $(find src -name '*.java')
          "$D8" --release --min-api 26 --lib "$PLATFORM_JAR" --classpath build/api --output build/dex $(find build/classes -name '*.class')
          cp resources/modhost-mod.properties build/dex/
          (cd build/dex && jar --create --file ../../{jar} classes.dex modhost-mod.properties)

      - name: Upload the mod jar
        uses: actions/upload-artifact@v4
        with:
          name: {jar}
          path: build/{jar}
"""


def make_workflow(files, jar_name):
    """One self-contained GitHub workflow that recreates `files` and builds the mod jar."""
    ind = "          "
    out = ["name: Build Java mod", "", "on:", "  push:", "    branches:", "      - main", "  workflow_dispatch:", "",
           "jobs:", "  build:", "    runs-on: ubuntu-latest", "", "    steps:",
           "      - name: Create files", "        run: |"]
    for path, text in files.items():
        assert "MODEOF" not in text
        d = os.path.dirname(path)
        if d:
            out.append(f"{ind}mkdir -p {d}")
        out.append(f"{ind}cat > {path} <<'MODEOF'")
        out += [(ind + ln) if ln else "" for ln in text.rstrip("\n").split("\n")]
        out.append(f"{ind}MODEOF")
    out.append(WORKFLOW_BUILD.format(jar=jar_name).rstrip("\n").replace("\n\n      - name: Build", "\n      - name: Build", 1))
    return "\n".join(out).replace("      - name: Create files", "      - name: Create files", 1) + "\n"


# ------------------------------------------------------------------ main

def main():
    ap = argparse.ArgumentParser(description="Convert a Java Edition mod jar into a Mod Host mod scaffold.")
    ap.add_argument("jar")
    ap.add_argument("-o", "--out", default="converted")
    ap.add_argument("--lang", choices=("java", "lua"), default="java")
    ap.add_argument("--api-dir", default=None, help="folder with the org/modhost API sources (for the workflow)")
    args = ap.parse_args()

    z = zipfile.ZipFile(args.jar)
    meta = read_metadata(z)
    classes = load_classes(z)
    jar_stem = os.path.splitext(os.path.basename(args.jar))[0]
    mod_id = re.sub(r"[^a-z0-9_]", "_", (meta["id"] or jar_stem).lower())
    outdir = os.path.join(args.out, mod_id)
    os.makedirs(outdir, exist_ok=True)

    # mixins
    mixins = [m for m in (mixin_info(cf) for cf in classes.values()) if m]
    event_map, unmapped, rows = {}, [], []
    for mi in mixins:
        tgt = mi["targets"][0] if mi["targets"] else "?"
        methods = [x for h in mi["handlers"] for x in h["methods"]]
        status = classify(tgt, methods)
        rows.append((simple_class(mi["class"]), ", ".join(simple_class(t) for t in mi["targets"]) or "?",
                     ", ".join(sorted(set(methods))) or "-", status))
        if status in ("attack", "key", "mouse", "tick"):
            event_map.setdefault(status, []).append(f"{simple_class(mi['class'])} -> {simple_class(tgt)}.{'/'.join(sorted(set(methods)))}")
        else:
            unmapped.append(simple_class(mi["class"]))

    # settings from enums
    settings, seen = [], set()
    for cf in classes.values():
        if cf.super == "java/lang/Enum":
            for s in derive_settings(cf):
                if s["key"] not in seen:
                    seen.add(s["key"])
                    settings.append(s)

    # bytecode notes for mixin handlers
    notes = [f"Bytecode of the mixin handlers in {os.path.basename(args.jar)}", ""]
    for mi in mixins:
        cf = classes[mi["class"]]
        for m in cf.methods:
            if any(ann_simple(a) in INJECTORS for a in m["annotations"]):
                notes += method_notes(cf, m) + [""]
    open(os.path.join(outdir, "bytecode_notes.txt"), "w", encoding="utf-8").write("\n".join(notes))

    # assets
    n_assets = 0
    for n in z.namelist():
        if n.startswith("assets/") and ("/lang/" in n or "/textures/" in n) and not n.endswith("/"):
            data = z.read(n)
            if len(data) < 2_000_000 and n_assets < 500:
                dest = os.path.join(outdir, n)
                os.makedirs(os.path.dirname(dest), exist_ok=True)
                open(dest, "wb").write(data)
                n_assets += 1

    # mod sources
    package = "converted." + java_ident(mod_id)
    project = os.path.join(outdir, "java-project" if args.lang == "java" else "lua-mod")
    if args.lang == "java":
        src = gen_java(meta, settings, event_map, package)
        props = properties_text(meta, package, "ConvertedMod", mod_id)
        files = {f"src/{package.replace('.', '/')}/ConvertedMod.java": src, "resources/modhost-mod.properties": props}
        api_dir = args.api_dir or os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "java", "src", "org", "modhost")
        api_found = os.path.isdir(api_dir)
        if api_found:
            for fn in sorted(os.listdir(api_dir)):
                if fn.endswith(".java") and fn not in ("JavaHost.java", "Native.java", "NativeApi.java"):
                    files["api/org/modhost/" + fn] = open(os.path.join(api_dir, fn), encoding="utf-8").read()
        for path, text in files.items():
            dest = os.path.join(project, path)
            os.makedirs(os.path.dirname(dest), exist_ok=True)
            open(dest, "w", encoding="utf-8").write(text)
        wf = make_workflow(files, mod_id + ".jar")
        open(os.path.join(project, "build_mod.yml"), "w", encoding="utf-8").write(wf)
    else:
        os.makedirs(project, exist_ok=True)
        open(os.path.join(project, "main.lua"), "w", encoding="utf-8").write(gen_lua(meta, settings, event_map))
        json.dump({"id": mod_id, "name": meta["name"] or mod_id, "version": meta["version"] or "0.1.0",
                   "author": ", ".join(meta["authors"]), "description": meta["description"]},
                  open(os.path.join(project, "mod.json"), "w"), indent=2)

    # report
    R = [f"# Conversion report: {meta['name'] or mod_id}", "",
         f"* Source: `{os.path.basename(args.jar)}` ({meta['format']} mod, id `{meta['id']}`, version `{meta['version']}`)",
         f"* Output: {args.lang} mod scaffold in `{os.path.basename(project)}/`",
         f"* Classes read: {len(classes)}; mixins: {len(mixins)}; settings found: {len(settings)}; assets copied: {n_assets}", "",
         "## What this tool did", "",
         "* Read the mod's metadata and its settings (from enum constants) and put them in the generated menu.",
         "* Mapped the Java hooks it recognised to Mod Host events (below) and left an empty handler for each.",
         "* Wrote readable bytecode of every hook to `bytecode_notes.txt`.", "",
         "## What it did NOT do", "",
         "The Java Edition logic is not translated. Bedrock has none of Java Minecraft's classes, so every",
         "handler body is a TODO that has to be written again against the Mod Host API (`docs/`).", "",
         "## Hooks (mixins)", "", "| Mixin class | Targets | Methods | Mod Host |", "|---|---|---|---|"]
    for r in rows:
        R.append("| " + " | ".join(r) + " |")
    if not rows:
        R.append("| (none found) | | | |")
    R += ["", "## Settings", ""]
    for s in settings[:200]:
        R.append(f"* `{s['key']}` ({s['type']}, default {s['default']}) from {s['source']} {s.get('note', '')}".rstrip())
    if not settings:
        R.append("None found. Only enum-based settings are detected for now.")
    if len(settings) > 64:
        R.append(f"\nThe menu holds at most 64 settings; the first 64 were generated.")
    R += ["", "## Mod Host events available today", "",
          "`key`, `touch`, `mouse` (input), `attack` (needs `attackEvent` in the host config), `tick` (host timer).",
          "There is no HUD drawing, sound, world or entity access yet; hooks that need those stay unsupported.", ""]
    open(os.path.join(outdir, "report.md"), "w", encoding="utf-8").write("\n".join(R))

    print(f"Converted {meta['name'] or mod_id}: {len(mixins)} mixins ({len(event_map)} event types mapped), "
          f"{len(settings)} settings, {n_assets} assets")
    print("Output:", outdir)


if __name__ == "__main__":
    main()
