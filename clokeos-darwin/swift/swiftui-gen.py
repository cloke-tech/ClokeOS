#!/usr/bin/env python3
"""Erzeugt Platzhalter-Typen für SwiftUI aus den .swiftinterface-Dateien des SDKs (SwiftUICore und SwiftUI).

Gewünscht sind die Typen, deren Typbeschreibung, Protokollbeschreibung oder Metadaten ein Programm importiert (Importliste von
macapps/stubs.sh). Für jeden Typ: Art (struct/class/enum/protocol), Verschachtelung, Anzahl der generischen Parameter. Für @frozen-Typen
zusätzlich die gespeicherten Eigenschaften bzw. die Fälle (ihr Speicherlayout ist im Programm einkompiliert); Typen, die solche Felder
benutzen (oft package/internal, z. B. AnyImageProviderBox), kommen als Platzhalter dazu. Funktionen und Konformanzen fehlen noch.

  swift/swiftui-gen.py <imports.json> <ausgabe.swift>
"""
import json, os, re, subprocess, sys

imports, out = sys.argv[1], sys.argv[2]
sdk = subprocess.run(["xcrun", "--show-sdk-path"], capture_output=True, text=True).stdout.strip()
files = [f"{sdk}/System/Library/Frameworks/{n}.framework/Versions/A/Modules/{n}.swiftmodule/arm64e-apple-macos.swiftinterface"
         for n in ("SwiftUICore", "SwiftUI")]

syms = [s for s in json.load(open(imports))["SwiftUI"]["symbols"] if s.startswith("_$s")]
dem = subprocess.run(["xcrun", "swift-demangle"], input="\n".join(syms), capture_output=True, text=True).stdout.splitlines()
want = set()
for dm in dem:
    m = re.match(r"(?:nominal type descriptor|protocol descriptor|type metadata accessor|type metadata) for SwiftUI((?:\.[A-Za-z_0-9]+)+)", dm)
    if m: want.add(m.group(1)[1:])

ATTR = re.compile(r"@[A-Za-z_][A-Za-z_0-9.:]*(?:\([^)]*\))?\s*")
MODS = re.compile(r"\b(?:nonisolated|final|indirect|public|open|package|internal|private|fileprivate|distributed)\b\s*")
DECL = re.compile(r"^\s*(struct|class|enum|actor|protocol)\s+([A-Za-z_][A-Za-z_0-9]*)\s*(<[^{]*?>)?")
EXT = re.compile(r"^\s*extension\s+([A-Za-z_0-9:.]+)")
FIELD = re.compile(r"^\s*(?:weak\s+|unowned\s+)?(var|let)\s+(\w+)\s*:\s*([^{=]+?)\s*$")
CASE = re.compile(r"^\s*case\s+(.+?)\s*$")

def qual(t):
    """Typname aus der Schnittstelle (SwiftUICore::Text.SwiftUICore::Storage) -> Text.Storage"""
    return re.sub(r"\b(?:SwiftUICore|SwiftUI)::", "", t).strip()

def generics(g):
    if not g: return []
    body = g[1:g.rindex(">")] if ">" in g else g[1:]
    depth, cur, parts = 0, "", []
    for ch in body:
        if ch in "<([": depth += 1
        if ch in ">)]": depth -= 1
        if ch == "," and depth == 0: parts.append(cur); cur = ""
        else: cur += ch
    if cur.strip(): parts.append(cur)
    return [p.split(":")[0].strip() for p in parts if p.strip()]

index = {}    # "A.B" -> dict(kind, gens, frozen, fields, cases)
for path in files:
    if not os.path.exists(path): continue
    stack = []        # (tiefe beim Öffnen, Name oder None, ist Typ)
    depth = 0
    pending_frozen = False
    for raw in open(path, errors="replace"):
        line = raw.split("//")[0]
        frozen_here = "@frozen" in line
        s = MODS.sub("", ATTR.sub("", line))
        opens, closes = s.count("{"), s.count("}")
        scope = [x[1] for x in stack if x[1]]
        m = DECL.match(s)
        e = EXT.match(s) if not m else None
        if m and opens:
            kind, name = m.group(1), m.group(2)
            q = ".".join(scope + [name])
            first = q not in index
            if first:
                index[q] = dict(kind=kind, gens=generics(m.group(3)), frozen=frozen_here or pending_frozen, fields=[], cases=[])
            stack.append((depth, name, first))   # zweite Deklaration (#if-Varianten der Schnittstelle): keine Felder mehr
        elif e and opens:
            stack.append((depth, qual(e.group(1)), None))
        elif opens > closes:
            stack.append((depth, None, None))
        elif stack and stack[-1][2] is True and depth == stack[-1][0] + 1:
            # Felder und Fälle direkt im Rumpf eines Typs
            owner = ".".join(x[1] for x in stack if x[1])
            ent = index.get(owner)
            if ent is not None and ent["frozen"]:
                f = FIELD.match(s)
                c = CASE.match(s) if ent["kind"] == "enum" else None
                if f and "{" not in s and not s.strip().startswith("static"): ent["fields"].append((f.group(2), f.group(3)))
                elif c: ent["cases"].append(c.group(1))
        pending_frozen = frozen_here and not m
        depth += opens - closes
        while stack and depth <= stack[-1][0]:
            stack.pop()

# Von Hand geschriebene Typen (swift/overlay/SwiftUIFuncs.swift), hier nicht als Platzhalter
HANDWRITTEN = {"NSHostingView", "NSHostingController"}
want -= HANDWRITTEN

# Hülle: gewünschte Typen plus alles, was Felder/Fälle von @frozen-Typen benutzen
need, todo = set(), list(want)
while todo:
    q = todo.pop()
    if q in need or q not in index or q.split(".")[0] in HANDWRITTEN: continue
    need.add(q)
    parts = q.split(".")
    for i in range(1, len(parts)):     # äußere Typen
        todo.append(".".join(parts[:i]))
    ent = index[q]
    for src in [ty for _, ty in ent["fields"]] + ent["cases"]:
        flat = src
        while re.search(r"<[^<>]*>", flat): flat = re.sub(r"<[^<>]*>", "", flat)   # generische Argumente entfernen, damit A<B>.C als A.C erkannt wird
        flat = flat.replace("SwiftUICore.", "SwiftUICore::").replace("SwiftUI.", "SwiftUI::")
        todo += [qual(x) for x in re.findall(r"(?:SwiftUICore|SwiftUI)::[A-Za-z_0-9]+(?:\.(?:SwiftUICore::|SwiftUI::)?[A-Za-z_0-9]+)*", flat)]
        todo += [qual(x) for x in re.findall(r"(?:SwiftUICore|SwiftUI)::[A-Za-z_0-9]+(?:\.(?:SwiftUICore::|SwiftUI::)?[A-Za-z_0-9]+)*", src)]
print("gesucht", len(want), "gefunden", len(want & set(index)), "mit Hülle", len(need), file=sys.stderr)
print("fehlen:", sorted(want - set(index))[:20], file=sys.stderr)

extra = {}   # Name -> Anzahl generischer Parameter (aus der Verwendung)
def swifttype(t, gens=()):
    t = re.sub(r"\b(?:SwiftUICore|SwiftUI)::", "SwiftUI.", t)
    t = re.sub(r"\bSwiftUICore\.", "SwiftUI.", t)
    for g in gens:   # assoziierte Typen generischer Parameter (Key.Value): die Platzhalter-Protokolle haben sie nicht
        t = re.sub(r"\b%s\.[A-Za-z_][A-Za-z_0-9]*" % re.escape(g), "Swift.Never", t)
    t = re.sub(r"SwiftUI\.([A-Za-z_0-9]+)\.SwiftUI\.", r"SwiftUI.\1.", t)
    t = t.replace("::", ".")
    t = re.sub(r"(?<=[>A-Za-z0-9_])\.SwiftUI\.", ".", t)            # SwiftUI.StateObject<T>.SwiftUI.Storage -> ….Storage
    def member(m):                                                   # Mitgliedstypen generischer Typen, die es als Platzhalter nicht gibt (Typaliasse)
        outer = re.sub(r"<.*", "", m.group(1)).replace("SwiftUI.", "")
        return m.group(0) if f"{outer}.{m.group(2)}" in index else "Swift.Never"
    t = re.sub(r"(SwiftUI\.[A-Za-z_0-9.]+<[^<>]*>)\.([A-Z][A-Za-z_0-9]*)", member, t)
    for name in re.findall(r"SwiftUI\.([A-Za-z_][A-Za-z_0-9]*)", t):  # in der Schnittstelle nicht deklarierte (private) Typen
        if name not in index and name not in extra: extra[name] = len(re.findall(r"SwiftUI\.%s<([^<>]*)>" % name, t) and re.search(r"SwiftUI\.%s<([^<>]*)>" % name, t).group(1).split(",") or [])
    return t

tree = {}
for q in need:
    node = tree
    parts = q.split(".")
    for p in parts[:-1]:
        node = node.setdefault(p, {"_q": None, "_kids": {}})["_kids"]
    node.setdefault(parts[-1], {"_q": None, "_kids": {}})["_q"] = q

lines = ["// erzeugt von swift/swiftui-gen.py: Platzhalter-Typen für SwiftUI (Metadaten und Layout der @frozen-Typen)",
         "import Swift", "import CoreFoundation", "import CoreGraphics", ""]
def emit(nodes, ind):
    for name, nd in sorted(nodes.items()):
        pad = "    " * ind
        q = nd["_q"]
        ent = index.get(q) if q else None
        if ent is None:
            lines.append(f"{pad}public enum {name} {{"); emit(nd["_kids"], ind + 1); lines.append(f"{pad}}}"); continue
        g = "<" + ", ".join(ent["gens"]) + ">" if ent["gens"] else ""
        fr = "@frozen " if ent["frozen"] and ent["kind"] in ("struct", "enum") else ""
        if ent["kind"] == "protocol":
            assoc = " ".join(f"associatedtype {x}" for x in ent["gens"])
            lines.append(f"{pad}public protocol {name}{g} {{ {assoc} }}")
            continue
        if ent["kind"] == "class":
            lines.append(f"{pad}open class {name}{g} {{ public init() {{ }}")
        elif ent["kind"] == "actor":
            lines.append(f"{pad}public actor {name}{g} {{")
        elif ent["kind"] == "enum":
            lines.append(f"{pad}{fr}public enum {name}{g} {{")
            cases = ent["cases"] if ent["frozen"] and ent["cases"] else ["_clkPlaceholder"]
            for c in cases: lines.append(f"{pad}    case {swifttype(c, ent['gens'])}")
        else:
            lines.append(f"{pad}{fr}public struct {name}{g} {{")
        if ent["kind"] in ("struct",) and ent["frozen"]:
            for fname, ftype in ent["fields"]:
                lines.append(f"{pad}    public var {fname}: {swifttype(ftype, ent['gens'])}")
            if ent["fields"]:
                def arg(f, ty):
                    st = swifttype(ty, ent["gens"])
                    esc = "@escaping " if st.startswith("(") and "->" in st and not st.endswith("?") else ""
                    return f"{f}: {esc}{st}"
                args = ", ".join(arg(f, ty) for f, ty in ent["fields"])
                lines.append(f"{pad}    public init({args}) {{ " + "; ".join(f"self.{f} = {f}" for f, _ in ent["fields"]) + " }")
        emit(nd["_kids"], ind + 1)
        lines.append(f"{pad}}}")
emit(tree, 0)
# private Typen, die nur in Feldern vorkommen: als Klasse (meist Speicherkästen), Anzahl der Parameter aus der Verwendung
for name, n in sorted(extra.items()):
    g = "<" + ", ".join(f"T{i}" for i in range(n)) + ">" if n else ""
    lines.append(f"open class {name}{g} {{ public init() {{ }} }}")
open(out, "w").write("\n".join(lines) + "\n")
