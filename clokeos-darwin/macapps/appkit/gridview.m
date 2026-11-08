/* NSGridView, NSGridRow, NSGridColumn, NSGridCell für Cocotrons AppKit (dort Platzhalter, die jede Nachricht verschlucken).
 *
 * Wie bei Apple legt das Gitter seine Ansichten über Bedingungen aus (auf dem Mac abgelesen, Ausgabe von -constraints):
 *   - Spalten/Zeilen hängen ohne Lücke aneinander (Abstand columnSpacing/rowSpacing), erste und letzte an den Rändern des Gitters;
 *   - jede Zelle liegt in ihrer Spalte/Zeile (Pflicht, abzüglich der Ränder leading/trailing/top/bottomPadding);
 *   - Spalten und Zeilen schmiegen sich mit Priorität 249 an ihren Inhalt;
 *   - Platzierung (leading/trailing/center/fill, top/bottom/center/fill; "inherited" = von Zelle über Zeile/Spalte zum Gitter,
 *     Vorgabe leading/top) ist Pflicht, senkrecht in Zeilen mit Grundlinienausrichtung nur 650;
 *   - Grundlinienausrichtung (rowAlignment firstBaseline/lastBaseline): Grundlinien der Zellen einer Zeile gleich.
 * Inhalte bekommen translatesAutoresizingMaskIntoConstraints = NO (wie bei Apple).
 * Spalten und Zeilen sind Layout-Hilfslinien des Gitters; die Bedingungen erzeugt -_clkGridConstraints bei jedem Lösen neu
 * (autolayout.m). Feste Breiten/Höhen (width/height != NSGridViewSizeForContent) sind Pflicht.
 * Nib-Schlüssel wie bei Apple: NSGrid_rows, NSGrid_columns, NSGrid_rowSpacing, NSGrid_columnSpacing, NSGrid_xPlacement,
 * NSGrid_yPlacement, NSGrid_alignment; Zeile/Spalte: NSGrid_height/width, NSGrid_*Padding, NSGrid_hidden, NSGrid_cells;
 * Zelle: NSGrid_content, NSGrid_mergeHead, NSGrid_xPlacement, NSGrid_yPlacement, NSGrid_alignment. */
#import <AppKit/AppKit.h>
#import <AppKit/NSLayoutGuide.h>
#import <Foundation/NSLayoutConstraint.h>
#import <objc/runtime.h>
#include <float.h>

/* Apples Wert (FLT_MIN); Nibs speichern ihn als Breite/Höhe "nach Inhalt" */
const CGFloat NSGridViewSizeForContent = 1.17549435e-38;

enum { GP_Inherited = 0, GP_None, GP_Leading, GP_Trailing, GP_Center, GP_Fill, GP_Top = GP_Leading, GP_Bottom = GP_Trailing };
enum { GA_Inherited = 0, GA_None, GA_FirstBaseline, GA_LastBaseline };
enum { LA_Leading = 5, LA_Trailing = 6, LA_Top = 3, LA_Bottom = 4, LA_Width = 7, LA_Height = 8, LA_CenterX = 9, LA_CenterY = 10,
       LA_LastBaseline = 11, LA_FirstBaseline = 12 };

@interface NSLayoutConstraint (CLKGrid)
+ (instancetype) constraintWithItem: (id) v1 attribute: (NSInteger) a1 relatedBy: (NSInteger) rel toItem: (id) v2
                          attribute: (NSInteger) a2 multiplier: (double) m constant: (double) c;
- (void) setPriority: (float) p;
- (void) setIdentifier: (NSString *) s;
@end
@interface NSLayoutGuide (CLKGrid)
- (void) setOwningView: (NSView *) v;
@end

static BOOL for_content(CGFloat x) { return x <= 2 * NSGridViewSizeForContent; }

@class NSGridView, NSGridRow, NSGridColumn;

@interface NSGridCell : NSObject <NSCoding> {
@public
    NSView *_content;
    NSGridRow *_row;          /* nicht gehalten */
    NSGridColumn *_col;       /* nicht gehalten */
    NSGridCell *_head;        /* nicht gehalten */
    NSInteger _xPl, _yPl, _align;
    NSArray *_custom;
}
@end

@interface NSGridRow : NSObject <NSCoding> {
@public
    NSGridView *_grid;        /* nicht gehalten */
    NSMutableArray *_cells;
    NSInteger _yPl, _align;
    CGFloat _height, _top, _bottom;
    BOOL _hidden;
    NSLayoutGuide *_guide;
}
@end

@interface NSGridColumn : NSObject <NSCoding> {
@public
    NSGridView *_grid;
    NSInteger _xPl;
    CGFloat _width, _lead, _trail;
    BOOL _hidden;
    NSLayoutGuide *_guide;
}
@end

@interface NSView (CLKGridPrivate)
- (void) _removeViewWithoutDisplay: (NSView *) v;
@end

@interface NSGridView : NSView {
@public
    NSMutableArray *_rows, *_cols;
    CGFloat _rowSpacing, _colSpacing;
    NSInteger _xPl, _yPl, _align;
}
- (void) _clkChanged;
@end

/* leere Zelle */
@interface _CLKGridEmptyContentView : NSView
@end
@implementation _CLKGridEmptyContentView
@end
static NSView *empty_view(void) {
    static NSView *v;
    if (!v) v = [[_CLKGridEmptyContentView alloc] initWithFrame: NSZeroRect];
    return v;
}
static BOOL is_empty(NSView *v) { return !v || v == empty_view(); }

@implementation NSGridCell
+ (NSView *) emptyContentView { return empty_view(); }
- (id) initWithCoder: (NSCoder *) c {
    if (!(self = [super init])) return nil;
    if ([c allowsKeyedCoding]) {
        _content = [[c decodeObjectForKey: @"NSGrid_content"] retain];
        _head = [c decodeObjectForKey: @"NSGrid_mergeHead"];
        _xPl = [c decodeIntegerForKey: @"NSGrid_xPlacement"];
        _yPl = [c decodeIntegerForKey: @"NSGrid_yPlacement"];
        _align = [c decodeIntegerForKey: @"NSGrid_alignment"];
    }
    return self;
}
- (void) encodeWithCoder: (NSCoder *) c {}
- (void) dealloc { [_content release]; [_custom release]; [super dealloc]; }
- (NSView *) contentView { return is_empty(_content) ? nil : _content; }
- (void) setContentView: (NSView *) v {
    NSGridView *g = _row ? _row->_grid : nil;
    if (_content == v) return;
    if (!is_empty(_content) && [_content superview] == g) [_content removeFromSuperview];
    [_content release];
    _content = [v retain];
    if (!is_empty(v)) [v setTranslatesAutoresizingMaskIntoConstraints: NO];
    if (g && !is_empty(v)) [g addSubview: v];
    [g _clkChanged];
}
- (NSGridRow *) row { return _row; }
- (NSGridColumn *) column { return _col; }
- (NSInteger) xPlacement { return _xPl; }
- (void) setXPlacement: (NSInteger) p { _xPl = p; [_row->_grid _clkChanged]; }
- (NSInteger) yPlacement { return _yPl; }
- (void) setYPlacement: (NSInteger) p { _yPl = p; [_row->_grid _clkChanged]; }
- (NSInteger) rowAlignment { return _align; }
- (void) setRowAlignment: (NSInteger) a { _align = a; [_row->_grid _clkChanged]; }
- (NSArray *) customPlacementConstraints { return _custom ?: [NSArray array]; }
- (void) setCustomPlacementConstraints: (NSArray *) a { [_custom autorelease]; _custom = [a copy]; [_row->_grid _clkChanged]; }
@end

@implementation NSGridRow
- (id) initWithCoder: (NSCoder *) c {
    if (!(self = [super init])) return nil;
    _height = NSGridViewSizeForContent;
    if ([c allowsKeyedCoding]) {
        _yPl = [c decodeIntegerForKey: @"NSGrid_yPlacement"];
        _align = [c decodeIntegerForKey: @"NSGrid_alignment"];
        if ([c containsValueForKey: @"NSGrid_height"]) _height = [c decodeDoubleForKey: @"NSGrid_height"];
        _top = [c decodeDoubleForKey: @"NSGrid_topPadding"];
        _bottom = [c decodeDoubleForKey: @"NSGrid_bottomPadding"];
        _hidden = [c decodeBoolForKey: @"NSGrid_hidden"];
        _cells = [[NSMutableArray alloc] initWithArray: [c decodeObjectForKey: @"NSGrid_cells"] ?: [NSArray array]];
    }
    return self;
}
- (id) init { if ((self = [super init])) { _cells = [NSMutableArray new]; _height = NSGridViewSizeForContent; } return self; }
- (void) encodeWithCoder: (NSCoder *) c {}
- (void) dealloc { [_cells release]; [_guide release]; [super dealloc]; }
- (NSGridView *) gridView { return _grid; }
- (NSInteger) numberOfCells { return [_cells count]; }
- (NSGridCell *) cellAtIndex: (NSInteger) i { return [_cells objectAtIndex: i]; }
- (NSInteger) yPlacement { return _yPl; }
- (void) setYPlacement: (NSInteger) p { _yPl = p; [_grid _clkChanged]; }
- (NSInteger) rowAlignment { return _align; }
- (void) setRowAlignment: (NSInteger) a { _align = a; [_grid _clkChanged]; }
- (CGFloat) height { return _height; }
- (void) setHeight: (CGFloat) h { _height = h; [_grid _clkChanged]; }
- (CGFloat) topPadding { return _top; }
- (void) setTopPadding: (CGFloat) p { _top = p; [_grid _clkChanged]; }
- (CGFloat) bottomPadding { return _bottom; }
- (void) setBottomPadding: (CGFloat) p { _bottom = p; [_grid _clkChanged]; }
- (BOOL) isHidden { return _hidden; }
- (void) setHidden: (BOOL) h {
    _hidden = h;
    for (NSGridCell *c in _cells) [[c contentView] setHidden: h];
    [_grid _clkChanged];
}
- (void) mergeCellsInRange: (NSRange) r {
    if (!r.length || NSMaxRange(r) > [_cells count]) return;
    NSGridCell *head = [_cells objectAtIndex: r.location];
    for (NSUInteger i = r.location; i < NSMaxRange(r); i++) ((NSGridCell *) [_cells objectAtIndex: i])->_head = head;
    [_grid _clkChanged];
}
@end

@implementation NSGridColumn
- (id) initWithCoder: (NSCoder *) c {
    if (!(self = [super init])) return nil;
    _width = NSGridViewSizeForContent;
    if ([c allowsKeyedCoding]) {
        _xPl = [c decodeIntegerForKey: @"NSGrid_xPlacement"];
        if ([c containsValueForKey: @"NSGrid_width"]) _width = [c decodeDoubleForKey: @"NSGrid_width"];
        _lead = [c decodeDoubleForKey: @"NSGrid_leadingPadding"];
        _trail = [c decodeDoubleForKey: @"NSGrid_trailingPadding"];
        _hidden = [c decodeBoolForKey: @"NSGrid_hidden"];
    }
    return self;
}
- (id) init { if ((self = [super init])) _width = NSGridViewSizeForContent; return self; }
- (void) encodeWithCoder: (NSCoder *) c {}
- (void) dealloc { [_guide release]; [super dealloc]; }
- (NSGridView *) gridView { return _grid; }
- (NSInteger) _index { return [_grid->_cols indexOfObjectIdenticalTo: self]; }
- (NSInteger) numberOfCells { return [_grid->_rows count]; }
- (NSGridCell *) cellAtIndex: (NSInteger) i { return [((NSGridRow *) [_grid->_rows objectAtIndex: i])->_cells objectAtIndex: [self _index]]; }
- (NSInteger) xPlacement { return _xPl; }
- (void) setXPlacement: (NSInteger) p { _xPl = p; [_grid _clkChanged]; }
- (CGFloat) width { return _width; }
- (void) setWidth: (CGFloat) w { _width = w; [_grid _clkChanged]; }
- (CGFloat) leadingPadding { return _lead; }
- (void) setLeadingPadding: (CGFloat) p { _lead = p; [_grid _clkChanged]; }
- (CGFloat) trailingPadding { return _trail; }
- (void) setTrailingPadding: (CGFloat) p { _trail = p; [_grid _clkChanged]; }
- (BOOL) isHidden { return _hidden; }
- (void) setHidden: (BOOL) h {
    _hidden = h;
    NSInteger i = [self _index];
    for (NSGridRow *r in _grid->_rows) if (i < (NSInteger) [r->_cells count]) [[[r->_cells objectAtIndex: i] contentView] setHidden: h];
    [_grid _clkChanged];
}
- (void) mergeCellsInRange: (NSRange) r {
    NSInteger i = [self _index];
    if (!r.length || NSMaxRange(r) > [_grid->_rows count]) return;
    NSGridCell *head = [((NSGridRow *) [_grid->_rows objectAtIndex: r.location])->_cells objectAtIndex: i];
    for (NSUInteger j = r.location; j < NSMaxRange(r); j++)
        ((NSGridCell *) [((NSGridRow *) [_grid->_rows objectAtIndex: j])->_cells objectAtIndex: i])->_head = head;
    [_grid _clkChanged];
}
@end

@implementation NSGridView

- (void) _clkSetup {
    if (!_rows) _rows = [NSMutableArray new];
    if (!_cols) _cols = [NSMutableArray new];
}
/* Zellen mit Zeile/Spalte verbinden (nach dem Laden und nach jeder Änderung der Struktur) */
- (void) _clkLink {
    for (NSGridColumn *c in _cols) c->_grid = self;
    for (NSGridRow *r in _rows) {
        r->_grid = self;
        while ([r->_cells count] < [_cols count]) {
            NSGridCell *c = [[NSGridCell new] autorelease];
            [r->_cells addObject: c];
        }
        NSUInteger i = 0;
        for (NSGridCell *c in r->_cells) {
            c->_row = r;
            c->_col = i < [_cols count] ? [_cols objectAtIndex: i] : nil;
            i++;
        }
    }
}
- (void) _clkChanged {
    [self invalidateIntrinsicContentSize];
    if ([self respondsToSelector: @selector(setNeedsLayout:)]) [self setNeedsLayout: YES];
}

- (id) initWithFrame: (NSRect) f {
    if ((self = [super initWithFrame: f])) {
        [self _clkSetup];
        _xPl = GP_Leading; _yPl = GP_Top;
        _rowSpacing = 6; _colSpacing = 6;
    }
    return self;
}
- (id) initWithCoder: (NSCoder *) c {
    if (!(self = [super initWithCoder: c])) return nil;
    [self _clkSetup];
    if ([c allowsKeyedCoding]) {
        _rowSpacing = [c decodeDoubleForKey: @"NSGrid_rowSpacing"];
        _colSpacing = [c decodeDoubleForKey: @"NSGrid_columnSpacing"];
        _xPl = [c decodeIntegerForKey: @"NSGrid_xPlacement"];
        _yPl = [c decodeIntegerForKey: @"NSGrid_yPlacement"];
        _align = [c decodeIntegerForKey: @"NSGrid_alignment"];
        [_rows addObjectsFromArray: [c decodeObjectForKey: @"NSGrid_rows"] ?: [NSArray array]];
        [_cols addObjectsFromArray: [c decodeObjectForKey: @"NSGrid_columns"] ?: [NSArray array]];
    }
    [self _clkLink];
    return self;
}
- (void) dealloc { [_rows release]; [_cols release]; [super dealloc]; }

/* Gitter aus Ansichten: Zeilen von oben */
- (void) _clkAppendRow: (NSArray *) views at: (NSInteger) at {
    NSGridRow *r = [[NSGridRow new] autorelease];
    r->_grid = self;
    while ([_cols count] < [views count]) {
        NSGridColumn *c = [[NSGridColumn new] autorelease];
        c->_grid = self;
        [_cols addObject: c];
        for (NSGridRow *o in _rows) [o->_cells addObject: [[NSGridCell new] autorelease]];
    }
    for (NSUInteger i = 0; i < [_cols count]; i++) {
        NSGridCell *c = [[NSGridCell new] autorelease];
        NSView *v = i < [views count] ? [views objectAtIndex: i] : nil;
        if (!is_empty(v)) {
            c->_content = [v retain];
            [v setTranslatesAutoresizingMaskIntoConstraints: NO];
            [self addSubview: v];
        }
        [r->_cells addObject: c];
    }
    if (at < 0 || at >= (NSInteger) [_rows count]) [_rows addObject: r];
    else [_rows insertObject: r atIndex: at];
    [self _clkLink];
    [self _clkChanged];
}
+ (instancetype) gridViewWithViews: (NSArray *) rows {
    NSGridView *g = [[[self alloc] initWithFrame: NSZeroRect] autorelease];
    for (NSArray *r in rows) [g _clkAppendRow: r at: -1];
    return g;
}
+ (instancetype) gridViewWithNumberOfColumns: (NSInteger) nc rows: (NSInteger) nr {
    NSGridView *g = [[[self alloc] initWithFrame: NSZeroRect] autorelease];
    NSMutableArray *empty = [NSMutableArray array];
    for (NSInteger i = 0; i < nc; i++) [empty addObject: empty_view()];
    for (NSInteger j = 0; j < nr; j++) [g _clkAppendRow: empty at: -1];
    return g;
}

- (NSInteger) numberOfRows { return [_rows count]; }
- (NSInteger) numberOfColumns { return [_cols count]; }
- (NSGridRow *) rowAtIndex: (NSInteger) i { return [_rows objectAtIndex: i]; }
- (NSGridColumn *) columnAtIndex: (NSInteger) i { return [_cols objectAtIndex: i]; }
- (NSInteger) indexOfRow: (NSGridRow *) r { return [_rows indexOfObjectIdenticalTo: r]; }
- (NSInteger) indexOfColumn: (NSGridColumn *) c { return [_cols indexOfObjectIdenticalTo: c]; }
- (NSGridCell *) cellAtColumnIndex: (NSInteger) c rowIndex: (NSInteger) r {
    return [((NSGridRow *) [_rows objectAtIndex: r])->_cells objectAtIndex: c];
}
- (NSGridCell *) cellForView: (NSView *) v {
    for (NSView *x = v; x && x != self; x = [x superview])
        for (NSGridRow *r in _rows)
            for (NSGridCell *c in r->_cells)
                if (c->_content == x) return c;
    return nil;
}
- (NSGridRow *) addRowWithViews: (NSArray *) views { [self _clkAppendRow: views at: -1]; return [_rows lastObject]; }
- (NSGridRow *) insertRowAtIndex: (NSInteger) i withViews: (NSArray *) views {
    [self _clkAppendRow: views at: i];
    return [_rows objectAtIndex: MIN(i, (NSInteger) [_rows count] - 1)];
}
- (void) removeRowAtIndex: (NSInteger) i {
    NSGridRow *r = [_rows objectAtIndex: i];
    for (NSGridCell *c in r->_cells) if (!is_empty(c->_content) && [c->_content superview] == self) [c->_content removeFromSuperview];
    [_rows removeObjectAtIndex: i];
    [self _clkChanged];
}
- (void) moveRowAtIndex: (NSInteger) from toIndex: (NSInteger) to {
    NSGridRow *r = [[_rows objectAtIndex: from] retain];
    [_rows removeObjectAtIndex: from];
    [_rows insertObject: r atIndex: to];
    [r release];
    [self _clkChanged];
}
- (NSGridColumn *) insertColumnAtIndex: (NSInteger) i withViews: (NSArray *) views {
    NSGridColumn *c = [[NSGridColumn new] autorelease];
    c->_grid = self;
    if (i < 0 || i > (NSInteger) [_cols count]) i = [_cols count];
    [_cols insertObject: c atIndex: i];
    NSUInteger j = 0;
    for (NSGridRow *r in _rows) {
        NSGridCell *cell = [[NSGridCell new] autorelease];
        NSView *v = j < [views count] ? [views objectAtIndex: j] : nil;
        if (!is_empty(v)) { cell->_content = [v retain]; [v setTranslatesAutoresizingMaskIntoConstraints: NO]; [self addSubview: v]; }
        [r->_cells insertObject: cell atIndex: i];
        j++;
    }
    while (j < [views count]) {   /* mehr Ansichten als Zeilen: neue Zeilen */
        NSMutableArray *row = [NSMutableArray array];
        for (NSUInteger k = 0; k < [_cols count]; k++) [row addObject: k == (NSUInteger) i ? [views objectAtIndex: j] : empty_view()];
        [self _clkAppendRow: row at: -1];
        j++;
    }
    [self _clkLink];
    [self _clkChanged];
    return c;
}
- (NSGridColumn *) addColumnWithViews: (NSArray *) views { return [self insertColumnAtIndex: [_cols count] withViews: views]; }
- (void) removeColumnAtIndex: (NSInteger) i {
    for (NSGridRow *r in _rows) {
        NSGridCell *c = [r->_cells objectAtIndex: i];
        if (!is_empty(c->_content) && [c->_content superview] == self) [c->_content removeFromSuperview];
        [r->_cells removeObjectAtIndex: i];
    }
    [_cols removeObjectAtIndex: i];
    [self _clkLink];
    [self _clkChanged];
}
- (void) mergeCellsInHorizontalRange: (NSRange) h verticalRange: (NSRange) v {
    if (!h.length || !v.length) return;
    NSGridCell *head = [self cellAtColumnIndex: h.location rowIndex: v.location];
    for (NSUInteger j = v.location; j < NSMaxRange(v); j++)
        for (NSUInteger i = h.location; i < NSMaxRange(h); i++) [self cellAtColumnIndex: i rowIndex: j]->_head = head;
    [self _clkChanged];
}

/* Wie bei Apple: Ersetzen einer Zellansicht ersetzt den Inhalt der Zelle (neue Ansicht ohne Autoresizing-Maske), Herausnehmen
 * leert die Zelle. Cocotron ruft willRemoveSubview: nicht; jedes Herausnehmen läuft über -_removeViewWithoutDisplay: der Oberansicht. */
- (NSGridCell *) _clkCellWithContent: (NSView *) v {
    for (NSGridRow *r in _rows)
        for (NSGridCell *c in r->_cells)
            if (c->_content == v) return c;
    return nil;
}
- (void) replaceSubview: (NSView *) old with: (NSView *) new {
    NSGridCell *c = [self _clkCellWithContent: old];
    [super replaceSubview: old with: new];
    if (c && new) {
        [new setTranslatesAutoresizingMaskIntoConstraints: NO];
        [c->_content release];
        c->_content = [new retain];
    }
    [self _clkChanged];
}
- (void) _removeViewWithoutDisplay: (NSView *) v {
    NSGridCell *c = [self _clkCellWithContent: v];
    if (c) {
        [c->_content autorelease];
        c->_content = nil;
    }
    [super _removeViewWithoutDisplay: v];
    [self _clkChanged];
}

- (CGFloat) rowSpacing { return _rowSpacing; }
- (void) setRowSpacing: (CGFloat) s { _rowSpacing = s; [self _clkChanged]; }
- (CGFloat) columnSpacing { return _colSpacing; }
- (void) setColumnSpacing: (CGFloat) s { _colSpacing = s; [self _clkChanged]; }
- (NSInteger) xPlacement { return _xPl; }
- (void) setXPlacement: (NSInteger) p { _xPl = p; [self _clkChanged]; }
- (NSInteger) yPlacement { return _yPl; }
- (void) setYPlacement: (NSInteger) p { _yPl = p; [self _clkChanged]; }
- (NSInteger) rowAlignment { return _align; }
- (void) setRowAlignment: (NSInteger) a { _align = a; [self _clkChanged]; }

/* ---- Bedingungen ---- */
static NSLayoutConstraint *K(id a, NSInteger aa, NSInteger rel, id b, NSInteger ba, double k, float prio) {
    NSLayoutConstraint *c = [NSLayoutConstraint constraintWithItem: a attribute: aa relatedBy: rel toItem: b attribute: ba multiplier: 1 constant: k];
    if (prio < 1000) [c setPriority: prio];
    return c;
}
static NSLayoutGuide *guide(NSGridView *g, NSLayoutGuide **slot) {
    if (!*slot) {
        *slot = [NSLayoutGuide new];
        [*slot setOwningView: g];
    }
    return *slot;
}
static NSInteger resolve(NSInteger a, NSInteger b, NSInteger c, NSInteger dflt) {
    if (a) return a;
    if (b) return b;
    if (c) return c;
    return dflt;
}

- (NSArray *) _clkGridConstraints {
    NSMutableArray *out = [NSMutableArray array];
    if ([self isHidden]) return out;
    NSMutableArray *vc = [NSMutableArray array], *vr = [NSMutableArray array];
    for (NSGridColumn *c in _cols) if (!c->_hidden) [vc addObject: c];
    for (NSGridRow *r in _rows) if (!r->_hidden) [vr addObject: r];
    if (![vc count] || ![vr count]) return out;
    /* Spalten und Zeilen hintereinander */
    id prev = nil;
    for (NSGridColumn *c in vc) {
        NSLayoutGuide *gd = guide(self, &c->_guide);
        if (!prev) [out addObject: K(gd, LA_Leading, 0, self, LA_Leading, 0, 1000)];
        else [out addObject: K(gd, LA_Leading, 0, prev, LA_Trailing, _colSpacing, 1000)];
        if (!for_content(c->_width)) [out addObject: K(gd, LA_Width, 0, nil, 0, c->_width, 1000)];
        else [out addObject: K(gd, LA_Width, 1, nil, 0, 0, 1000)];
        prev = gd;
    }
    [out addObject: K(self, LA_Trailing, 0, prev, LA_Trailing, 0, 1000)];
    prev = nil;
    for (NSGridRow *r in vr) {
        NSLayoutGuide *gd = guide(self, &r->_guide);
        if (!prev) [out addObject: K(gd, LA_Top, 0, self, LA_Top, 0, 1000)];
        else [out addObject: K(gd, LA_Top, 0, prev, LA_Bottom, _rowSpacing, 1000)];
        if (!for_content(r->_height)) [out addObject: K(gd, LA_Height, 0, nil, 0, r->_height, 1000)];
        else [out addObject: K(gd, LA_Height, 1, nil, 0, 0, 1000)];
        prev = gd;
    }
    [out addObject: K(self, LA_Bottom, 0, prev, LA_Bottom, 0, 1000)];

    /* Zellen */
    for (NSGridRow *r in vr) {
        id baseRef = nil;
        for (NSUInteger ci = 0; ci < [r->_cells count] && ci < [_cols count]; ci++) {
            NSGridCell *cell = [r->_cells objectAtIndex: ci];
            NSGridColumn *col = [_cols objectAtIndex: ci];
            NSView *v = cell->_content;
            if (col->_hidden || is_empty(v) || [v superview] != self) continue;
            if (cell->_head && cell->_head != cell) continue;
            /* Ausdehnung verbundener Zellen */
            NSGridColumn *lastCol = col;
            NSGridRow *lastRow = r;
            for (NSGridRow *r2 in vr)
                for (NSUInteger c2 = 0; c2 < [r2->_cells count] && c2 < [_cols count]; c2++) {
                    NSGridCell *x = [r2->_cells objectAtIndex: c2];
                    if (x->_head != cell) continue;
                    NSGridColumn *cc = [_cols objectAtIndex: c2];
                    if (!cc->_hidden && [_cols indexOfObjectIdenticalTo: cc] > [_cols indexOfObjectIdenticalTo: lastCol]) lastCol = cc;
                    if ([_rows indexOfObjectIdenticalTo: r2] > [_rows indexOfObjectIdenticalTo: lastRow]) lastRow = r2;
                }
            NSLayoutGuide *gl = col->_guide, *gr = lastCol->_guide, *gt = r->_guide, *gb = lastRow->_guide;
            CGFloat lp = col->_lead, tp = lastCol->_trail, tpad = r->_top, bpad = lastRow->_bottom;
            /* enthalten (Pflicht) und anschmiegen (249) */
            [out addObject: K(v, LA_Leading, 1, gl, LA_Leading, lp, 1000)];
            [out addObject: K(v, LA_Trailing, -1, gr, LA_Trailing, -tp, 1000)];
            [out addObject: K(v, LA_Top, 1, gt, LA_Top, tpad, 1000)];
            [out addObject: K(v, LA_Bottom, -1, gb, LA_Bottom, -bpad, 1000)];
            [out addObject: K(v, LA_Leading, -1, gl, LA_Leading, lp, 249)];
            [out addObject: K(v, LA_Trailing, 1, gr, LA_Trailing, -tp, 249)];
            [out addObject: K(v, LA_Top, -1, gt, LA_Top, tpad, 249)];
            [out addObject: K(v, LA_Bottom, 1, gb, LA_Bottom, -bpad, 249)];
            /* Platzierung */
            NSInteger align = resolve(cell->_align, r->_align, _align, GA_None);
            BOOL baseline = (align == GA_FirstBaseline || align == GA_LastBaseline) && lastRow == r;
            NSInteger xp = resolve(cell->_xPl, col->_xPl, _xPl, GP_Leading), yp = resolve(cell->_yPl, r->_yPl, _yPl, GP_Top);
            float yprio = baseline ? 650 : 1000;
            if (xp == GP_Leading || xp == GP_Fill) [out addObject: K(v, LA_Leading, 0, gl, LA_Leading, lp, 1000)];
            if (xp == GP_Trailing || xp == GP_Fill) [out addObject: K(v, LA_Trailing, 0, gr, LA_Trailing, -tp, 1000)];
            if (xp == GP_Center && gl == gr) [out addObject: K(v, LA_CenterX, 0, gl, LA_CenterX, (lp - tp) / 2, 1000)];
            if (yp == GP_Top || yp == GP_Fill) [out addObject: K(v, LA_Top, 0, gt, LA_Top, tpad, yprio)];
            if (yp == GP_Bottom || yp == GP_Fill) [out addObject: K(v, LA_Bottom, 0, gb, LA_Bottom, -bpad, yprio)];
            if (yp == GP_Center && gt == gb) [out addObject: K(v, LA_CenterY, 0, gt, LA_CenterY, (tpad - bpad) / 2, yprio)];
            if (baseline) {
                NSInteger ba = align == GA_FirstBaseline ? LA_FirstBaseline : LA_LastBaseline;
                if (!baseRef) baseRef = v;
                else [out addObject: K(v, ba, 0, baseRef, ba, 0, 1000)];
            }
            if (cell->_custom) [out addObjectsFromArray: cell->_custom];
        }
    }
    return out;
}

@end
