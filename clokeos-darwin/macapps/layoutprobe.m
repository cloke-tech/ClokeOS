/* Auto-Layout-Probe: lädt eine Nib (oder baut Testfälle in Code), legt sie aus und gibt die Rahmen aller Ansichten aus.
 * Läuft unverändert auf dem Mac (Vergleichswerte) und in ClokeOS (docs/logic-launch.md, Runde 61).
 *
 *   layoutprobe                 eingebaute Fälle: intrinsische Größen, Bedingungen, fittingSize, NSGridView, Fenstergröße
 *   layoutprobe <x.nib> [Besitzerklasse]   Nib laden, jedes Fenster auslegen, Baum mit Rahmen ausgeben
 *
 * Bauen: clang -arch arm64 -mmacosx-version-min=11.0 -fno-stack-check -fobjc-arc -framework AppKit layoutprobe.m -o layoutprobe
 * Fehlende Klassen der Nib (eigene Klassen des Programms) legt die Probe zur Laufzeit als Unterklassen von NSControl/NSObject an. */
#import <AppKit/AppKit.h>
#import <objc/runtime.h>

static NSString *R(NSRect r) { return [NSString stringWithFormat: @"{%g,%g %gx%g}", r.origin.x, r.origin.y, r.size.width, r.size.height]; }
static NSString *S(NSSize s) { return [NSString stringWithFormat: @"%gx%g", s.width, s.height]; }

static NSString *title(NSView *v) {
    if ([v respondsToSelector: @selector(title)] && ![v isKindOfClass: [NSBox class]]) {
        id t = [(id) v title];
        if ([t isKindOfClass: [NSString class]] && [t length]) return t;
    }
    if ([v isKindOfClass: [NSTextField class]]) return [(NSTextField *) v stringValue];
    return nil;
}

static void dump(NSView *v, int depth) {
    if (depth > 12) return;
    NSString *t = title(v);
    NSSize ics = [v intrinsicContentSize];
    printf("%*s%s %s%s%s ics=%s%s\n", depth * 2, "", class_getName([v class]), [R([v frame]) UTF8String],
           t ? " \"" : "", t ? [[t substringToIndex: MIN([t length], (NSUInteger) 40)] UTF8String] : "", S(ics).UTF8String,
           [v isHidden] ? " hidden" : "");
    for (NSView *s in [v subviews]) dump(s, depth + 1);
}

/* eigene Klassen der Nib zur Laufzeit anlegen */
@interface ProbeOwner : NSObject
@end
@implementation ProbeOwner
- (void) setValue: (id) v forUndefinedKey: (NSString *) k {}
- (id) valueForUndefinedKey: (NSString *) k { return nil; }
@end

static void make_class(NSString *s) {
    unichar c0 = [s characterAtIndex: 0];
    BOOL ident = c0 >= 'A' && c0 <= 'Z';
    for (NSUInteger k = 0; k < [s length] && ident; k++) {
        unichar c = [s characterAtIndex: k];
        if (!(isalnum(c) || c == '_')) ident = NO;
    }
    if (!ident || NSClassFromString(s)) return;
    Class sup = [s hasSuffix: @"Controller"] || [s hasSuffix: @"Delegate"] ? [ProbeOwner class]
              : [s hasSuffix: @"Panel"] ? [NSPanel class] : [s hasSuffix: @"Window"] ? [NSWindow class] : [NSControl class];
    Class k = objc_allocateClassPair(sup, [s UTF8String], 0);
    if (k) objc_registerClassPair(k);
}

static void make_classes(NSData *nib) {
    /* Eigene Klassennamen stehen als NS.bytes (Länge davor, danach folgt gleich der nächste Wert) in der Nib: jede druckbare Folge,
     * die wie ein Klassenname aussieht, wird mit und ohne ihr letztes Zeichen als Klasse angelegt, wenn es sie noch nicht gibt. */
    const char *b = [nib bytes];
    NSUInteger n = [nib length], i = 0;
    while (i < n) {
        NSUInteger j = i;
        while (j < n && b[j] >= 0x21 && b[j] < 0x7f) j++;
        if (j - i >= 6) {
            NSString *s = [[NSString alloc] initWithBytes: b + i length: j - i encoding: NSASCIIStringEncoding];
            make_class(s);
            make_class([s substringToIndex: [s length] - 1]);
        }
        i = j + 1;
    }
}

static void nibcase(const char *path, const char *owner) {
    NSData *d = [NSData dataWithContentsOfFile: [NSString stringWithUTF8String: path]];
    if (!d) { printf("keine Nib: %s\n", path); return; }
    make_classes(d);
    NSNib *nib = [[NSNib alloc] initWithNibData: d bundle: nil];
    id own = owner ? [NSClassFromString([NSString stringWithUTF8String: owner]) new] : [ProbeOwner new];
    NSArray *top = nil;
    /* Cocotron hat nur den älteren Namen instantiateNibWithOwner:topLevelObjects: */
    BOOL ok = [nib respondsToSelector: @selector(instantiateWithOwner:topLevelObjects:)]
                  ? [nib instantiateWithOwner: own topLevelObjects: &top]
                  : [nib instantiateNibWithOwner: own topLevelObjects: &top];
    if (!ok) { printf("Laden fehlgeschlagen\n"); return; }
    for (id o in top) {
        if (![o isKindOfClass: [NSWindow class]]) continue;
        NSWindow *w = o;
        NSView *cv = [w contentView];
        printf("Fenster vorher frame=%s content=%s\n", R([w frame]).UTF8String, R([cv frame]).UTF8String);
        printf("fittingSize %s\n", S([cv fittingSize]).UTF8String);
        [w layoutIfNeeded];
        printf("Fenster nach layoutIfNeeded frame=%s content=%s\n", R([w frame]).UTF8String, R([cv frame]).UTF8String);
        [w orderFront: nil];
        [w displayIfNeeded];
        printf("Fenster sichtbar frame=%s content=%s\n", R([w frame]).UTF8String, R([cv frame]).UTF8String);
        dump(cv, 0);
        [w orderOut: nil];
    }
}

/* ---- eingebaute Fälle ---- */
static NSButton *check(NSString *t) { return [NSButton checkboxWithTitle: t target: nil action: nil]; }

static void ics_case(void) {
    printf("== intrinsische Größen\n");
    NSArray *vs = @[ [NSTextField labelWithString: @"Bit Depth:"], [NSTextField labelWithString: @"Fade in & out samples:"],
                     [NSTextField textFieldWithString: @"44"], check(@"Include Audio Tail"), check(@"Include Tempo Information"),
                     [NSButton buttonWithTitle: @"OK" target: nil action: nil], [NSButton buttonWithTitle: @"Cancel" target: nil action: nil],
                     [NSButton radioButtonWithTitle: @"Radio" target: nil action: nil] ];
    for (NSView *v in vs) printf("%s \"%s\" ics=%s fitting=%s baseline=%g\n", class_getName([v class]), title(v).UTF8String,
                                 S([v intrinsicContentSize]).UTF8String, S([v fittingSize]).UTF8String, [v firstBaselineOffsetFromTop]);
    NSPopUpButton *p = [[NSPopUpButton alloc] initWithFrame: NSMakeRect(0, 0, 100, 25) pullsDown: NO];
    [p addItemsWithTitles: @[ @"CAF", @"POW-r #2 (Noise Shaping)" ]];
    printf("NSPopUpButton ics=%s baseline=%g\n", S([p intrinsicContentSize]).UTF8String, [p firstBaselineOffsetFromTop]);
    NSTextField *wrap = [NSTextField wrappingLabelWithString: @"Requires a lot of words that should wrap at some point in the line"];
    printf("wrapping ics=%s pmlw=%g\n", S([wrap intrinsicContentSize]).UTF8String, [wrap preferredMaxLayoutWidth]);
    NSView *plain = [NSView new];
    printf("NSView ics=%s noMetric=%g\n", S([plain intrinsicContentSize]).UTF8String, NSViewNoIntrinsicMetric);
    printf("priorities hug=%g/%g acp=%g/%g (label hug %g)\n", [plain contentHuggingPriorityForOrientation: 0],
           [plain contentHuggingPriorityForOrientation: 1], [plain contentCompressionResistancePriorityForOrientation: 0],
           [plain contentCompressionResistancePriorityForOrientation: 1],
           [[NSTextField labelWithString: @"x"] contentHuggingPriorityForOrientation: 0]);
}

static void constraint_case(void) {
    printf("== Bedingungen\n");
    NSView *root = [[NSView alloc] initWithFrame: NSMakeRect(0, 0, 400, 300)];
    NSButton *ok = [NSButton buttonWithTitle: @"OK" target: nil action: nil], *cancel = [NSButton buttonWithTitle: @"Cancel" target: nil action: nil];
    NSTextField *l = [NSTextField labelWithString: @"Name:"], *f = [NSTextField textFieldWithString: @""];
    for (NSView *v in @[ ok, cancel, l, f ]) { [v setTranslatesAutoresizingMaskIntoConstraints: NO]; [root addSubview: v]; }
    [NSLayoutConstraint activateConstraints: @[
        [[ok trailingAnchor] constraintEqualToAnchor: [root trailingAnchor] constant: -20],
        [[ok bottomAnchor] constraintEqualToAnchor: [root bottomAnchor] constant: -20],
        [[cancel trailingAnchor] constraintEqualToAnchor: [ok leadingAnchor] constant: -12],
        [[cancel firstBaselineAnchor] constraintEqualToAnchor: [ok firstBaselineAnchor]],
        [[ok widthAnchor] constraintEqualToAnchor: [cancel widthAnchor]],
        [[l leadingAnchor] constraintEqualToAnchor: [root leadingAnchor] constant: 20],
        [[l topAnchor] constraintEqualToAnchor: [root topAnchor] constant: 20],
        [[f leadingAnchor] constraintEqualToAnchor: [l trailingAnchor] constant: 8],
        [[f trailingAnchor] constraintEqualToAnchor: [root trailingAnchor] constant: -20],
        [[f firstBaselineAnchor] constraintEqualToAnchor: [l firstBaselineAnchor]],
        [[f widthAnchor] constraintGreaterThanOrEqualToConstant: 50],
        [[cancel topAnchor] constraintGreaterThanOrEqualToAnchor: [f bottomAnchor] constant: 20],
    ]];
    printf("fittingSize %s\n", S([root fittingSize]).UTF8String);
    [root layoutSubtreeIfNeeded];
    dump(root, 0);
    NSLayoutConstraint *c = [NSLayoutConstraint constraintWithItem: ok attribute: NSLayoutAttributeWidth relatedBy: NSLayoutRelationEqual
                                                            toItem: nil attribute: NSLayoutAttributeNotAnAttribute multiplier: 1 constant: 90];
    [c setPriority: 750];
    [c setActive: YES];
    [root layoutSubtreeIfNeeded];
    printf("OK mit Breite 90@750: %s cancel %s\n", R([ok frame]).UTF8String, R([cancel frame]).UTF8String);
    printf("Bedingungen an root: %lu\n", (unsigned long) [[root constraints] count]);
}

static void window_case(void) {
    printf("== Fenster wächst mit Inhalt\n");
    NSWindow *w = [[NSWindow alloc] initWithContentRect: NSMakeRect(100, 100, 200, 100) styleMask: NSWindowStyleMaskTitled
                                                backing: NSBackingStoreBuffered defer: NO];
    NSView *cv = [w contentView];
    NSButton *a = check(@"A rather long checkbox title that needs room");
    NSButton *b = check(@"Second");
    for (NSView *v in @[ a, b ]) { [v setTranslatesAutoresizingMaskIntoConstraints: NO]; [cv addSubview: v]; }
    [NSLayoutConstraint activateConstraints: @[
        [[a leadingAnchor] constraintEqualToAnchor: [cv leadingAnchor] constant: 20],
        [[cv trailingAnchor] constraintEqualToAnchor: [a trailingAnchor] constant: 20],
        [[a topAnchor] constraintEqualToAnchor: [cv topAnchor] constant: 20],
        [[b leadingAnchor] constraintEqualToAnchor: [a leadingAnchor]],
        [[b topAnchor] constraintEqualToAnchor: [a bottomAnchor] constant: 8],
        [[cv bottomAnchor] constraintEqualToAnchor: [b bottomAnchor] constant: 120],
    ]];
    printf("vorher content=%s\n", R([cv frame]).UTF8String);
    [w layoutIfNeeded];
    printf("nach layoutIfNeeded window=%s content=%s a=%s b=%s\n", R([w frame]).UTF8String, R([cv frame]).UTF8String,
           R([a frame]).UTF8String, R([b frame]).UTF8String);
}

static void grid_case(void) {
    printf("== NSGridView\n");
    NSGridView *g = [NSGridView gridViewWithViews: @[
        @[ [NSTextField labelWithString: @"Start:"], [NSTextField textFieldWithString: @"1 1 1 1"] ],
        @[ [NSTextField labelWithString: @"Mode:"], [NSPopUpButton new] ],
        @[ [NSGridCell emptyContentView], check(@"Include Audio Tail") ],
        @[ [NSGridCell emptyContentView], check(@"Include Tempo Information") ],
        @[ [NSTextField labelWithString: @"Normalize:"], [NSButton buttonWithTitle: @"Overload Protection Only" target: nil action: nil] ],
    ]];
    [g setRowSpacing: 6];
    [g setColumnSpacing: 4];
    [[g columnAtIndex: 0] setXPlacement: NSGridCellPlacementTrailing];
    [g setRowAlignment: NSGridRowAlignmentFirstBaseline];
    [[g rowAtIndex: 3] setTopPadding: 10];
    printf("fittingSize %s\n", S([g fittingSize]).UTF8String);
    NSView *root = [[NSView alloc] initWithFrame: NSMakeRect(0, 0, 500, 300)];
    [g setTranslatesAutoresizingMaskIntoConstraints: NO];
    [root addSubview: g];
    [NSLayoutConstraint activateConstraints: @[ [[g leadingAnchor] constraintEqualToAnchor: [root leadingAnchor] constant: 20],
                                                [[g topAnchor] constraintEqualToAnchor: [root topAnchor] constant: 20] ]];
    [root layoutSubtreeIfNeeded];
    dump(root, 0);
}

int main(int argc, char **argv) {
    @autoreleasepool {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy: NSApplicationActivationPolicyAccessory];
        if (argc > 1) { nibcase(argv[1], argc > 2 ? argv[2] : NULL); return 0; }
        void (*cases[])(void) = { ics_case, constraint_case, window_case, grid_case };
        for (int i = 0; i < 4; i++) {
            @try { cases[i](); }
            @catch (NSException *e) { printf("Ausnahme: %s\n", [[e reason] UTF8String]); }
        }
    }
    return 0;
}
