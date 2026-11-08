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
# … und CGContextShowGlyphsWithAdvances (Anzahl, erste Glyphe, Textposition, CTM), höchstens 300
/^void CGContextShowGlyphsWithAdvances\(CGContextRef context,$/,/^\{$/s/^\{$/{\
    if (getenv("CLOKE_CGCALL_TRACE")) {\
        static int clk_n;\
        Dl_info clk_di;\
        void *clk_ra = __builtin_return_address(0);\
        BOOL clk_own = dladdr(clk_ra, \&clk_di) \&\& clk_di.dli_fname \&\& strstr(clk_di.dli_fname, "ClokeOS\/MacFrameworks");\
        if (!clk_own \&\& clk_n++ < 300) {\
            CGPoint clk_p = CGContextGetTextPosition(context);\
            CGAffineTransform clk_m = CGContextGetCTM(context);\
            CGRect clk_cb = CGContextGetClipBoundingBox(context);\
            O2ColorRef clk_fc = O2ContextFillColor((O2ContextRef) context);\
            const O2Float *clk_k = clk_fc ? O2ColorGetComponents(clk_fc) : NULL;\
            size_t clk_kn = clk_fc ? O2ColorGetNumberOfComponents(clk_fc) : 0;\
            O2FontRef clk_f = O2GStateFont(O2ContextCurrentGState((O2ContextRef) context));\
            CFStringRef clk_fn = clk_f ? CGFontCopyFullName((CGFontRef) clk_f) : NULL;\
            fprintf(stderr, "[clokeos-cg]   font %s\\n", clk_fn ? [(NSString *) clk_fn UTF8String] : "(keine)");\
            if (clk_fn) CFRelease(clk_fn);\
            fprintf(stderr, "[clokeos-cg]   clip %.0f,%.0f %.0fx%.0f fill n=%zu %.2f %.2f %.2f %.2f\\n", clk_cb.origin.x, clk_cb.origin.y, clk_cb.size.width,\
                    clk_cb.size.height, clk_kn, clk_kn > 0 ? clk_k[0] : -1.0, clk_kn > 1 ? clk_k[1] : -1.0, clk_kn > 2 ? clk_k[2] : -1.0, clk_kn > 3 ? clk_k[3] : -1.0);\
            fprintf(stderr, "[clokeos-cg] ShowGlyphsWithAdvances n=%u g0=%u adv0=%.1f at %.1f,%.1f ctm=[%.2f %.2f %.2f %.2f %.1f %.1f] from %p\\n", count,\
                    count ? glyphs[0] : 0, (count \&\& advances) ? advances[0].width : -1.0, clk_p.x, clk_p.y, clk_m.a, clk_m.b, clk_m.c, clk_m.d,\
                    clk_m.tx, clk_m.ty, clk_ra);\
        }\
    }/
s|^#import <Onyx2D/O2Context.h>$|#import <Onyx2D/O2Context.h>\
#import <Onyx2D/O2GraphicsState.h>|
# … CLOKE_FILL_BT=1: CGContextFillRect meldet die ersten 6 Rücksprünge in Logic (bzw. außerhalb der ClokeOS-Frameworks), höchstens 300
/^void CGContextFillRect\(CGContextRef context, CGRect rect\) \{$/a\
    if (getenv("CLOKE_FILL_BT")) {\
        static int clk_n;\
        if (clk_n < 300) {\
            void *clk_pcs[40]; int clk_k = backtrace(clk_pcs, 40);\
            char clk_buf[1400]; int clk_len = 0, clk_got = 0;\
            for (int clk_i = 1; clk_i < clk_k \&\& clk_got < 14; clk_i++) {\
                Dl_info clk_di;\
                if (!dladdr(clk_pcs[clk_i], \&clk_di) || !clk_di.dli_fname || strstr(clk_di.dli_fname, "ClokeOS\/MacFrameworks")) continue;\
                clk_len += snprintf(clk_buf + clk_len, sizeof clk_buf - clk_len, " %s+%#lx", strrchr(clk_di.dli_fname, '\/') + 1, (unsigned long) ((char *) clk_pcs[clk_i] - (char *) clk_di.dli_fbase));\
                clk_got++;\
            }\
            if (clk_got) { clk_n++; fprintf(stderr, "[clokeos-fill] %.0fx%.0f%s\\n", rect.size.width, rect.size.height, clk_buf); }\
        }\
    }
s|#import <dlfcn.h>|#import <dlfcn.h>\
#import <execinfo.h>|
