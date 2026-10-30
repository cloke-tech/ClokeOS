#!/usr/bin/env python3
"""Erzeugt Stub-Frameworks für geschlossene Apple-Bibliotheken, damit Mac-Programme sie laden können.

    macapps/stubgen/stubgen.py IMPORTS.json AUSGABE [--arch x86_64|arm64] [--only Name ...]
        --build  PDLIBS CFOBJ     auch übersetzen (PDLIBS = Sysroot mit libSystem/libobjc, CFOBJ = CoreFoundation-Dylib)

IMPORTS.json kommt von collect.py (Importe je Bibliothek, Installationsname, Versionen). Für jede
Bibliothek aus FRAMEWORKS entsteht AUSGABE/src/<Name>.c und, mit --build, ein Framework unter
AUSGABE/root/Library/ClokeOS/MacFrameworks. Je Symbol:
  - C-Funktion      Stub, gibt 0/NULL/Fehlerstatus zurück (Typ aus den SDK-Headern, sonst void *);
                    mit CLOKE_STUB_VERBOSE im Environment steht beim ersten Aufruf eine Zeile auf stderr
  - Konstante       NSString-/CFString-Konstanten = ihr eigener Name, alles andere 128 Byte Nullen
  - ObjC-Klasse     echte Unterklasse von NSObject mit Wurzelklasse, die jede unbekannte Nachricht
                    mit nil/0 beantwortet (+resolveInstanceMethod:/+resolveClassMethod:)
  - Swift (_$s…)    nicht erzeugt, steht in AUSGABE/swift-symbols.txt (braucht das echte Swift-Overlay)
Von Hand geschriebenes liegt in overrides/<Name>.c (erste Zeile: "// provides: sym sym …").
"""
import argparse, json, os, re, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))

# Reihenfolge wie im Auftrag; "dylib" = flache Bibliothek unter /usr/lib
FRAMEWORKS = """Metal MetalKit MetalPerformanceShaders ScreenCaptureKit StoreKit Network CryptoKit
UserNotifications GameController MultipeerConnectivity QuickLookUI PDFKit GameplayKit CoreMotion
iTunesLibrary DiscRecording UniformTypeIdentifiers CoreML WebKit JavaScriptCore Security PackageKit
AssetCacheServices libMobileGestalt Cinematic AVFoundation AVFAudio CoreMedia CoreVideo VideoToolbox
MediaToolbox AVKit CoreMIDI""".split()

# Symbole, die die Programme nicht importieren, die es aber geben soll
EXTRAS = {
    "Metal": ["MTLCopyAllDevicesWithObserver", "MTLRemoveDeviceObserver"],
    # PureDarwins SystemConfiguration (im Framework-Pfad mitgeladen) importiert diese fünf aus Security;
    # das Stub-Security ersetzt PureDarwins Security im Framework-Pfad ganz
    "Security": ["SecTaskCreateFromSelf", "SecTaskCopyValueForEntitlement", "CC_SHA256_Init", "CC_SHA256_Update", "CC_SHA256_Final"],
    "libMobileGestalt": ["MGCopyAnswer", "MGCopyAnswerWithError", "MGGetBoolAnswer", "MGGetSInt32Answer",
                         "MGGetSInt64Answer", "MGGetFloat32Answer", "MGGetStringAnswer", "MGGetProductType",
                         "MGIsDeviceOneOfType", "MGCopyMultipleAnswers"],
}
# Eingetragen, wenn collect.py die Bibliothek nicht gesehen hat (nur geladen, nichts importiert)
FALLBACK = {
    "libMobileGestalt": ("/usr/lib/libMobileGestalt.dylib", "1.0.0", "1.0.0"),
    "Cinematic": ("/System/Library/Frameworks/Cinematic.framework/Versions/A/Cinematic", "1.0.0", "1.0.0"),
}
NOTES = {"CoreMIDI": "nur Stub; durch eine echte Implementierung (über /dev/umidi o. ä.) zu ersetzen"}

# Rückgabewert je Typname: p = Zeiger/Ganzzahl 0, d = double, f = float, v = void, e = Fehlerstatus
DOUBLE = {"double", "CGFloat", "Float64", "NSTimeInterval", "CFTimeInterval", "CFAbsoluteTime", "CMTimeScale_d"}
FLOAT = {"float", "Float32"}
STATUS = {"OSStatus": -4, "CVReturn": -6660, "OSErr": -4, "CMSDecoderStatus": -4}
STRUCTS = {"CMTime": 24, "CMTimeRange": 48, "CMTimeMapping": 96, "CGRect": 32, "CGSize": 16, "CGPoint": 16,
           "NSRange": 16, "CFRange": 16, "CMVideoDimensions": 8, "CGAffineTransform": 48, "CMFormatDescriptionRef_": 8,
           "AudioStreamBasicDescription": 40, "AudioTimeStamp": 64, "MIDIPacket": 266, "CVTime": 24,
           "NSEdgeInsets": 32, "CGVector": 16, "CMVideoDimension": 8}
# Konstanten mit sinnvollem Anfangswert (CMTime: value, timescale, flags, epoch)
CMTIME = {"kCMTimeZero": (0, 1, 1), "kCMTimeInvalid": (0, 0, 0), "kCMTimeIndefinite": (0, 0, 17),
          "kCMTimePositiveInfinity": (0, 0, 5), "kCMTimeNegativeInfinity": (0, 0, 9)}
# Funktionen, die in den Headern fehlen: Security/CoreMIDI geben fast überall OSStatus zurück
UNKNOWN_RET = {"Security": ("e", -4), "CoreMIDI": ("e", -4)}
STRINGISH = re.compile(r"(Notification|Key|Keys|Domain|Name|Identifier|Mode|Preset|Profile|Tag|Type|Option|Options|Property|Extension)$")


# ---------------------------------------------------------------- Header-Auswertung

def sdk_path():
    return subprocess.run(["xcrun", "--show-sdk-path"], capture_output=True, text=True).stdout.strip()


def strip_noise(t):
    t = re.sub(r"/\*.*?\*/", " ", t, flags=re.S)
    t = re.sub(r"//[^\n]*", " ", t)
    t = re.sub(r"(?m)^\s*#(?:[^\n\\]|\\\n|\\.)*$", " ", t)
    t = re.sub(r"(?m)^\s*[A-Z][A-Z0-9_]*_(?:BEGIN|END|ENABLED|DISABLED)\s*$", " ", t)
    return t


class Headers:
    """Sucht Deklarationen in den SDK-Headern, um Funktion/Konstante und Typen zu unterscheiden."""

    def __init__(self, sdk):
        self.sdk = sdk
        self.cache = {}
        self.strtypes = None

    def text(self, fw):
        if fw not in self.cache:
            parts = []
            base = f"{self.sdk}/System/Library/Frameworks/{fw}.framework"
            for d, _, files in os.walk(base, followlinks=True):
                if "/Versions/" in d:
                    continue
                for f in files:
                    if f.endswith(".h"):
                        try:
                            parts.append(strip_noise(open(os.path.join(d, f), errors="replace").read()))
                        except OSError:
                            pass
            self.cache[fw] = "\n".join(parts)
        return self.cache[fw]

    def string_types(self):
        """Namen, die ein typedef auf NSString * / CFStringRef sind (AVMediaType, NSNotificationName …)."""
        if self.strtypes is None:
            s = {"NSString", "CFStringRef", "NSNotificationName", "NSRunLoopMode", "NSErrorDomain", "NSPasteboardType"}
            for fw in ("Foundation", "CoreFoundation", "AppKit", "AVFoundation", "AVFAudio", "CoreMedia", "CoreVideo", "Metal"):
                for m in re.finditer(r"typedef\s+(?:NSString|CFStringRef|CFString)\s*\*?\s*(?:_Nonnull\s+)?(?:const\s+)?(\w+)", self.text(fw)):
                    s.add(m.group(1))
            self.strtypes = s
        return self.strtypes

    def decl(self, fw, name):
        """Liefert (Anweisung vor dem Namen, Text nach dem Namen) der ersten C-Deklaration oder None."""
        t = self.text(fw)
        for src in (t, self.global_text(name)):
            r = self._decl(src, name)
            if r:
                return r
        return None

    def prefetch(self, names):
        """Eine Runde über alle SDK-Header: in welchen kommt welcher Name vor (Umbrella-Header, CoreServices …)."""
        want = set(names)
        self.where = {}
        for base in (f"{self.sdk}/System/Library/Frameworks", f"{self.sdk}/usr/include"):
            for d, _, files in os.walk(base, followlinks=True):
                if "/Versions/" in d or "/Modules" in d:
                    continue
                for f in files:
                    if f.endswith(".h"):
                        p = os.path.join(d, f)
                        try:
                            for n in want.intersection(re.findall(r"[A-Za-z_]\w*", open(p, errors="replace").read())):
                                self.where.setdefault(n, set()).add(p)
                        except OSError:
                            pass

    def global_text(self, name):
        return "\n".join(strip_noise(open(p, errors="replace").read()) for p in sorted(getattr(self, "where", {}).get(name, ()))[:6])

    def _decl(self, t, name):
        for m in re.finditer(r"(?<![\w$])" + re.escape(name) + r"(?![\w$])", t):
            a = max(t.rfind(";", 0, m.start()), t.rfind("}", 0, m.start()), t.rfind("{", 0, m.start())) + 1
            b = t.find(";", m.end())
            if b < 0:
                continue
            pre, post = t[a:m.start()], t[m.end():b]
            if "@interface" in pre or "@protocol" in pre or "@property" in pre or "-(" in pre or "- (" in pre:
                continue
            return pre, post
        return None


def clean_type(pre):
    pre = re.sub(r"\b[A-Z][A-Z0-9_]*\s*\((?:[^()]|\([^()]*\))*\)", " ", pre)          # API_AVAILABLE(...)
    pre = re.sub(r"\b(extern|static|inline|const|volatile|_Nullable|_Nonnull|__nullable|__nonnull|struct|enum|unsigned|"
                 r"__attribute__\s*\(\(.*?\)\)|_Null_unspecified)\b", " ", pre)
    toks = [x for x in re.findall(r"[\w$]+|\*", pre)
            if not (re.fullmatch(r"[A-Z][A-Z0-9_]+", x) and x not in ("BOOL",)) and not x.startswith("__")]
    return toks


def ret_class(toks):
    """Wählt die Rückgabeart aus den Typ-Tokens vor dem Funktionsnamen."""
    if not toks:
        return ("p",)
    if toks[-1] == "*" or "*" in toks:
        return ("p",)
    t = toks[-1]
    if t == "void":
        return ("v",)
    if t in DOUBLE:
        return ("d",)
    if t in FLOAT:
        return ("f",)
    if t in STATUS:
        return ("e", STATUS[t])
    if t in STRUCTS:
        return ("s", STRUCTS[t])
    return ("p",)


def classify(h, fw, sym):
    """-> ('class', Name) | ('swift',) | ('skip', Grund) | ('func', Rückgabeart) | ('str',) | ('data', Init)"""
    n = sym[1:] if sym.startswith("_") else sym
    if n.startswith("OBJC_CLASS_$_"):
        return ("class", n[13:])
    if n.startswith("OBJC_METACLASS_$_"):
        return ("meta", n[17:])
    if n.startswith("OBJC_EHTYPE_$_") or n.startswith("OBJC_IVAR_$_"):
        return ("skip", "ObjC-EH/IVAR")
    if n.startswith("$s") or n.startswith("$S") or n.startswith("_$s"):
        return ("swift",)
    if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", n):
        return ("skip", "Name nicht als C-Bezeichner darstellbar")
    d = h.decl(fw, n) if fw else None
    if d:
        pre, post = d
        if re.search(r"\(\s*\*\s*$", pre) or re.match(r"\s*\)\s*\(", post):
            return ("data", (0, 0, 0))
        if re.match(r"\s*\(", post):
            return ("func", ret_class(clean_type(pre)))
        toks = clean_type(pre)
        if n in CMTIME:
            return ("data", CMTIME[n])
        if ("*" in toks and any(x in h.string_types() for x in toks)) or "CFStringRef" in toks:
            return ("str",)
        return ("data", (0, 0, 0))
    # nicht in den Headern (privat, oder anderer Header): nach dem Namen raten
    if n in CMTIME:
        return ("data", CMTIME[n])
    if re.match(r"k[A-Z]", n):
        return ("str",) if STRINGISH.search(n) else ("data", (0, 0, 0))
    if re.match(r"[A-Z]", n) and re.search(r"(Notification|Key|Domain)$", n):
        return ("str",)
    return ("func", UNKNOWN_RET.get(fw, ("p",)))


# ---------------------------------------------------------------- Quelltext

PRELUDE = r"""// Erzeugt von macapps/stubgen/stubgen.py ({fw}); nicht von Hand ändern.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <objc/runtime.h>
#include <objc/NSObject.h>
#define FW "{fw}"
#define CLK_EXPORT __attribute__((visibility("default"), used))
static void clk_log(const char *what) {{
  static int on = -1;
  if (on < 0) on = getenv("CLOKE_STUB_VERBOSE") != NULL;
  if (on) fprintf(stderr, "[clokeos-stub] %s: %s\n", FW, what);
}}
#define FN_V(n)    CLK_EXPORT void n(void) {{ static char l; if (!l) {{ l = 1; clk_log(#n); }} }}
#define FN_P(n)    CLK_EXPORT void *n(void) {{ static char l; if (!l) {{ l = 1; clk_log(#n); }} return 0; }}
#define FN_D(n)    CLK_EXPORT double n(void) {{ static char l; if (!l) {{ l = 1; clk_log(#n); }} return 0; }}
#define FN_F(n)    CLK_EXPORT float n(void) {{ static char l; if (!l) {{ l = 1; clk_log(#n); }} return 0; }}
#define FN_E(n, e) CLK_EXPORT int n(void) {{ static char l; if (!l) {{ l = 1; clk_log(#n); }} return e; }}
#define FN_S(n, sz) typedef struct {{ char b[sz]; }} clk_s_##n; \
  CLK_EXPORT clk_s_##n n(void) {{ static char l; if (!l) {{ l = 1; clk_log(#n); }} clk_s_##n z; memset(&z, 0, sizeof z); return z; }}
#define DATA(n)    CLK_EXPORT char n[128] __attribute__((aligned(16)));
#define DATA_CM(n, v, t, f) CLK_EXPORT struct {{ int64_t value; int32_t timescale; uint32_t flags; int64_t epoch; char pad[96]; }} n = {{ v, t, f, 0 }};
#define STR(n)     CLK_EXPORT const void *n = __builtin___CFStringMakeConstantString(#n);

// Wurzelklasse aller Stub-Klassen: jede unbekannte Nachricht liefert nil/0
@interface ClokeStubRoot_{fw} : NSObject
@end
@implementation ClokeStubRoot_{fw}
static id clk_nil(id self, SEL _cmd, ...) {{ return 0; }}
+ (BOOL)resolveInstanceMethod:(SEL)s {{
  const char *n = sel_getName(s);
  if (n[0] == '_') return NO;
  if (getenv("CLOKE_STUB_VERBOSE")) fprintf(stderr, "[clokeos-stub] %s: -[%s %s]\n", FW, class_getName(self), n);
  return class_addMethod(self, s, (IMP)clk_nil, "@@:");
}}
+ (BOOL)resolveClassMethod:(SEL)s {{
  const char *n = sel_getName(s);
  if (n[0] == '_') return NO;
  if (getenv("CLOKE_STUB_VERBOSE")) fprintf(stderr, "[clokeos-stub] %s: +[%s %s]\n", FW, class_getName(self), n);
  return class_addMethod(object_getClass(self), s, (IMP)clk_nil, "@@:");
}}
@end
"""


def emit(fw, syms, hdr, override):
    """-> (Quelltext, Zähler, Swift-Liste, übersprungen)"""
    provided = set()
    if override:
        m = re.match(r"//\s*provides:\s*(.*)", override)
        provided = set(m.group(1).split()) if m else set()
    out = [PRELUDE.format(fw=fw)]
    cnt = dict(func=0, str=0, data=0, cls=0, swift=0, skip=0, hand=0)
    swift, skipped, classes = [], [], set()
    for s in sorted(syms):
        n = s[1:] if s.startswith("_") else s
        if n in provided:
            cnt["hand"] += 1
            continue
        k = classify(hdr, fw, s)
        if k[0] == "swift":
            cnt["swift"] += 1; swift.append(s)
        elif k[0] == "skip":
            cnt["skip"] += 1; skipped.append((s, k[1]))
        elif k[0] in ("class", "meta"):
            if k[1] not in classes:
                classes.add(k[1]); cnt["cls"] += 1
                out.append(f"@interface {k[1]} : ClokeStubRoot_{fw}\n@end\n@implementation {k[1]}\n@end\n")
        elif k[0] == "func":
            r = k[1]
            if r[0] == "v": out.append(f"FN_V({n})")
            elif r[0] == "d": out.append(f"FN_D({n})")
            elif r[0] == "f": out.append(f"FN_F({n})")
            elif r[0] == "e": out.append(f"FN_E({n}, {r[1]})")
            elif r[0] == "s": out.append(f"FN_S({n}, {r[1]})")
            else: out.append(f"FN_P({n})")
            cnt["func"] += 1
        elif k[0] == "str":
            out.append(f"STR({n})"); cnt["str"] += 1
        else:
            if n in CMTIME:
                out.append(f"DATA_CM({n}, {k[1][0]}, {k[1][1]}, {k[1][2]})")
            else:
                out.append(f"DATA({n})")
            cnt["data"] += 1
    if override:
        out.append("\n// ---- von Hand (overrides/)\n" + override)
    return "\n".join(out) + "\n", cnt, swift, skipped


# ---------------------------------------------------------------- Bauen

def build(a, fw, info, src, root, pd, cfobj, reexports):
    sdk = sdk_path()
    install = info["install"]
    flat = install.endswith(".dylib")
    target = f"{a.arch}-apple-macos11.0"
    if flat:
        d = os.path.join(root, os.path.dirname(install).lstrip("/"))
        outf = os.path.join(root, install.lstrip("/"))
    else:
        m = re.search(r"/([^/]+)\.framework/Versions/([^/]+)/", install)
        d = os.path.join(root, "Library/ClokeOS/MacFrameworks", f"{m.group(1)}.framework")
        outf = os.path.join(d, "Versions", m.group(2), m.group(1))
    os.makedirs(os.path.dirname(outf), exist_ok=True)
    cmd = ["clang", "-target", target, "-isysroot", sdk, "-O2", "-g0", "-w", "-fno-objc-arc",
           "-dynamiclib", "-nostdlib", "-Wl,-platform_version,macos,11.0,11.5", f"-Wl,-syslibroot,{pd}",
           "-install_name", install, "-compatibility_version", info["compat"], "-current_version", info["current"],
           "-o", outf, "-x", "objective-c", src, "-x", "none"]
    if a.arch == "arm64":
        cmd[cmd.index("-O2"):cmd.index("-O2")] = ["-fno-stack-check"]
        cmd.insert(cmd.index("-dynamiclib"), "-Wl,-no_fixup_chains")
    # Re-Exporte: andere Stubs aus derselben Ausgabe (Reihenfolge der Abhängigkeiten siehe main)
    for r in reexports:
        cmd += [f"-Wl,-reexport_library,{r}"]
    cmd += [cfobj, os.path.join(pd, "usr/lib/libobjc.A.dylib"), os.path.join(pd, "usr/lib/libSystem.B.dylib")]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode or not os.path.isfile(outf):
        sys.stderr.write(f"--- {fw}: Übersetzen fehlgeschlagen\n" + "\n".join(l[:200] for l in r.stderr.splitlines()[:12]) + "\n")
        return None
    if not flat:
        ver = os.path.basename(os.path.dirname(outf))
        res = os.path.join(d, "Versions", ver, "Resources")
        os.makedirs(res, exist_ok=True)
        name = os.path.basename(outf)
        with open(os.path.join(res, "Info.plist"), "w") as f:
            f.write(f'''<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleIdentifier</key><string>com.clokeos.stub.{name}</string>
<key>CFBundleName</key><string>{name}</string>
<key>CFBundleExecutable</key><string>{name}</string>
<key>CFBundlePackageType</key><string>FMWK</string>
<key>CFBundleVersion</key><string>{info["current"]}</string>
<key>ClokeStub</key><true/>
</dict></plist>
''')
        for link, tgt in (("Versions/Current", ver), (name, f"Versions/Current/{name}"), ("Resources", "Versions/Current/Resources")):
            p = os.path.join(d, link)
            if os.path.lexists(p):
                os.remove(p)
            os.symlink(tgt, p)
    return outf


def host_reexports(install):
    r = subprocess.run(["dyld_info", "-dependents", install], capture_output=True, text=True).stdout
    return [l.split()[-1] for l in r.splitlines() if l.strip().startswith("re-export")]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("imports")
    ap.add_argument("out")
    ap.add_argument("--arch", default="arm64", choices=["x86_64", "arm64"])
    ap.add_argument("--only", nargs="*")
    ap.add_argument("--merge", action="append", default=[], metavar="IMPORTS.json",
                    help="weitere Importlisten (z. B. der schon gebauten Mac-Frameworks); ihre Symbole kommen dazu")
    ap.add_argument("--build", nargs=2, metavar=("PDLIBS", "CFOBJ"))
    a = ap.parse_args()
    imp = json.load(open(a.imports))
    for m in a.merge:
        for k, v in json.load(open(m)).items():
            if k in imp:
                imp[k]["symbols"] = sorted(set(imp[k]["symbols"]) | set(v["symbols"]))
    hdr = Headers(sdk_path())
    os.makedirs(os.path.join(a.out, "src"), exist_ok=True)
    allsyms = {x[1:] if x.startswith("_") else x for i in imp.values() for x in i["symbols"]}
    hdr.prefetch(sorted(x for x in allsyms if re.fullmatch(r"[A-Za-z][A-Za-z0-9_]*", x) and not x.startswith("OBJC")))
    root = os.path.join(a.out, "root")
    names = a.only or FRAMEWORKS
    infos = {}
    for fw in names:
        i = imp.get(fw) or {}
        inst, comp, cur = (i.get("install"), i.get("compat"), i.get("current"))
        if not inst:
            inst, comp, cur = FALLBACK.get(fw, (None, None, None))
        if not inst:
            print(f"{fw}: nicht in den Importen und ohne Fallback, übersprungen")
            continue
        infos[fw] = dict(install=inst, compat=comp or "1.0.0", current=cur or "1.0.0")
    # Re-Exporte des Originals, soweit das Ziel ebenfalls ein Stub ist
    re_of = {}
    for fw, i in infos.items():
        targets = [os.path.basename(x) for x in host_reexports(i["install"])] if not i["install"].endswith(".dylib") else []
        re_of[fw] = [t for t in targets if t in infos and t != fw]
    report, swift_all, order = {}, {}, []
    for fw, i in infos.items():
        syms = list((imp.get(fw) or {}).get("symbols", [])) + ["_" + x for x in EXTRAS.get(fw, [])]
        ov = os.path.join(HERE, "overrides", fw + ".c")
        override = open(ov).read() if os.path.isfile(ov) else None
        code, cnt, swift, skipped = emit(fw, set(syms), hdr, override)
        src = os.path.join(a.out, "src", fw + ".c")
        open(src, "w").write(code)
        cnt["total"] = len(set(syms)); cnt["reexports"] = re_of[fw]; cnt["note"] = NOTES.get(fw, "")
        cnt["skipped"] = skipped
        report[fw] = cnt
        swift_all[fw] = swift
        print(f"{fw:26s} {cnt['total']:4d} Symbole: {cnt['func']} Funktionen, {cnt['str']}+{cnt['data']} Konstanten, "
              f"{cnt['cls']} Klassen, {cnt['hand']} von Hand, {cnt['swift']} Swift (offen), {cnt['skip']} übersprungen")
    with open(os.path.join(a.out, "swift-symbols.txt"), "w") as f:
        for fw, l in swift_all.items():
            for s in l:
                f.write(f"{fw}\t{s}\n")
    if a.build:
        pd, cfobj = a.build
        built = {}
        todo = list(infos)
        # Re-Export-Ziele zuerst bauen
        while todo:
            fw = next((x for x in todo if all(r in built or r not in todo for r in re_of[x])), todo[0])
            todo.remove(fw)
            built[fw] = build(a, fw, infos[fw], os.path.join(a.out, "src", fw + ".c"), root, pd, cfobj,
                              [built[r] for r in re_of[fw] if built.get(r)])
            report[fw]["built"] = bool(built[fw])
        print("gebaut:", sum(1 for v in built.values() if v), "von", len(built))
    json.dump(dict(frameworks=infos, report=report), open(os.path.join(a.out, "report.json"), "w"), indent=1)


if __name__ == "__main__":
    main()
