# ClokeOS: Bildlage wie bei Apple (auf dem Mac nachgeprüft): setImage: mit Bild bei NSNoImage macht daraus NSImageOnly, setTitle: mit Text
# bei einer reinen Bildzelle ohne bisherigen Titel NSImageOverlaps. Cocotron ließ NSNoImage stehen; Knöpfe, die ein Programm im Code mit
# Bild baut (Logic Pros Transport- und Ansichtsknöpfe), zeigten so nur ihren Titel ("Play", "Stop") statt des Bildes.
/^- \(void\) setTitle: \(NSString \*\) title \{$/,/^\}$/c\
- (void) setImage: (NSImage *) image {\
    [super setImage: image];\
    if (image != nil && _imagePosition == NSNoImage)\
        _imagePosition = NSImageOnly;\
}\
\
- (void) setTitle: (NSString *) title {\
    if ([title length] > 0 && _imagePosition == NSImageOnly && [_titleOrAttributedTitle length] == 0 && [self image] != nil)\
        _imagePosition = NSImageOverlaps;\
    title = [title copy];\
    [_titleOrAttributedTitle release];\
    _titleOrAttributedTitle = title;\
}
# ClokeOS: CLOKE_BUTTON_TRACE=1 meldet für die ersten 60 Knöpfe mit Bild: Titel, Bild, Bildrechteck, Rahmen der Ansicht (Fehlersuche)
s/^        \[self drawImage: image withFrame: rect inView: controlView\];$/        {\n            static int clk_trace = -1, clk_n;\n            if (clk_trace < 0) clk_trace = getenv("CLOKE_BUTTON_TRACE") != NULL;\n            if (clk_trace \&\& clk_n++ < 60)\n                NSLog(@"[clokeos-button] %@ \\"%@\\" image=%@ %@ rect=%@ view=%@ flipped=%d", [self class], [self title], image ? NSStringFromSize([image size]) : @"nil", image, NSStringFromRect(rect), NSStringFromRect([controlView frame]), (int) [controlView isFlipped]);\n        }\n        [self drawImage: image withFrame: rect inView: controlView];/
