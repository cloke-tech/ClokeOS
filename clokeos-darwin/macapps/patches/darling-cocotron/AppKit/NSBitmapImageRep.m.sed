# ClokeOS: bitmapImageRepByRetaggingWithColorSpace: gibt dieselben Pixel zurück (Farbräume verwaltet Cocotron nicht), statt nil.
# Logic Pros MAToolKit versieht seine Knopfbilder so mit sRGB; mit nil fehlten alle Knopfbilder (Ausnahmen beim Zeichnen).
/^- \(NSBitmapImageRep \*\) bitmapImageRepByRetaggingWithColorSpace: \(NSColorSpace \*\) newSpace \{$/,/^\}$/{
s/^    printf\("STUB %s\\n", __PRETTY_FUNCTION__\);$//
s/^    return nil;$/    return [[self retain] autorelease];/
}
