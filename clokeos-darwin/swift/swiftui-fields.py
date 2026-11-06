#!/usr/bin/env python3
"""Listet für @frozen-Typen aus den SwiftUI-Schnittstellen des SDKs die gespeicherten Eigenschaften (Name, Typ).
Ihr Speicherlayout ist in Programmen wie Logic Pro einkompiliert; Platzhalter brauchen dieselben Felder.
  swift/swiftui-fields.py [Typname …]     ohne Namen: alle"""
import re, subprocess, sys
sdk = subprocess.run(["xcrun", "--show-sdk-path"], capture_output=True, text=True).stdout.strip()
FIELD = re.compile(r"^\s*(?:@[\w:.]+(?:\([^)]*\))?\s*)*(?:(?:package|internal|public|private|fileprivate|nonisolated|final|weak|unowned)(?:\(set\))?\s+)*(var|let)\s+(\w+)\s*:\s*([^{=]+?)\s*$")
HEAD = re.compile(r"^@frozen\b.*?\b(struct|enum)\s+(\w+)")
want = set(sys.argv[1:])
for mod in ("SwiftUICore", "SwiftUI"):
    path = f"{sdk}/System/Library/Frameworks/{mod}.framework/Versions/A/Modules/{mod}.swiftmodule/arm64e-apple-macos.swiftinterface"
    lines = open(path, errors="replace").read().splitlines()
    i = 0
    while i < len(lines):
        m = HEAD.match(lines[i])
        if m and lines[i].rstrip().endswith("{") and (not want or m.group(2) in want):
            depth, j, fields = 1, i + 1, []
            while j < len(lines) and depth > 0:
                s = lines[j]
                if depth == 1:
                    f = FIELD.match(s)
                    if f: fields.append(f"{f.group(2)}: {f.group(3)}")
                depth += s.count("{") - s.count("}")
                j += 1
            print(f"{mod} {m.group(1)} {m.group(2)}: " + "; ".join(fields))
            i = j
        else:
            i += 1
