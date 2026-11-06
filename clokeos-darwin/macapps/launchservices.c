/* LaunchServices für Mac-Programme auf ClokeOS (nur der Teil, den Darlings CoreFoundation braucht).
 *
 * CFURL fragt den Typbezeichner einer Datei (kCFURLTypeIdentifierKey, NSURLTypeIdentifierKey) über
 * /System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/LaunchServices ab, per dlopen/dlsym:
 * UTTypeCreatePreferredIdentifierForTag und kUTTagClassFilenameExtension. PureDarwin hat diese Bibliothek nicht; dlopen schlug fehl,
 * und Logic Pro meldete beim Öffnen eines Projekts "No such file or directory".
 *
 * Zuordnung Endung -> Typ: zuerst die Typen, die das laufende Programm in seiner Info.plist erklärt (UTExportedTypeDeclarations,
 * UTImportedTypeDeclarations, CFBundleDocumentTypes/LSItemContentTypes), dann eine kleine Tabelle üblicher Systemtypen, sonst wie bei
 * Apple ein dynamischer Bezeichner "dyn.…" (hier lesbar: dyn.ext-<Endung>). CoreFoundation wird erst zur Laufzeit gefunden
 * (-undefined dynamic_lookup), die Bibliothek bindet nur libSystem. */
#include <CoreFoundation/CoreFoundation.h>
#include <string.h>

const CFStringRef kUTTagClassFilenameExtension = CFSTR("public.filename-extension");
const CFStringRef kUTTagClassMIMEType = CFSTR("public.mime-type");

static const char *known[][2] = {
    {"txt", "public.plain-text"}, {"text", "public.plain-text"}, {"rtf", "public.rtf"}, {"html", "public.html"},
    {"htm", "public.html"}, {"xml", "public.xml"}, {"plist", "com.apple.property-list"}, {"json", "public.json"},
    {"png", "public.png"}, {"jpg", "public.jpeg"}, {"jpeg", "public.jpeg"}, {"tif", "public.tiff"}, {"tiff", "public.tiff"},
    {"gif", "com.compuserve.gif"}, {"pdf", "com.adobe.pdf"}, {"wav", "com.microsoft.waveform-audio"},
    {"aif", "public.aiff-audio"}, {"aiff", "public.aiff-audio"}, {"caf", "com.apple.coreaudio-format"}, {"mp3", "public.mp3"},
    {"m4a", "com.apple.m4a-audio"}, {"mid", "public.midi-audio"}, {"midi", "public.midi-audio"}, {"mov", "com.apple.quicktime-movie"},
    {"mp4", "public.mpeg-4"}, {"zip", "public.zip-archive"}, {"app", "com.apple.application-bundle"}, {"bundle", "com.apple.bundle"},
    {"framework", "com.apple.framework"}, {"component", "com.apple.audio-unit"}, {"dylib", "com.apple.mach-o-dylib"},
};

static Boolean has_tag(CFTypeRef tags, CFStringRef tag) {
    if (!tags) return false;
    if (CFGetTypeID(tags) == CFStringGetTypeID())
        return CFStringCompare((CFStringRef)tags, tag, kCFCompareCaseInsensitive) == kCFCompareEqualTo;
    if (CFGetTypeID(tags) == CFArrayGetTypeID())
        for (CFIndex i = 0; i < CFArrayGetCount((CFArrayRef)tags); i++)
            if (has_tag(CFArrayGetValueAtIndex((CFArrayRef)tags, i), tag)) return true;
    return false;
}

/* Typ aus der Info.plist des Programms; NULL wenn keiner passt */
static CFStringRef from_main_bundle(CFStringRef tagClass, CFStringRef tag) {
    CFBundleRef b = CFBundleGetMainBundle();
    CFDictionaryRef info = b ? CFBundleGetInfoDictionary(b) : NULL;
    if (!info) return NULL;
    const char *decls[] = {"UTExportedTypeDeclarations", "UTImportedTypeDeclarations"};
    for (int d = 0; d < 2; d++) {
        CFStringRef k = CFStringCreateWithCString(NULL, decls[d], kCFStringEncodingUTF8);
        CFArrayRef arr = CFDictionaryGetValue(info, k);
        CFRelease(k);
        if (!arr || CFGetTypeID(arr) != CFArrayGetTypeID()) continue;
        for (CFIndex i = 0; i < CFArrayGetCount(arr); i++) {
            CFDictionaryRef t = CFArrayGetValueAtIndex(arr, i);
            if (CFGetTypeID(t) != CFDictionaryGetTypeID()) continue;
            CFDictionaryRef spec = CFDictionaryGetValue(t, CFSTR("UTTypeTagSpecification"));
            CFStringRef ident = CFDictionaryGetValue(t, CFSTR("UTTypeIdentifier"));
            if (spec && ident && CFGetTypeID(spec) == CFDictionaryGetTypeID() && has_tag(CFDictionaryGetValue(spec, tagClass), tag))
                return CFRetain(ident);
        }
    }
    if (!CFEqual(tagClass, kUTTagClassFilenameExtension)) return NULL;
    CFArrayRef docs = CFDictionaryGetValue(info, CFSTR("CFBundleDocumentTypes"));
    if (docs && CFGetTypeID(docs) == CFArrayGetTypeID())
        for (CFIndex i = 0; i < CFArrayGetCount(docs); i++) {
            CFDictionaryRef t = CFArrayGetValueAtIndex(docs, i);
            if (CFGetTypeID(t) != CFDictionaryGetTypeID() || !has_tag(CFDictionaryGetValue(t, CFSTR("CFBundleTypeExtensions")), tag)) continue;
            CFArrayRef types = CFDictionaryGetValue(t, CFSTR("LSItemContentTypes"));
            if (types && CFGetTypeID(types) == CFArrayGetTypeID() && CFArrayGetCount(types) > 0)
                return CFRetain(CFArrayGetValueAtIndex(types, 0));
        }
    return NULL;
}

CFStringRef UTTypeCreatePreferredIdentifierForTag(CFStringRef tagClass, CFStringRef tag, CFStringRef conformingTo) {
    (void)conformingTo;
    if (!tagClass || !tag) return NULL;
    CFStringRef r = from_main_bundle(tagClass, tag);
    if (r) return r;
    char ext[64] = "";
    CFStringGetCString(tag, ext, sizeof ext, kCFStringEncodingUTF8);
    if (CFEqual(tagClass, kUTTagClassFilenameExtension))
        for (size_t i = 0; i < sizeof known / sizeof *known; i++)
            if (!strcasecmp(ext, known[i][0])) return CFStringCreateWithCString(NULL, known[i][1], kCFStringEncodingUTF8);
    return CFStringCreateWithFormat(NULL, NULL, CFSTR("dyn.ext-%@"), tag);
}
