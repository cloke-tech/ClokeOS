/* Probe für ansichtsbasierte Tabellen: läuft unverändert auf dem Mac (Vergleichswerte) und in ClokeOS (docs/logic-launch.md, Runde 62).
 *
 *   tableprobe [tableprobe.nib]
 *
 * Fall 1 baut eine Tabelle in Code (Delegat tableView:viewForTableColumn:row: mit makeViewWithIdentifier:owner:, sonst neue Ansichten:
 * Ankreuzfeld und NSTableCellView mit Beschriftung über Bedingungen), gibt die sichtbaren Zeilen- und Zellansichten aus, scrollt,
 * fügt Zeilen ein, löscht welche, wählt, lädt neu und fragt viewAtColumn:row:makeIfNecessary:.
 * Fall 2 lädt tableprobe.nib (erzeugt von tableprobe-nib.py: Vorlagen im NSTableViewArchivedReusableViewsKey), einmal mit Delegat
 * (Text und Haken je Zeile, Aktion toggle: an den Besitzer), einmal nur mit Datenquelle (objectValue der Zellansichten).
 *
 * Bauen: clang -arch arm64 -mmacosx-version-min=11.0 -fno-stack-check -fobjc-arc -framework AppKit tableprobe.m -o tableprobe
 *        python3 tableprobe-nib.py tableprobe.nib */
#import <AppKit/AppKit.h>
#import <objc/runtime.h>

static NSString *R(NSRect r) { return [NSString stringWithFormat: @"{%g,%g %gx%g}", r.origin.x, r.origin.y, r.size.width, r.size.height]; }

/* ---- Besitzer / Delegat / Datenquelle ---- */
/* Haken aus dem Namen (letzte Ziffer ungerade), nicht aus der Zeilennummer: AppKit hält auch Zeilen außerhalb des Bildes vor und
 * verschiebt sie beim Einfügen/Löschen, ohne neu zu fragen */
static BOOL odd(NSString *s) { return [s length] && ([s characterAtIndex: [s length] - 1] & 1); }

@interface TableProbeOwner : NSObject <NSTableViewDataSource, NSTableViewDelegate>
@property (strong) NSTableView *table;
@property (strong) NSMutableArray *names;
@property NSInteger made, toggles, toggledRow;
@end
@implementation TableProbeOwner
- (instancetype) init {
    if ((self = [super init])) {
        _names = [NSMutableArray array];
        for (int i = 0; i < 30; i++) [_names addObject: [NSString stringWithFormat: @"Row %d", i]];
    }
    return self;
}
- (NSInteger) numberOfRowsInTableView: (NSTableView *) t { return [_names count]; }
- (void) toggle: (id) sender {
    _toggles++;
    _toggledRow = [_table rowForView: sender];
}
@end

/* Fall 1: Ansichten in Code, wiederverwendet über makeViewWithIdentifier: */
@interface CodeOwner : TableProbeOwner
@end
@implementation CodeOwner
- (NSView *) tableView: (NSTableView *) t viewForTableColumn: (NSTableColumn *) c row: (NSInteger) row {
    if ([[c identifier] isEqual: @"check"]) {
        NSButton *b = [t makeViewWithIdentifier: @"check" owner: self];
        if (!b) {
            b = [[NSButton alloc] initWithFrame: NSMakeRect(0, 0, 20, 18)];
            [b setButtonType: NSButtonTypeSwitch];
            [b setTitle: @""];
            [b setIdentifier: @"check"];
            [b setTarget: self];
            [b setAction: @selector(toggle:)];
            self.made++;
        }
        [b setState: odd(self.names[row]) ? NSControlStateValueOn : NSControlStateValueOff];
        return b;
    }
    NSTableCellView *v = [t makeViewWithIdentifier: @"name" owner: self];
    if (!v) {
        v = [[NSTableCellView alloc] initWithFrame: NSMakeRect(0, 0, 150, 20)];
        [v setIdentifier: @"name"];
        NSTextField *f = [[NSTextField alloc] initWithFrame: NSMakeRect(2, 2, 146, 16)];
        [f setBezeled: NO]; [f setDrawsBackground: NO]; [f setFont: [NSFont systemFontOfSize: 13]]; [f setEditable: NO]; [f setSelectable: NO];
        [f setTranslatesAutoresizingMaskIntoConstraints: NO];
        [v addSubview: f];
        [v setTextField: f];
        [v addConstraint: [NSLayoutConstraint constraintWithItem: f attribute: NSLayoutAttributeLeading relatedBy: NSLayoutRelationEqual
                                                          toItem: v attribute: NSLayoutAttributeLeading multiplier: 1 constant: 2]];
        [v addConstraint: [NSLayoutConstraint constraintWithItem: v attribute: NSLayoutAttributeTrailing relatedBy: NSLayoutRelationEqual
                                                          toItem: f attribute: NSLayoutAttributeTrailing multiplier: 1 constant: 2]];
        [v addConstraint: [NSLayoutConstraint constraintWithItem: f attribute: NSLayoutAttributeCenterY relatedBy: NSLayoutRelationEqual
                                                          toItem: v attribute: NSLayoutAttributeCenterY multiplier: 1 constant: 0]];
        self.made++;
    }
    [[v textField] setStringValue: self.names[row]];
    return v;
}
@end

/* Fall 2a: Vorlagen aus der Nib, Delegat setzt Text und Haken (wie Logic Pros Bounce-Dialog) */
@interface NibOwner : TableProbeOwner
@end
@implementation NibOwner
- (NSView *) tableView: (NSTableView *) t viewForTableColumn: (NSTableColumn *) c row: (NSInteger) row {
    NSView *v = [t makeViewWithIdentifier: [c identifier] owner: self];
    if ([v isKindOfClass: [NSTableCellView class]]) [[(NSTableCellView *) v textField] setStringValue: self.names[row]];
    else if ([v isKindOfClass: [NSButton class]]) [(NSButton *) v setState: odd(self.names[row]) ? NSControlStateValueOn : NSControlStateValueOff];
    return v;
}
@end

/* Fall 2b: kein viewFor…, nur die Datenquelle (objectValue an die Zellansicht) */
@interface ValueOwner : TableProbeOwner
@end
@implementation ValueOwner
- (id) tableView: (NSTableView *) t objectValueForTableColumn: (NSTableColumn *) c row: (NSInteger) row {
    return [NSString stringWithFormat: @"%@:%ld", [c identifier], (long) row];
}
@end

/* ---- Ausgabe ---- */
static void settle(NSWindow *w) {
    for (int i = 0; i < 3; i++) {
        [w layoutIfNeeded];
        [w displayIfNeeded];
        [[NSRunLoop currentRunLoop] runMode: NSDefaultRunLoopMode beforeDate: [NSDate dateWithTimeIntervalSinceNow: 0.05]];
    }
}

static NSRange shown(NSTableView *t) {
    NSRect vr = [t visibleRect];
    NSInteger a = [t rowAtPoint: NSMakePoint(NSMidX(vr), NSMinY(vr) + 1)];
    NSInteger b = [t rowAtPoint: NSMakePoint(NSMidX(vr), NSMaxY(vr) - 1)];
    if (b < 0) b = [t numberOfRows] - 1;
    return a < 0 ? NSMakeRange(0, 0) : NSMakeRange(a, b - a + 1);
}

static void dump(NSTableView *t, TableProbeOwner *o, const char *what) {
    NSRange r = shown(t);
    printf("-- %s: rows=%ld visible=%lu..%lu selectedRow=%ld\n", what, (long) [t numberOfRows], (unsigned long) r.location,
           (unsigned long) (NSMaxRange(r) - 1), (long) [t selectedRow]);
    for (NSUInteger row = r.location; row < NSMaxRange(r); row++) {
        NSTableRowView *rv = [t rowViewAtRow: row makeIfNecessary: NO];
        printf("  row %lu: %s %s super=%s sel=%d\n", (unsigned long) row, rv ? class_getName([rv class]) : "nil",
               rv ? R([rv frame]).UTF8String : "", [rv superview] == t ? "table" : "?", (int) [rv isSelected]);
        for (NSInteger c = 0; c < [t numberOfColumns]; c++) {
            NSView *v = [t viewAtColumn: c row: row makeIfNecessary: NO];
            if (!v) { printf("    col %ld: nil\n", (long) c); continue; }
            printf("    col %ld: %s %s in-row=%d id=%s", (long) c, class_getName([v class]), R([v frame]).UTF8String,
                   (int) ([v superview] == rv), [[v identifier] UTF8String] ?: "-");
            if ([v isKindOfClass: [NSTableCellView class]]) {
                NSTableCellView *cv = (NSTableCellView *) v;
                NSTextField *f = [cv textField];
                printf(" text=%s \"%s\" obj=%s bg=%ld rowForView=%ld colForView=%ld", f ? R([f frame]).UTF8String : "nil",
                       [[f stringValue] UTF8String] ?: "", [[[cv objectValue] description] UTF8String] ?: "nil", (long) [cv backgroundStyle],
                       (long) [t rowForView: f], (long) [t columnForView: f]);
            } else if ([v isKindOfClass: [NSButton class]]) {
                NSButton *b = (NSButton *) v;
                printf(" state=%ld target=%s action=%s", (long) [b state], [b target] == o ? "owner" : "other",
                       [b action] ? sel_getName([b action]) : "-");
            }
            printf("\n");
        }
    }
}

static NSScrollView *embed(NSTableView *t, NSWindow **win) {
    NSWindow *w = [[NSWindow alloc] initWithContentRect: NSMakeRect(100, 100, 300, 200) styleMask: NSWindowStyleMaskTitled
                                                backing: NSBackingStoreBuffered defer: NO];
    [w setReleasedWhenClosed: NO];
    NSScrollView *sv = [[NSScrollView alloc] initWithFrame: NSMakeRect(10, 10, 240, 112)];
    [sv setHasVerticalScroller: NO];
    [sv setBorderType: NSNoBorder];
    [t setHeaderView: nil];
    if ([t respondsToSelector: @selector(setColumnAutoresizingStyle:)]) [t setColumnAutoresizingStyle: NSTableViewNoColumnAutoresizing];
    if ([t respondsToSelector: @selector(setStyle:)]) [t setStyle: NSTableViewStylePlain];
    [sv setDocumentView: t];
    [[w contentView] addSubview: sv];
    [w orderFront: nil];
    *win = w;
    return sv;
}

static void code_case(void) {
    printf("== Fall 1: Ansichten in Code\n");
    CodeOwner *o = [CodeOwner new];
    NSTableView *t = [[NSTableView alloc] initWithFrame: NSMakeRect(0, 0, 200, 100)];
    o.table = t;
    for (NSString *ident in @[ @"check", @"name" ]) {
        NSTableColumn *c = [[NSTableColumn alloc] initWithIdentifier: ident];
        [c setWidth: [ident isEqual: @"check"] ? 20 : 204];   /* 20 + 8 + 204 + 8 = Breite der Clip-Ansicht */
        [t addTableColumn: c];
    }
    [t setIntercellSpacing: NSMakeSize(8, 2)];
    [t setRowHeight: 20];
    NSWindow *w;
    NSScrollView *sv = embed(t, &w);
    [t setDelegate: o];
    [t setDataSource: o];
    [t reloadData];
    settle(w);
    dump(t, o, "anfangs");
    printf("neu angelegt: %ld\n", (long) o.made);

    [[sv contentView] scrollToPoint: NSMakePoint(0, 220)];
    [sv reflectScrolledClipView: [sv contentView]];
    settle(w);
    dump(t, o, "gescrollt auf y=220");
    printf("neu angelegt nach Scrollen <= 16: %s\n", o.made <= 16 ? "ja" : "nein");
    printf("Zeile 2 ohne Anlegen: %s\n", [t viewAtColumn: 1 row: 2 makeIfNecessary: NO] ? "da" : "nil");
    NSTableCellView *mk = [t viewAtColumn: 1 row: 2 makeIfNecessary: YES];
    printf("Zeile 2 angelegt: %s \"%s\"\n", class_getName([mk class]), [[[mk textField] stringValue] UTF8String] ?: "");

    [[sv contentView] scrollToPoint: NSMakePoint(0, 0)];
    [sv reflectScrolledClipView: [sv contentView]];
    settle(w);
    [o.names insertObject: @"Neu A" atIndex: 1];
    [t insertRowsAtIndexes: [NSIndexSet indexSetWithIndex: 1] withAnimation: 0];
    settle(w);
    dump(t, o, "Zeile 1 eingefügt");

    [o.names removeObjectAtIndex: 2];
    [o.names removeObjectAtIndex: 0];
    NSMutableIndexSet *del = [NSMutableIndexSet indexSetWithIndex: 0];
    [del addIndex: 2];
    [t removeRowsAtIndexes: del withAnimation: 0];
    settle(w);
    dump(t, o, "Zeilen 0 und 2 gelöscht");

    [t selectRowIndexes: [NSIndexSet indexSetWithIndex: 1] byExtendingSelection: NO];
    settle(w);
    dump(t, o, "Zeile 1 gewählt");

    o.names[3] = @"Geändert";
    [t reloadDataForRowIndexes: [NSIndexSet indexSetWithIndex: 3] columnIndexes: [NSIndexSet indexSetWithIndex: 1]];
    settle(w);
    printf("Zeile 3 nach reloadDataForRowIndexes: \"%s\"\n",
           [[[[t viewAtColumn: 1 row: 3 makeIfNecessary: NO] textField] stringValue] UTF8String] ?: "nil");

    [o.names removeAllObjects];
    for (int i = 0; i < 3; i++) [o.names addObject: [NSString stringWithFormat: @"Kurz %d", i]];
    [t reloadData];
    settle(w);
    dump(t, o, "reloadData mit 3 Zeilen");
    __block int avail = 0;
    [t enumerateAvailableRowViewsUsingBlock: ^(NSTableRowView *rv, NSInteger row) { avail++; }];
    printf("verfügbare Zeilenansichten: %d\n", avail);
    [w orderOut: nil];
}

static void nib_case(NSString *path) {
    NSData *d = [NSData dataWithContentsOfFile: path];
    if (!d) { printf("== Fall 2: %s fehlt\n", [path UTF8String]); return; }
    for (int k = 0; k < 2; k++) {
        printf("== Fall 2%c: Nib, %s\n", "ab"[k], k ? "nur Datenquelle (objectValue)" : "Delegat mit makeViewWithIdentifier:owner:");
        TableProbeOwner *o = k ? [ValueOwner new] : [NibOwner new];
        NSNib *nib = [[NSNib alloc] initWithNibData: d bundle: nil];
        NSArray *top = nil;
        BOOL ok = [nib respondsToSelector: @selector(instantiateWithOwner:topLevelObjects:)]
                      ? [nib instantiateWithOwner: o topLevelObjects: &top]
                      : [nib instantiateNibWithOwner: o topLevelObjects: &top];
        NSTableView *t = o.table;
        printf("geladen=%d table=%s Spalten=%ld Vorlagen=%s\n", (int) ok, t ? class_getName([t class]) : "nil", (long) [t numberOfColumns],
               [[[[[t registeredNibsByIdentifier] allKeys] sortedArrayUsingSelector: @selector(compare:)] componentsJoinedByString: @","]
                   UTF8String] ?: "-");
        if (!t) continue;
        NSWindow *w;
        embed(t, &w);
        [t setDelegate: o];
        [t setDataSource: o];
        [t reloadData];
        settle(w);
        dump(t, o, "Nib");
        if (!k) {
            NSButton *b = [t viewAtColumn: 0 row: 1 makeIfNecessary: NO];
            if ([b isKindOfClass: [NSButton class]]) [b performClick: nil];
            printf("toggle: %ld mal, Zeile %ld, Zustand jetzt %ld\n", (long) o.toggles, (long) o.toggledRow, (long) [b state]);
            NSView *again = [t makeViewWithIdentifier: @"name" owner: o];
            printf("makeViewWithIdentifier name: %s id=%s; unbekannt: %s\n", class_getName([again class]), [[again identifier] UTF8String] ?: "-",
                   [t makeViewWithIdentifier: @"gibtsnicht" owner: o] ? "da" : "nil");
        }
        [w orderOut: nil];
    }
}

int main(int argc, char **argv) {
    @autoreleasepool {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy: NSApplicationActivationPolicyAccessory];
        code_case();
        NSString *nib = argc > 1 ? @(argv[1])
                                 : [[@(argv[0]) stringByDeletingLastPathComponent] stringByAppendingPathComponent: @"tableprobe.nib"];
        nib_case(nib);
    }
    return 0;
}
