# ClokeOS: Sichern legt den Pfad aus Ordner + Name (nameFieldStringValue) + Endung an. Bisher nahm _selectFile: nur die in der Liste gewählte Zeile;
# ohne Auswahl (das Panel hat kein Namensfeld) war der Pfad leer und Logic Pros Bounce schrieb nichts.
# Ohne Bildschirm: CLOKE_SAVEPANEL_DIR=<Ordner> beantwortet runModal sofort mit OK und diesem Ordner.
/^- \(IBAction\) _selectFile: \(id\) sender \{$/i\
- (NSString *) _clkTargetPathInDirectory: (NSString *) dir {\
    NSString *name = [_nameFieldStringValue length] ? _nameFieldStringValue : @"Untitled";\
    NSString *p = [(dir ? dir : NSHomeDirectory()) stringByAppendingPathComponent: name];\
    NSString *ext = [_requiredFileType length] ? _requiredFileType : ([_allowedFileTypes count] ? [_allowedFileTypes objectAtIndex: 0] : nil);\
    if ([ext length] && ![[p pathExtension] isEqualToString: ext])\
        p = [p stringByAppendingPathExtension: ext];\
    return p;\
}\

/^    NSURL \*url = \[_outlineView itemAtRow: \[_outlineView selectedRow\]\];$/,/^    \[self _setFilename: \[url path\]\];$/c\
    NSInteger row = (NSInteger)[_outlineView selectedRow];\
    NSURL *url = row >= 0 ? [_outlineView itemAtRow: row] : nil;\
    NSString *p = [url path];\
    BOOL isDir = NO;\
    if (p == nil || ([[NSFileManager defaultManager] fileExistsAtPath: p isDirectory: &isDir] && isDir))\
        p = [self _clkTargetPathInDirectory: p ? p : [self directory]];\
    [self _setFilename: p];
/^- \(NSInteger\) runModal \{$/a\
    const char *autoDir = getenv("CLOKE_SAVEPANEL_DIR");\
    if (autoDir != NULL) {\
        [self _setFilename: [self _clkTargetPathInDirectory: [NSString stringWithUTF8String: autoDir]]];\
        NSLog(@"NSSavePanel: CLOKE_SAVEPANEL_DIR -> %@", [self filename]);\
        return NSOKButton;\
    }
