# ClokeOS: CTFontManagerRegisterFontsForURL trägt die Schriftdatei in Onyx2Ds fontconfig-Konfiguration ein (dieselbe, aus der
# +[NSFont fontWithName:size:] sucht). Logic Pro registriert so seine eigenen Schriften (MACore/Resources/*.ttf, z. B. für die
# Transportanzeige); als Platzhalter (false) blieb die Schrift nil, und Logic warf beim Öffnen eines Projekts eine Ausnahme.
# fontconfig und Onyx2D werden über dlsym gefunden (beide sind in jedem AppKit-Prozess geladen), CoreText bindet sie nicht.
s/^(#import <CoreText\/CTFontManager.h>)$/\1\n#include <dlfcn.h>\n#include <limits.h>\nstatic bool clk_register_font(CFURLRef url, bool add) {\n    char path[PATH_MAX];\n    if (url == NULL || !CFURLGetFileSystemRepresentation(url, true, (UInt8 *) path, sizeof path))\n        return false;\n    void *(*shared)(void) = (void *(*)(void)) dlsym(RTLD_DEFAULT, "O2FontSharedFontConfig");\n    int (*addFile)(void *, const unsigned char *) = (int (*)(void *, const unsigned char *)) dlsym(RTLD_DEFAULT, "FcConfigAppFontAddFile");\n    if (!add)\n        return true;   \/* Abmelden kennt fontconfig nicht; die Schrift bleibt bis zum Prozessende *\/\n    if (shared == NULL || addFile == NULL)\n        return false;\n    return addFile(shared(), (const unsigned char *) path) != 0;\n}/
/^bool CTFontManagerRegisterFontsForURL\(CFURLRef fontURL, CTFontManagerScope scope, CFErrorRef \* error\)$/,/^\}$/{
s/^    printf\("STUB %s\\n", __PRETTY_FUNCTION__\);$/    if (error) *error = NULL;/
s/^    return false;$/    return clk_register_font(fontURL, true);/
}
/^bool CTFontManagerUnregisterFontsForURL\(CFURLRef fontURL, CTFontManagerScope scope, CFErrorRef \* error\)$/,/^\}$/{
s/^    printf\("STUB %s\\n", __PRETTY_FUNCTION__\);$/    if (error) *error = NULL;/
s/^    return false;$/    return clk_register_font(fontURL, false);/
}
