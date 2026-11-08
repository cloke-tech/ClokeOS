# ClokeOS: Nibs im Binärformat NIBArchive (neuere Xcode, z. B. alle Nibs von Logic Pro) vor dem Entpacken in ein NSKeyedArchiver-Archiv
# umsetzen (macapps/appkit/nibarchive.m); andere Nibs bleiben unverändert.
s/^(@implementation NSNib)$/NSData *ClkNibArchiveToKeyed(NSData *data);\n\n\1/
s/initForReadingWithData: _data\] autorelease\];/initForReadingWithData: (_flags._isKeyed ? ClkNibArchiveToKeyed(_data) : _data)] autorelease];/
# -initWithNibData:bundle: (macOS 10.8) fehlte: Nib aus Daten im Speicher (immer schlüsselbasiert)
s/^- initWithNibNamed: \(NSString \*\) name bundle: \(NSBundle \*\) bundle \{$/- initWithNibData: (NSData *) data bundle: (NSBundle *) bundle {\n    if (!data) { [self release]; return nil; }\n    _data = [data copy];\n    _flags._isKeyed = TRUE;\n    _allObjects = [NSMutableArray new];\n    return self;\n}\n\n&/
