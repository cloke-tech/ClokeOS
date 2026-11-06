# ClokeOS: CTFontCreateWithName(AndOptions) liefert ein NSFont (bei Apple sind CTFont und NSFont dasselbe Objekt; Logic Pro reicht
# CTFonts als NSFontAttributeName weiter und NSFonts an CT-Funktionen). Wie bei Apple nie NULL: unbekannte Namen ergeben die
# Systemschrift. NSFont wird zur Laufzeit gesucht (CoreText bindet AppKit nicht); die von CTFont*-Abfragen benutzten KTFont-Methoden
# (copyName, boundingRect) ergänzt macapps/appkit/modern.m an NSFont. CTFontCopyPostScriptName nimmt fontName.
s/^(#import <CoreText\/CTFont.h>)$/\1\n#import <objc\/runtime.h>\n#import <objc\/message.h>\nstatic CTFontRef clk_nsfont(CFStringRef name, CGFloat size) {\n    Class nsfont = objc_getClass("NSFont");\n    if (nsfont == Nil)\n        return NULL;\n    if (size <= 0)\n        size = 12;\n    id f = name ? ((id (*)(id, SEL, id, CGFloat)) objc_msgSend)(nsfont, sel_registerName("fontWithName:size:"), (id) name, size) : nil;\n    if (f == nil)\n        f = ((id (*)(id, SEL, CGFloat)) objc_msgSend)(nsfont, sel_registerName("systemFontOfSize:"), size);\n    return (CTFontRef) CFRetain((CFTypeRef) f);\n}/
/^CTFontRef CTFontCreateWithName\(CFStringRef name, CGFloat size, const CGAffineTransform \*matrix\)$/,/^\}$/{
s/^    printf\("STUB %s\\n", __PRETTY_FUNCTION__\);$//
s/^    return nil;$/    return clk_nsfont(name, size);/
}
/^CTFontRef CTFontCreateWithNameAndOptions\(CFStringRef name, CGFloat size,$/,/^\}$/{
s/^    printf\("STUB %s\\n", __PRETTY_FUNCTION__\);$//
s/^    return nil;$/    return clk_nsfont(name, size);/
}
/^CFStringRef CTFontCopyPostScriptName\(CTFontRef font\)$/,/^\}$/{
s/^    printf\("STUB %s\\n", __PRETTY_FUNCTION__\);$//
s/^    return nil;$/    id f = (id) font;\n    if ([f respondsToSelector: sel_registerName("fontName")])\n        return (CFStringRef) [((id (*)(id, SEL)) objc_msgSend)(f, sel_registerName("fontName")) copy];\n    return [font copyName];/
}
