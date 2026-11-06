/* ImageIO für ClokeOS: CGImageSource und CGImageDestination über Onyx2D.
 *
 * Darling hat dafür ein eigenes ImageIO.framework im Hauptrepo (nicht geladen). Onyx2D bringt
 * mit O2ImageSource und O2ImageDestination schon alles mit (PNG, JPEG, TIFF, BMP, ICNS lesen;
 * PNG, JPEG, TIFF schreiben). Diese Datei gibt dem Apples Namen und Install-Namen
 * (/System/Library/Frameworks/ImageIO.framework/Versions/A/ImageIO), AppKit (NSBitmapImageRep)
 * und Programme binden daran. CGImageRef ist in Darlings CoreGraphics ein O2Image.
 *
 * Die Konstanten tragen Apples Werte; Onyx2D benutzt absichtlich dieselben Zeichenketten. */
#import <ImageIO/ImageIO.h>
#import <Onyx2D/O2DataProvider.h>
#import <Onyx2D/O2ImageDestination.h>
#import <Onyx2D/O2ImageSource.h>

const CFStringRef kCGImagePropertyDPIWidth = CFSTR("DPIWidth");
const CFStringRef kCGImagePropertyDPIHeight = CFSTR("DPIHeight");
const CFStringRef kCGImagePropertyPixelWidth = CFSTR("PixelWidth");
const CFStringRef kCGImagePropertyPixelHeight = CFSTR("PixelHeight");
const CFStringRef kCGImagePropertyOrientation = CFSTR("Orientation");
const CFStringRef kCGImagePropertyTIFFDictionary = CFSTR("{TIFF}");
const CFStringRef kCGImagePropertyExifDictionary = CFSTR("{Exif}");
const CFStringRef kCGImagePropertyTIFFXResolution = CFSTR("XResolution");
const CFStringRef kCGImagePropertyTIFFYResolution = CFSTR("YResolution");
const CFStringRef kCGImagePropertyTIFFOrientation = CFSTR("Orientation");
const CFStringRef kCGImagePropertyTIFFImageDescription = CFSTR("ImageDescription");
const CFStringRef kCGImagePropertyTIFFSoftware = CFSTR("Software");
const CFStringRef kCGImageDestinationLossyCompressionQuality = CFSTR("kCGImageDestinationLossyCompressionQuality");
const CFStringRef kCGImageDestinationBackgroundColor = CFSTR("kCGImageDestinationBackgroundColor");
const CFStringRef kCGImageDestinationDPI = CFSTR("kCGImageDestinationDPI");

#define SRC(s) ((O2ImageSource *) (s))
#define DST(d) ((O2ImageDestination *) (d))

/* --- Lesen ------------------------------------------------------------------------------------ */
CGImageSourceRef CGImageSourceCreateWithData(CFDataRef data, CFDictionaryRef options) {
    if (data == NULL)
        return NULL;
    return (CGImageSourceRef)[O2ImageSource newImageSourceWithData: data options: options];
}

CGImageSourceRef CGImageSourceCreateWithURL(CFURLRef url, CFDictionaryRef options) {
    if (url == NULL)
        return NULL;
    return (CGImageSourceRef)[O2ImageSource newImageSourceWithURL: (NSURL *) url
                                                          options: (NSDictionary *) options];
}

CGImageSourceRef CGImageSourceCreateWithDataProvider(CGDataProviderRef provider, CFDictionaryRef options) {
    if (provider == NULL)
        return NULL;
    return (CGImageSourceRef)[O2ImageSource newImageSourceWithDataProvider: (O2DataProvider *) provider
                                                                   options: options];
}

CFStringRef CGImageSourceGetType(CGImageSourceRef isrc) {
    return isrc ? [SRC(isrc) type] : NULL;
}

size_t CGImageSourceGetCount(CGImageSourceRef isrc) {
    return isrc ? [SRC(isrc) count] : 0;
}

CFDictionaryRef CGImageSourceCopyProperties(CGImageSourceRef isrc, CFDictionaryRef options) {
    // Eigenschaften der ganzen Datei kennt Onyx2D nicht, nur die je Bild.
    return isrc ? (CFDictionaryRef)[[NSDictionary alloc] init] : NULL;
}

CFDictionaryRef CGImageSourceCopyPropertiesAtIndex(CGImageSourceRef isrc, size_t index, CFDictionaryRef options) {
    if (isrc == NULL || index >= [SRC(isrc) count])
        return NULL;
    return [SRC(isrc) copyPropertiesAtIndex: index options: options];
}

CGImageRef CGImageSourceCreateImageAtIndex(CGImageSourceRef isrc, size_t index, CFDictionaryRef options) {
    if (isrc == NULL || index >= [SRC(isrc) count])
        return NULL;
    return (CGImageRef) O2ImageSourceCreateImageAtIndex(SRC(isrc), index, options);
}

CFArrayRef CGImageSourceCopyTypeIdentifiers(void) {
    return (CFArrayRef)[[NSArray alloc] initWithObjects: @"public.png", @"public.jpeg", @"public.tiff",
                                                          @"com.microsoft.bmp", @"com.apple.icns", nil];
}

/* --- Schreiben -------------------------------------------------------------------------------- */
CGImageDestinationRef CGImageDestinationCreateWithData(CFMutableDataRef data, CFStringRef type, size_t count,
                                                       CFDictionaryRef options) {
    return (CGImageDestinationRef) O2ImageDestinationCreateWithData(data, type, count, options);
}

CGImageDestinationRef CGImageDestinationCreateWithURL(CFURLRef url, CFStringRef type, size_t count,
                                                      CFDictionaryRef options) {
    return (CGImageDestinationRef) O2ImageDestinationCreateWithURL(url, type, count, options);
}

void CGImageDestinationSetProperties(CGImageDestinationRef idst, CFDictionaryRef properties) {
    O2ImageDestinationSetProperties(DST(idst), properties);
}

void CGImageDestinationAddImage(CGImageDestinationRef idst, CGImageRef image, CFDictionaryRef properties) {
    O2ImageDestinationAddImage(DST(idst), (O2ImageRef) image, properties);
}

void CGImageDestinationAddImageFromSource(CGImageDestinationRef idst, CGImageSourceRef isrc, size_t index,
                                          CFDictionaryRef properties) {
    O2ImageDestinationAddImageFromSource(DST(idst), SRC(isrc), index, properties);
}

bool CGImageDestinationFinalize(CGImageDestinationRef idst) {
    return O2ImageDestinationFinalize(DST(idst));
}

CFArrayRef CGImageDestinationCopyTypeIdentifiers(void) {
    return O2ImageDestinationCopyTypeIdentifiers();
}
