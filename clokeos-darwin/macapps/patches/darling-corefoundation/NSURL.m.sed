# ClokeOS: eine URL ohne Endung hat keine Pfadendung (NULL); nicht freigeben, kein Typbezeichner (Logic Pro, Ordner ohne Endung beim
# Öffnen eines Projekts: CFRelease(NULL) -> Absturz). Wird erst erreicht, seit es LaunchServices gibt (macapps/launchservices.c).
s/^(        CFStringRef extension = CFURLCopyPathExtension\(url\);)$/\1\n        if (extension == NULL)\n            return NULL;/
# ClokeOS: Lesezeichen (Bookmarks) und CFURLResourceIsReachable kommen aus macapps/cfcompat.m; Darlings Platzhalter (immer NULL/false) werden
# umbenannt. Logic Pro findet seine Klangbibliothek nur über ein Lesezeichen (Logic Pro Library.bookmark).
s/^CFDataRef CFURLCreateBookmarkDataFromFile\(/static CFDataRef clk_darling_stub_fromfile(/
s/^Boolean CFURLWriteBookmarkDataToFile\(/static Boolean clk_darling_stub_tofile(/
s/^CFURLRef CFURLCreateByResolvingBookmarkData\(/static CFURLRef clk_darling_stub_resolve(/
s/^CFDataRef CFURLCreateBookmarkData\(/static CFDataRef clk_darling_stub_create(/
s/^Boolean CFURLResourceIsReachable\(/static Boolean clk_darling_stub_reachable(/
