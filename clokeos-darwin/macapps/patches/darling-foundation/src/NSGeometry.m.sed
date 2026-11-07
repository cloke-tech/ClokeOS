# ClokeOS: NSPointFromString/NSSizeFromString/NSRectFromString lesen wie bei Apple einfach die Zahlen der Reihe nach und überspringen alles
# andere (Klammern, Kommas, Buchstaben); fehlende Werte sind 0 (auf dem Mac nachgeprüft: "{60,60,60,100}" -> Rechteck 60 60 60 100,
# "{20,100}" -> Punkt 20 100, "x=3 y=4" -> 3 4). Darling verlangte genau "{{x, y}, {w, h}}" und las Floats in CGFloat-Felder;
# alles andere ergab 0. Logic Pro liest so alle Farben seiner Assets ("{r,g,b,a}", "{grau,alpha}"): sie wurden durchsichtig.
s|^#import <math.h>$|#import <math.h>\
#include <stdlib.h>|
/^NSPoint NSPointFromString\(NSString\* string\)$/,/^\}$/c\
static unsigned clk_geo_numbers(NSString *string, CGFloat *out, unsigned max) {\
    for (unsigned i = 0; i < max; i++)\
        out[i] = 0;\
    const char *s = [string UTF8String];\
    unsigned n = 0;\
    while (s && *s && n < max) {\
        BOOL start = (*s >= '0' && *s <= '9') ||\
                     ((*s == '-' || *s == '+' || *s == '.') && ((s[1] >= '0' && s[1] <= '9') || (s[1] == '.' && s[2] >= '0' && s[2] <= '9')));\
        if (start) {\
            char *end;\
            double v = strtod(s, &end);\
            if (end > s) {\
                out[n++] = v;\
                s = end;\
                continue;\
            }\
        }\
        s++;\
    }\
    return n;\
}\
\
NSPoint NSPointFromString(NSString* string)\
{\
    CGFloat v[2];\
    clk_geo_numbers(string, v, 2);\
    return NSMakePoint(v[0], v[1]);\
}
/^NSSize NSSizeFromString\(NSString\* string\)$/,/^\}$/c\
NSSize NSSizeFromString(NSString* string)\
{\
    CGFloat v[2];\
    clk_geo_numbers(string, v, 2);\
    return NSMakeSize(v[0], v[1]);\
}
/^NSRect NSRectFromString\(NSString\* string\)$/,/^\}$/c\
NSRect NSRectFromString(NSString* string)\
{\
    CGFloat v[4];\
    clk_geo_numbers(string, v, 4);\
    return NSMakeRect(v[0], v[1], v[2], v[3]);\
}
