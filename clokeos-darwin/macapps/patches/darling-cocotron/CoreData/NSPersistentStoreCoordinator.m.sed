# ClokeOS: NSBinaryStoreType und NSSQLiteStoreType gibt es in Cocotron nicht; beide laufen über den Speicher im Arbeitsspeicher
# (nichts wird geschrieben; Logic Pros Audio-Unit-Zwischenspeicher entsteht so bei jedem Start neu).
s/^(                        forKey: NSXMLStoreType\];)$/\1\n        [_storeTypes setObject: [NSInMemoryPersistentStore class] forKey: NSBinaryStoreType];\n        [_storeTypes setObject: [NSInMemoryPersistentStore class] forKey: NSSQLiteStoreType];/
