# CFURLEnumeratorPushURL öffnete den Ordner mit dem URL-Pfad (CFURLCopyPath), der noch
# Prozent-kodiert ist: "/Library/Application%20Support/…" gibt es nicht, also fand
# -[NSFileManager enumeratorAtURL:…] in Ordnern mit Leerzeichen nichts (Logic fand so keine
# Sampler-Instrumente). Den Dateisystempfad der URL nehmen; urlPath bleibt für die Fehlermeldung.
s/Boolean success = CFStringGetFileSystemRepresentation\(urlPath, path, PATH_MAX\);/Boolean success = CFURLGetFileSystemRepresentation(url, true, (UInt8 *)path, PATH_MAX);/
