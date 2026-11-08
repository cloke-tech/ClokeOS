# ClokeOS: NSTextAlignment wie bei Apple. Auf arm64 gilt die Reihenfolge von iOS (links 0, Mitte 1, rechts 2, Blocksatz 3, natürlich 4),
# auf x86_64 die alte von macOS (links 0, rechts 1, Mitte 2). Mac-Programme für arm64 übergeben die neuen Werte; Cocotron las 1 als
# "rechts" (zentrierter Text landete rechts). Archive (Nibs, NSCellFlags2, NSParagraphStyle "NSAlignment") speichern auf allen
# Architekturen die alte Reihenfolge; _CLKTextAlignmentFromArchive/_CLKTextAlignmentToArchive rechnen um (auf dem Mac nachgeprüft).
/^    NSLeftTextAlignment,$/,/^\} NSTextAlignment;$/c\
#if defined(__arm64__) || defined(__aarch64__)\
    NSLeftTextAlignment = 0,\
    NSCenterTextAlignment = 1,\
    NSRightTextAlignment = 2,\
#else\
    NSLeftTextAlignment = 0,\
    NSRightTextAlignment = 1,\
    NSCenterTextAlignment = 2,\
#endif\
    NSJustifiedTextAlignment = 3,\
    NSNaturalTextAlignment = 4\
} NSTextAlignment;\
#define NSTextAlignmentLeft NSLeftTextAlignment\
#define NSTextAlignmentCenter NSCenterTextAlignment\
#define NSTextAlignmentRight NSRightTextAlignment\
#define NSTextAlignmentJustified NSJustifiedTextAlignment\
#define NSTextAlignmentNatural NSNaturalTextAlignment\
/* Archivwert (alte Reihenfolge: links 0, rechts 1, Mitte 2) <-> NSTextAlignment */\
static inline NSTextAlignment _CLKTextAlignmentFromArchive(unsigned int v) {\
    switch (v) {\
    case 1: return NSRightTextAlignment;\
    case 2: return NSCenterTextAlignment;\
    case 3: return NSJustifiedTextAlignment;\
    case 4: return NSNaturalTextAlignment;\
    default: return NSLeftTextAlignment;\
    }\
}\
static inline unsigned int _CLKTextAlignmentToArchive(NSTextAlignment a) {\
    switch (a) {\
    case NSRightTextAlignment: return 1;\
    case NSCenterTextAlignment: return 2;\
    case NSJustifiedTextAlignment: return 3;\
    case NSNaturalTextAlignment: return 4;\
    default: return 0;\
    }\
}
