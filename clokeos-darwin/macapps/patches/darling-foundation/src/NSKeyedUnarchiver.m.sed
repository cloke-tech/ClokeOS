# ClokeOS: versionForClassName: liefert wie bei Apple die Version der Klasse im Prozess (Schlüsselarchive speichern keine), statt in eine
# Debug-Falle (brk) zu laufen. Logic Pros MACore-Klassen fragen danach beim Laden eines Projekts.
/^- \(NSInteger\)versionForClassName:\(NSString \*\)className$/,/^\}$/{
s/^    DEBUG_BREAK\(\);$/    Class cls = NSClassFromString(className);\n    if (cls != Nil)\n        return [cls version];/
}
