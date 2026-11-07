# ClokeOS: bitmapImageRepByRetaggingWithColorSpace: gibt dieselben Pixel zurück (Farbräume verwaltet Cocotron nicht), statt nil.
# Logic Pros MAToolKit versieht seine Knopfbilder so mit sRGB; mit nil fehlten alle Knopfbilder (Ausnahmen beim Zeichnen).
/^- \(NSBitmapImageRep \*\) bitmapImageRepByRetaggingWithColorSpace: \(NSColorSpace \*\) newSpace \{$/,/^\}$/{
s/^    printf\("STUB %s\\n", __PRETTY_FUNCTION__\);$//
s/^    return nil;$/    return [[self retain] autorelease];/
}
# ClokeOS: initWithCGImage: mit NULL liefert nil (wie bei Apple); Bilder ohne Farbraum (Masken) haben einen Anteil je Pixel, statt
# CGColorSpaceGetNumberOfComponents(NULL) aufzurufen (Absturz, als Logic Pro nach dem Laden eines Projekts Bilder vorbereitete).
s/^(- initWithCGImage: \(CGImageRef\) cgImage \{)$/\1\n    if (cgImage == NULL) {\n        [self release];\n        return nil;\n    }/
s/^    _samplesPerPixel = CGColorSpaceGetNumberOfComponents\(colorSpace\);$/    _samplesPerPixel = colorSpace ? CGColorSpaceGetNumberOfComponents(colorSpace) : 1;/
