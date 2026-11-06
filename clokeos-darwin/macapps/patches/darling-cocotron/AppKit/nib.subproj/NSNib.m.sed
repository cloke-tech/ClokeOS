# ClokeOS: Nibs im Binärformat NIBArchive (neuere Xcode, z. B. alle Nibs von Logic Pro) vor dem Entpacken in ein NSKeyedArchiver-Archiv
# umsetzen (macapps/appkit/nibarchive.m); andere Nibs bleiben unverändert.
s/^(@implementation NSNib)$/NSData *ClkNibArchiveToKeyed(NSData *data);\n\n\1/
s/initForReadingWithData: _data\] autorelease\];/initForReadingWithData: (_flags._isKeyed ? ClkNibArchiveToKeyed(_data) : _data)] autorelease];/
