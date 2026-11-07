# ClokeOS: Dokumente aus einer URL wie bei Apple mit -initWithContentsOfURL:ofType:error: anlegen (Fehler kommt beim Aufrufer an);
# die alte Fassung ohne error: verlor den Grund, und Logic Pro zeigte nur "error 256".
/^- makeDocumentWithContentsOfURL: \(NSURL \*\) url$/,/^\}$/s/^    result = \[\[\[class alloc\] initWithContentsOfURL: url$/    result = [[[class alloc] initWithContentsOfURL: url ofType: type error: error] autorelease];\n    return result;\n    result = [[[class alloc] initWithContentsOfURL: url/
# ClokeOS: Dokumenttypen sind wie bei Apple (seit 10.5) die UTIs aus LSItemContentTypes, wenn die Info.plist sie angibt; der alte Name
# (CFBundleTypeName) gilt weiter. Logic Pro prüft den Typ mit type:conformsToType: gegen UTIs und lud mit "Logic Pro Project" kein Projekt.
s/^(        if \(\[name isEqualToString: type\]\))$/        if ([name isEqualToString: type] || [[check objectForKey: @"LSItemContentTypes"] containsObject: type])/
/^- \(Class\) documentClassForType: \(NSString \*\) type \{$/,/^\}$/s/^        if \(\[type isEqualToString: \[fileType objectForKey: @"CFBundleTypeName"\]\]\) \{$/        if ([type isEqualToString: [fileType objectForKey: @"CFBundleTypeName"]] || [[fileType objectForKey: @"LSItemContentTypes"] containsObject: type]) {/
/^- \(NSString \*\) typeForContentsOfURL: \(NSURL \*\) url error: \(NSError \*\*\) error \{$/,/^\}$/s/^            return \[fileType objectForKey: @"CFBundleTypeName"\];$/            return UTI;/
