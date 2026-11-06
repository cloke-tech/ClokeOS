# ClokeOS: eine URL ohne Endung hat keine Pfadendung (NULL); nicht freigeben, kein Typbezeichner (Logic Pro, Ordner ohne Endung beim
# Öffnen eines Projekts: CFRelease(NULL) -> Absturz). Wird erst erreicht, seit es LaunchServices gibt (macapps/launchservices.c).
s/^(        CFStringRef extension = CFURLCopyPathExtension\(url\);)$/\1\n        if (extension == NULL)\n            return NULL;/
