# ClokeOS: Ebenen (Core Animation) zeichnet Cocotron über OpenGL, das es hier nicht gibt; ebenengestützte Ansichten blieben schwarz
# (Logic Pro). Ohne Ebenen zeichnet jede Ansicht wie gewohnt mit drawRect: ins Fenster. CLOKE_VIEW_LAYERS=1 schaltet sie wieder ein.
s|^    //NSViewLayersEnabled = \[defaults boolForKey: @"NSViewLayersEnabled"\];$|    NSViewLayersEnabled = getenv("CLOKE_VIEW_LAYERS") != NULL;|
# ClokeOS: trackingAreas liefert wie bei Apple eine Kopie. Programme entfernen beim Durchlaufen Bereiche (Logic Pros MAToolKit in
# updateTrackingAreas); mit dem inneren Array warf das "Illegal mutation while fast enumerating" und beendete Logic.
/^- \(NSArray \*\) trackingAreas \{$/,/^\}$/s/^    return _trackingAreas;$/    return [[_trackingAreas copy] autorelease];/
# ClokeOS: gemeinsamer Vorfahr zweier Ansichten (war nicht umgesetzt; Logic Pro rechnet damit Koordinaten zwischen Spurkopf und Spur um)
/^- \(NSView \*\) ancestorSharedWithView: \(NSView \*\) view \{$/,/^\}$/c\
- (NSView *) ancestorSharedWithView: (NSView *) view {\
    for (NSView *a = self; a != nil; a = [a superview])\
        if (view == a || [view isDescendantOf: a])\
            return a;\
    return nil;\
}
# … nebenläufiges Zeichnen gibt es nicht; Abfrage und Setzen ohne Meldung
/^- \(BOOL\) canDrawConcurrently \{$/,/^\}$/s/^    NSUnimplementedMethod\(\);$//
/^- \(void\) setCanDrawConcurrently: \(BOOL\) canDraw \{$/,/^\}$/s/^    NSUnimplementedMethod\(\);$//
# ClokeOS: Ebenen ohne OpenGL (macapps/appkit/softlayers.m). Bei ausgeschalteter Ebenen-Darstellung hat eine Ansicht mit wantsLayer
# trotzdem eine Ebene (wie bei Apple; vorher nil, und alles, was ein Programm an layer setzte, ging verloren), ebenso eine mit setLayer:
# gesetzte. Gezeichnet wird weiter ins Fenster (nicht in ein eigenes Bitmap wie Cocotrons OpenGL-Weg); die Ebene malt softlayers.m dazu.
s|^#import <QuartzCore/CATransaction.h>$|#import <QuartzCore/CATransaction.h>\n#import <objc/message.h>|
/^- \(CALayer \*\) layer \{$/,/^\}$/s/^    return _layer;$/    if (_layer == nil \&\& _wantsLayer \&\& !NSViewLayersEnabled) {\n        _layer = [[self makeBackingLayer] retain];\n        [_layer setDelegate: self];\n        configureLayerGeometry(self);\n    }\n    return _layer;/
/^- \(void\) setLayer: \(CALayer \*\) value \{$/,/^\}$/s/^        return;$/    {\n        if (value != _layer) {\n            [_layer release];\n            _layer = [value retain];\n            [self setNeedsDisplay: YES];\n        }\n        return;\n    }/
s/^        \} else if \(_layer != nil\) \{$/        } else if (_layer != nil \&\& NSViewLayersEnabled) {/
s/^    if \(viewBeingPrinted == nil \&\& _layer != nil\) \{$/    if (viewBeingPrinted == nil \&\& _layer != nil \&\& NSViewLayersEnabled) {/
s/^            \[self drawRect: rect\];$/            if (!NSViewLayersEnabled \&\& (_layer != nil || _wantsLayer) \&\& [self respondsToSelector: @selector(_clkDrawLayerBackedRect:)])\n                ((void (*)(id, SEL, NSRect)) objc_msgSend)(self, @selector(_clkDrawLayerBackedRect:), rect);\n            else\n                [self drawRect: rect];/
