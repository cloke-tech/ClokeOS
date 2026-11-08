# ClokeOS: diese vier waren STUBs ohne Wirkung; macapps/appkit/cggradient.m setzt sie um (zusammen mit CGGradient)
/^void CGContextDrawLinearGradient\(CGContextRef c,$/,/^\}$/d
/^void CGContextDrawRadialGradient\(CGContextRef c,$/,/^\}$/d
/^void CGContextDrawTiledImage\(CGContextRef c, CGRect rect, CGImageRef image\)$/,/^\}$/d
/^void CGContextShowGlyphsAtPositions\(CGContextRef c,$/,/^\}$/d
# ClokeOS: CLOKE_CGCALL_TRACE=1 meldet die ersten 400 Bild-, Masken-, Mischmodus- und Ebenenaufrufe aus dem Programm (nicht aus den Mac-Frameworks) mit Aufrufer (Bild + Versatz),
# Rechteck und Bildgröße (Fehlersuche: was ein Programm zum Zeichnen eines Symbols tut)
s|^#import <CoreGraphics/CGContext.h>$|#import <CoreGraphics/CGContext.h>\n#import <dlfcn.h>\n#import <stdio.h>\n#import <stdlib.h>\n#import <string.h>\nstatic void clk_cgcall(const char *what, void *ra, CGRect r, CGImageRef img, long extra) {\n    static int on = -1, n;\n    if (on < 0) on = getenv("CLOKE_CGCALL_TRACE") != NULL;\n    if (!on \|\| n >= 400) return;\n    Dl_info di;\n    const char *m = "?";\n    unsigned long off = 0;\n    if (dladdr(ra, \&di) \&\& di.dli_fname) { m = strrchr(di.dli_fname, '/') ? strrchr(di.dli_fname, '/') + 1 : di.dli_fname; off = (unsigned long) ((char *) ra - (char *) di.dli_fbase); }\n    if (strstr(di.dli_fname ? di.dli_fname : "", "/Library/ClokeOS/MacFrameworks/")) return;\n    n++;\n    fprintf(stderr, "[clokeos-cg] %s %.0f,%.0f %.0fx%.0f img=%zux%zu x=%ld from %s+%#lx\\n", what, r.origin.x, r.origin.y, r.size.width, r.size.height,\n            img ? CGImageGetWidth(img) : 0, img ? CGImageGetHeight(img) : 0, extra, m, off);\n}|
/^void CGContextDrawImage\(CGContextRef context, CGRect rect, CGImageRef image\) \{$/a\
    clk_cgcall("DrawImage", __builtin_return_address(0), rect, image, 0);
/^void CGContextClipToMask\(CGContextRef context, CGRect rect, CGImageRef image\) \{$/a\
    clk_cgcall("ClipToMask", __builtin_return_address(0), rect, image, 0);
/^void CGContextSetBlendMode\(CGContextRef context, CGBlendMode blendMode\) \{$/a\
    clk_cgcall("SetBlendMode", __builtin_return_address(0), CGRectZero, NULL, (long) blendMode);
# ClokeOS: CLOKE_CGCALL_TRACE meldet auch CGContextSelectFont (Name, Größe) und CGContextShowText (Text), höchstens 300
/^void CGContextSelectFont\(CGContextRef context, const char \*name, CGFloat size,$/,/^\{$/s/^\{$/{\
    if (getenv("CLOKE_CGCALL_TRACE")) {\
        static int clk_n;\
        if (clk_n++ < 300)\
            fprintf(stderr, "[clokeos-cg] SelectFont %s %.1f enc=%d\\n", name ? name : "(null)", (double) size, (int) encoding);\
    }/
/^void CGContextShowText\(CGContextRef context, const char \*text, unsigned count\) \{$/a\
    if (getenv("CLOKE_CGCALL_TRACE")) {\
        static int clk_n;\
        if (clk_n++ < 300)\
            fprintf(stderr, "[clokeos-cg] ShowText \\"%.*s\\"\\n", (int) (count < 60 ? count : 60), text ? text : "");\
    }
