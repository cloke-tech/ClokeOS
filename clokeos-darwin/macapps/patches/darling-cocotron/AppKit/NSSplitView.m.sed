# ClokeOS: isOpaque ohne Meldung; NO wie bei Apple (die Teilansichten decken die Fläche, nicht die Split-Ansicht selbst)
/^- \(BOOL\) isOpaque \{$/,/^\}$/{
s/^    NSUnimplementedMethod\(\);$//
s/^    return YES;$/    return NO;/
}
# ClokeOS: setPosition:ofDividerAtIndex: begrenzte die *Breiten* der beiden Teilansichten auf die minimale/maximale *Teilerposition*;
# eine Teilansicht bei x = 368 konnte so nie schmaler als 368 Punkte werden (Logic Pros Live-Loops-Spalte, die Logic auf 0 setzt, blieb
# rund 400 Punkte breit und schwarz). Jetzt wird die Position begrenzt: die erste Ansicht endet dort, die zweite beginnt hinter dem Teiler.
/^        \/\/ Figure out the adjusted widths$/,/^                \(NSMinX\(frame1\) \+ NSWidth\(frame1\)\) - NSWidth\(resize1\);$/c\
        {\
            CGFloat p = constrainTo(floor(position), minPosition, maxPosition);\
            resize0.size.width = MAX(0, p - NSMinX(frame0));\
            CGFloat start = MIN(NSMaxX(frame1), p + [self dividerThickness]);\
            resize1.size.width = MAX(0, NSMaxX(frame1) - start);\
            resize1.origin.x = NSMaxX(frame1) - NSWidth(resize1);\
        }
/^        \/\/ Figure out the adjusted heights$/,/^                \(NSMinY\(frame1\) \+ NSHeight\(frame1\)\) - NSHeight\(resize1\);$/c\
        {\
            CGFloat p = constrainTo(floor(position), minPosition, maxPosition);\
            resize0.size.height = MAX(0, p - NSMinY(frame0));\
            CGFloat start = MIN(NSMaxY(frame1), p + [self dividerThickness]);\
            resize1.size.height = MAX(0, NSMaxY(frame1) - start);\
            resize1.origin.y = NSMaxY(frame1) - NSHeight(resize1);\
        }
# ClokeOS: arrangedSubviews/addArrangedSubview:/insertArrangedSubview:atIndex:/removeArrangedSubview: (10.11) fehlten. Eine nicht mehr
# angeordnete Unteransicht bleibt wie bei Apple Unteransicht, bekommt aber keinen Platz mehr (zählt wie eingeklappt). Logic Pro nimmt so
# die Live-Loops-Spalte aus dem Arrangierbereich; ohne das blieb sie als schwarze Fläche stehen.
s|^#import <AppKit/NSSplitView.h>$|#import <AppKit/NSSplitView.h>\n#import <objc/runtime.h>\nstatic char clk_unarranged;|
/^- \(BOOL\) isSubviewCollapsed: \(NSView \*\) subview \{$/,/^\}$/s/^    return \[subview isHidden\];$/    return [subview isHidden] || [objc_getAssociatedObject(self, \&clk_unarranged) containsObject: subview];/
/^- \(BOOL\) isOpaque \{$/i\
- (NSMutableSet *) clk_unarrangedSet {\
    NSMutableSet *s = objc_getAssociatedObject(self, &clk_unarranged);\
    if (s == nil) {\
        s = [NSMutableSet set];\
        objc_setAssociatedObject(self, &clk_unarranged, s, OBJC_ASSOCIATION_RETAIN);\
    }\
    return s;\
}\
- (NSArray *) arrangedSubviews {\
    NSMutableArray *a = [NSMutableArray array];\
    NSSet *out = objc_getAssociatedObject(self, &clk_unarranged);\
    for (NSView *v in [self subviews])\
        if (![out containsObject: v])\
            [a addObject: v];\
    return a;\
}\
- (BOOL) arrangesAllSubviews {\
    return [objc_getAssociatedObject(self, &clk_unarranged) count] == 0;\
}\
- (void) setArrangesAllSubviews: (BOOL) flag {\
}\
- (void) addArrangedSubview: (NSView *) view {\
    [[self clk_unarrangedSet] removeObject: view];\
    if ([view superview] != self)\
        [self addSubview: view];\
    else\
        [self adjustSubviews];\
}\
- (void) insertArrangedSubview: (NSView *) view atIndex: (NSInteger) index {\
    [[self clk_unarrangedSet] removeObject: view];\
    NSArray *arranged = [self arrangedSubviews];\
    if ([view superview] != self) {\
        if (index < (NSInteger) [arranged count])\
            [self addSubview: view positioned: NSWindowBelow relativeTo: [arranged objectAtIndex: index]];\
        else\
            [self addSubview: view];\
    }\
    [self adjustSubviews];\
}\
- (void) removeArrangedSubview: (NSView *) view {\
    if (view == nil || [view superview] != self)\
        return;\
    [[self clk_unarrangedSet] addObject: view];\
    [self adjustSubviews];\
}\

