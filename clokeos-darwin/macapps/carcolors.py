#!/usr/bin/env python3
"""Benannte Farben aus den Assets.car-Dateien eines Mac-Programms für ClokeOS' AppKit (+[NSColor colorNamed:bundle:]).

Auf dem Mac liest Apples `assetutil --info` jede Assets.car aus; neben jede kommt eine Datei Assets.colors.plist
{ Name: [Rot, Grün, Blau, Alpha] } (Graustufen als drei gleiche Anteile). Gibt es eine Farbe in mehreren Fassungen (hell/dunkel,
Kontrast), gilt die ohne Erscheinungsbild, sonst die dunkle (Logic Pro ist dunkel), sonst die erste. Abgelegt wird in einem
Spiegelordner, der Pfade relativ zum Programm behält; macapps/logicimage.sh bzw. deploy-files.sh schreiben ihn ins Image.

    macapps/carcolors.py "/Applications/Logic Pro.app" <Zielordner>    -> <Zielordner>/<…>/Assets.colors.plist
Nichts davon landet im Repository (abgeleitet aus Apples bzw. des Herstellers Dateien)."""
import json, os, plistlib, subprocess, sys

def colors(car):
    r = subprocess.run(["xcrun", "assetutil", "--info", car], capture_output=True, text=True)
    try:
        items = json.loads(r.stdout)
    except ValueError:
        return {}
    best = {}
    for it in items:
        if it.get("AssetType") != "Color" or "Name" not in it:
            continue
        comp = it.get("Color components") or []
        if len(comp) == 2:
            comp = [comp[0], comp[0], comp[0], comp[1]]
        elif len(comp) == 3:
            comp = comp + [1.0]
        if len(comp) < 4:
            continue
        app = it.get("Appearance", "")
        rank = 0 if not app else (1 if "Dark" in app and "Contrast" not in app else 2)
        name = it["Name"]
        if name not in best or rank < best[name][0]:
            best[name] = (rank, [float(x) for x in comp[:4]])
    return {k: v[1] for k, v in best.items()}

def main():
    app, dst = sys.argv[1], sys.argv[2]
    n = 0
    for dp, dn, fn in os.walk(app):
        if "Assets.car" in fn:
            c = colors(os.path.join(dp, "Assets.car"))
            if not c:
                continue
            out = os.path.join(dst, os.path.relpath(dp, os.path.dirname(app.rstrip("/"))))
            os.makedirs(out, exist_ok=True)
            with open(os.path.join(out, "Assets.colors.plist"), "wb") as f:
                plistlib.dump(c, f, fmt=plistlib.FMT_BINARY)
            n += 1
            print(f"{os.path.relpath(dp, app)}: {len(c)} Farben")
    print(f"{n} Dateien nach {dst}")

if __name__ == "__main__":
    main()
