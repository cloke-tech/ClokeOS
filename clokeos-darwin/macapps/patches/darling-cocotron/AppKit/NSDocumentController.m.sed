# ClokeOS: Dokumente aus einer URL wie bei Apple mit -initWithContentsOfURL:ofType:error: anlegen (Fehler kommt beim Aufrufer an);
# die alte Fassung ohne error: verlor den Grund, und Logic Pro zeigte nur "error 256".
/^- makeDocumentWithContentsOfURL: \(NSURL \*\) url$/,/^\}$/s/^    result = \[\[\[class alloc\] initWithContentsOfURL: url$/    result = [[[class alloc] initWithContentsOfURL: url ofType: type error: error] autorelease];\n    return result;\n    result = [[[class alloc] initWithContentsOfURL: url/
