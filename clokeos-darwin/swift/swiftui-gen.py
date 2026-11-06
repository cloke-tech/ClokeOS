#!/usr/bin/env python3
"""Erzeugt Platzhalter-Typen für SwiftUI aus den .swiftinterface-Dateien des SDKs (SwiftUI und SwiftUICore):
nur die Typen, deren Typbeschreibung/Protokollbeschreibung ein Programm importiert (Liste aus der Importliste), mit Art (struct/class/enum/protocol), Verschachtelung und
Anzahl der generischen Parameter. Das Ergebnis macht Metadaten wie Published<SwiftUI.Image> instanziierbar; Funktionen und Konformanzen fehlen noch.

  swift/swiftui-gen.py <imports.json> <ausgabe.swift>
"""
import json, re, subprocess, sys, os

imports, out = sys.argv[1], sys.argv[2]
sdk = subprocess.run(["xcrun", "--show-sdk-path"], capture_output=True, text=True).stdout.strip()
files = [f"{sdk}/System/Library/Frameworks/{n}.framework/Versions/A/Modules/{n}.swiftmodule/arm64e-apple-macos.swiftinterface" for n in ("SwiftUICore", "SwiftUI")]

# gewünschte Typen: "SwiftUI.A.B" aus den Beschreibungen
syms = [s for s in json.load(open(imports))["SwiftUI"]["symbols"] if s.startswith("_$s")]
dem = subprocess.run(["xcrun", "swift-demangle"], input="\n".join(syms), capture_output=True, text=True).stdout.splitlines()
want = {}
for dm in dem:
    m = re.match(r"(?:nominal type descriptor|protocol descriptor|type metadata accessor|type metadata) for (SwiftUI(?:\.[A-Za-z_0-9]+)+)", dm)
    if m: want[m.group(1)] = True

ATTR = re.compile(r"@[A-Za-z_][A-Za-z_0-9.:]*(?:\([^)]*\))?\s*")
MODS = re.compile(r"\b(?:nonisolated|final|indirect|public|open|package|internal|distributed|__consuming|consuming|borrowing)\b\s*")
decl_re = re.compile(r"^\s*(struct|class|enum|actor|protocol)\s+([A-Za-z_][A-Za-z_0-9]*)\s*(<[^{]*?>)?")
ext_re = re.compile(r"^\s*extension\s+([A-Za-z_0-9:.<>?]+)")

def generics(g):
    if not g: return []
    inner, depth, cur, res = g[1:], 1, "", []
    # bis zur passenden schließenden Klammer
    body = ""
    for ch in g:
        if ch == "<": depth += 1 if body else 0
        body += ch
    body = g[1:g.rindex(">")] if ">" in g else g[1:]
    depth, cur = 0, ""
    for ch in body:
        if ch in "<(": depth += 1
        if ch in ">)": depth -= 1
        if ch == "," and depth == 0: res.append(cur.strip()); cur = ""
        else: cur += ch
    if cur.strip(): res.append(cur.strip())
    # Parameter ohne Einschränkungen: "T : Foo" -> "T"; "each T" bleibt
    names = []
    for p in res:
        p = p.split(":")[0].strip()
        if p and not p.startswith("let "): names.append(p)
    return names

found = {}   # "SwiftUI.A.B" -> (kind, [generics], [outer generics])
for fpath in files:
    if not os.path.exists(fpath): continue
    stack = []   # (tiefe, name, kind) für Typen und Erweiterungen
    depth = 0
    for line in open(fpath, errors="replace"):
        s = line.split("//")[0]
        s = MODS.sub("", ATTR.sub("", s))
        m = decl_re.match(s)
        e = ext_re.match(s) if not m else None
        scope_name = None; kind = None; gens = []
        if m and s.rstrip().endswith("{") or (m and "{" in s):
            kind, name, g = m.group(1), m.group(2), m.group(3)
            gens = generics(g)
            path = ".".join([x[1] for x in stack if x[1]] + [name]) if stack else name
            full = "SwiftUI." + path
            outer_g = [gg for x in stack for gg in x[3]]
            if full in want and full not in found:
                found[full] = (kind, gens, outer_g)
            scope_name = name; scope_gens = gens
        elif e and "{" in s:
            tname = e.group(1).replace("SwiftUICore::", "").replace("SwiftUI::", "")
            tname = re.sub(r"<.*", "", tname)
            if "::" in tname or tname.startswith("Swift") or "." in tname and False: tname = None
            scope_name = tname; scope_gens = []
        # Klammertiefe nachführen
        opens, closes = s.count("{"), s.count("}")
        if scope_name is not None or (opens > closes):
            if scope_name is not None:
                stack.append((depth, scope_name if (e or m) else None, kind, scope_gens if scope_name is not None else []))
            else:
                stack.append((depth, None, None, []))
        depth += opens - closes
        while stack and depth <= stack[-1][0] and closes:
            stack.pop()
print("gesucht", len(want), "gefunden", len(found), file=sys.stderr)
missing = sorted(set(want) - set(found))
print("fehlen:", missing[:20], file=sys.stderr)

# Quelltext: verschachtelt ausgeben
tree = {}
for full, (kind, gens, og) in found.items():
    parts = full.split(".")[1:]
    node = tree
    for p in parts[:-1]:
        node = node.setdefault(p, {"_decl": None, "_kids": {}})["_kids"]
    node.setdefault(parts[-1], {"_decl": None, "_kids": {}})["_decl"] = (kind, gens)
lines = ["// erzeugt von swift/swiftui-gen.py: Platzhalter-Typen für SwiftUI (nur Metadaten)", "import Swift", ""]
def emit(nodes, indent):
    for name, nd in sorted(nodes.items()):
        d = nd["_decl"]
        pad = "    " * indent
        if d is None:
            lines.append(f"{pad}public enum {name} {{")      # nur Namensraum für verschachtelte Typen
            emit(nd["_kids"], indent + 1); lines.append(f"{pad}}}")
        else:
            kind, gens = d
            g = "<" + ", ".join(gens) + ">" if gens else ""
            if kind == "protocol":
                if gens:   # primäre assoziierte Typen: protocol Gesture<Value>
                    lines.append(f"{pad}public protocol {name}<{', '.join(gens)}> {{ " + " ".join(f"associatedtype {x}" for x in gens) + " }")
                else:
                    lines.append(f"{pad}public protocol {name} {{ }}")
            elif kind == "class":
                lines.append(f"{pad}open class {name}{g} {{ public init() {{ }}")
                emit(nd["_kids"], indent + 1); lines.append(f"{pad}}}")
            elif kind == "actor":
                lines.append(f"{pad}public actor {name}{g} {{")
                emit(nd["_kids"], indent + 1); lines.append(f"{pad}}}")
            elif kind == "enum":
                lines.append(f"{pad}public enum {name}{g} {{ case _clkPlaceholder")
                emit(nd["_kids"], indent + 1); lines.append(f"{pad}}}")
            else:
                lines.append(f"{pad}public struct {name}{g} {{")
                emit(nd["_kids"], indent + 1); lines.append(f"{pad}}}")
emit(tree, 0)
open(out, "w").write("\n".join(lines) + "\n")
