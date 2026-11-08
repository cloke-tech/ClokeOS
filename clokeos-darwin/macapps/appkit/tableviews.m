/* Ansichtsbasierte Tabellen für Cocotrons NSTableView/NSOutlineView (docs/logic-launch.md, Runde 62).
 *
 * Cocotron zeichnete nur Zellen. Tabellen, deren Delegat tableView:viewForTableColumn:row: (bzw. outlineView:viewForTableColumn:item:)
 * anbietet oder deren Nib Vorlagen-Ansichten mitbringt (NSTableViewArchivedReusableViewsKey: Kennung -> eingebettete NSNib), blieben
 * leer (Logic Pros Bounce-Dialog: Ankreuzfelder ohne Namen der Ziele). Hier wie bei Apple:
 *   - NSTableCellView (objectValue, textField, imageView, backgroundStyle, rowSizeStyle) und NSTableRowView (selected, emphasized,
 *     Hintergrund, Auswahl zeichnen) statt Darlings Platzhaltern (patches/…/NSTableCellView.m.sed, NSTableRowView.m.sed).
 *   - makeViewWithIdentifier:owner: (Wiederverwendung, sonst Nib mit dem Besitzer laden; die erste Ansicht der obersten Ebene bekommt die
 *     Kennung), registerNib:forIdentifier:, registeredNibsByIdentifier, viewAtColumn:row:makeIfNecessary:, rowViewAtRow:makeIfNecessary:,
 *     rowForView:, columnForView:, enumerateAvailableRowViewsUsingBlock:, begin/endUpdates, insert/remove/moveRow…,
 *     reloadDataForRowIndexes:columnIndexes:, -[NSNib instantiateWithOwner:topLevelObjects:].
 *   - Für die sichtbaren Zeilen entsteht je eine Zeilenansicht (Delegat tableView:rowViewForRow: oder NSTableRowView) als Unteransicht
 *     der Tabelle, darin je Spalte die Ansicht des Delegaten (ohne viewFor… die Vorlage mit der Kennung der Spalte); liefert die
 *     Datenquelle objectValue…, bekommt die Ansicht ihn über setObjectValue:. Abgleich im Layout-Durchgang (layout.m) und vor dem
 *     Zeichnen; Zeilen, die aus dem Bild scrollen, wandern in die Wiederverwendung. reloadData fragt alle sichtbaren Ansichten neu ab.
 *   - Rahmen wie AppKit: Zeile = rectOfRow: über die volle Tabellenbreite; Zelle = Spalte ohne Zwischenraum (je halber Abstand an
 *     beiden Seiten), Höhe = Zeilenhöhe; in der Gliederungsspalte einer NSOutlineView um Ebene × Einzug + Dreieck eingerückt.
 *   - Cocotrons Zellzeichnen, Auswahlfläche, Klick auf Knopfzellen und Bearbeiten per Doppelklick entfallen bei solchen Tabellen
 *     (patches/…/NSTableView.m.sed, NSOutlineView.m.sed); die Zeilenansicht zeichnet die Auswahl. Klicks auf Beschriftungen, Bilder und
 *     leere Flächen einer Zeile gehen an die Tabelle (Zeile wählen), aktive Steuerelemente bekommen sie selbst.
 * Spur: CLOKE_TABLE_TRACE=1 meldet angelegte/entfernte Zeilen und Nib-Ansichten auf stderr. */
#import <AppKit/AppKit.h>
#import <AppKit/NSTableCellView.h>
#import <AppKit/NSTableRowView.h>
#import <AppKit/NSNib.h>
#import <objc/runtime.h>
#include <stdlib.h>

@interface NSView (CLKTableDecl)
- (void) setNeedsLayout: (BOOL) f;
- (void) layout;
- (void) viewWillDraw;
- (void) setIdentifier: (NSString *) i;
- (NSString *) identifier;
@end
@interface NSTableView (CLKTableCocotron)
- (id) dataSourceObjectValueForTableColumn: (NSTableColumn *) c row: (NSInteger) r;
- (NSInteger) rowForView: (NSView *) v;
- (id) viewAtColumn: (NSInteger) c row: (NSInteger) r makeIfNecessary: (BOOL) m;
- (id) makeViewWithIdentifier: (NSString *) i owner: (id) o;
- (NSInteger) selectionHighlightStyle;
@end
@interface NSTableRowView (CLKTableDecl)
- (void) _clkUpdateInterior;
- (void) setSelected: (BOOL) f;
- (void) setPreviousRowSelected: (BOOL) f;
- (void) setNextRowSelected: (BOOL) f;
- (void) setEmphasized: (BOOL) f;
- (void) setSelectionHighlightStyle: (NSInteger) s;
@end

static int g_trace = -1;
static BOOL trace(void) {
    if (g_trace < 0) { const char *e = getenv("CLOKE_TABLE_TRACE"); g_trace = e ? atoi(e) : 0; }
    return g_trace > 0;
}

enum { BS_Normal = 0, BS_Emphasized = 1 };
static NSString *const kRowKey = @"\x01NSTableRowView";   /* Wiederverwendung eigener Zeilenansichten */

/* ======================================================================================================================
 * NSTableCellView
 * ====================================================================================================================== */
@implementation NSTableCellView {
    id _objectValue;
    NSTextField *_textField;     /* Steckdosen wie bei Apple nicht gehalten (sind Unteransichten) */
    NSImageView *_imageView;
    NSInteger _backgroundStyle, _rowSizeStyle;
}

- (id) initWithCoder: (NSCoder *) coder {
    if (!(self = [super initWithCoder: coder])) return nil;
    _rowSizeStyle = -1;
    if ([coder allowsKeyedCoding] && [coder containsValueForKey: @"NSReuseIdentifierKey"] && ![self identifier])
        [self setIdentifier: [coder decodeObjectForKey: @"NSReuseIdentifierKey"]];
    return self;
}
- (id) initWithFrame: (NSRect) f {
    if (!(self = [super initWithFrame: f])) return nil;
    _rowSizeStyle = -1;
    return self;
}
- (void) dealloc {
    [_objectValue release];
    [super dealloc];
}
- (id) objectValue { return _objectValue; }
- (void) setObjectValue: (id) v {
    if (v == _objectValue) return;
    [self willChangeValueForKey: @"objectValue"];
    [_objectValue release];
    _objectValue = [v retain];
    [self didChangeValueForKey: @"objectValue"];
}
- (NSTextField *) textField { return _textField; }
- (void) setTextField: (NSTextField *) t { _textField = t; }
- (NSImageView *) imageView { return _imageView; }
- (void) setImageView: (NSImageView *) i { _imageView = i; }
- (NSInteger) rowSizeStyle { return _rowSizeStyle; }
- (void) setRowSizeStyle: (NSInteger) s { _rowSizeStyle = s; }
- (NSArray *) draggingImageComponents { return [NSArray array]; }
- (NSBackgroundStyle) backgroundStyle { return (NSBackgroundStyle) _backgroundStyle; }
/* wie bei Apple an die Zellen der Steuerelemente weiterreichen (helle Schrift auf hervorgehobener Auswahl) */
- (void) setBackgroundStyle: (NSBackgroundStyle) s {
    _backgroundStyle = s;
    for (NSView *v in [self subviews])
        if ([v isKindOfClass: [NSControl class]] && [[(NSControl *) v cell] respondsToSelector: @selector(setBackgroundStyle:)]) {
            [[(NSControl *) v cell] setBackgroundStyle: s];
            [v setNeedsDisplay: YES];
        }
}
@end

/* für NSTextFieldCell (patches/…/NSTextFieldCell.m.sed): Schriftfarbe auf hervorgehobenem Grund wie AppKit – Systemfarben werden
 * alternateSelectedControlTextColor (weiß), eigene Farben bleiben */
__attribute__((visibility("hidden"))) NSColor *ClkCellTextColor(NSCell *cell, NSColor *c) {
    if ((NSInteger) [cell backgroundStyle] != BS_Emphasized) return c;
    static NSSet *names;
    if (!names) names = [[NSSet alloc] initWithObjects: @"controlTextColor", @"labelColor", @"textColor", @"secondaryLabelColor",
                                                        @"tertiaryLabelColor", @"selectedControlTextColor", @"selectedTextColor", nil];
    BOOL sys = c == nil;
    @try {
        if (!sys && [[c colorSpaceName] isEqual: NSNamedColorSpace] && [names containsObject: [c colorNameComponent]]) sys = YES;
    } @catch (id e) {}
    for (NSString *n in names) {
        if (sys) break;
        SEL s = NSSelectorFromString(n);
        if ([NSColor respondsToSelector: s] && [c isEqual: [NSColor performSelector: s]]) sys = YES;
    }
    return sys ? [NSColor alternateSelectedControlTextColor] : c;
}

/* ======================================================================================================================
 * NSTableRowView
 * ====================================================================================================================== */
@implementation NSTableRowView {
    BOOL _selected, _emphasized, _groupRowStyle, _floating, _target, _prevSel, _nextSel;
    NSInteger _highlight, _dropStyle;
    CGFloat _dropIndent;
    NSColor *_bg;
}

static void row_init(NSTableRowView *self) {
    self->_emphasized = YES;
    self->_highlight = 0;   /* NSTableViewSelectionHighlightStyleRegular */
}
- (id) initWithFrame: (NSRect) f {
    if (!(self = [super initWithFrame: f])) return nil;
    row_init(self);
    return self;
}
- (id) initWithCoder: (NSCoder *) coder {
    if (!(self = [super initWithCoder: coder])) return nil;
    row_init(self);
    if ([coder allowsKeyedCoding] && [coder containsValueForKey: @"NSReuseIdentifierKey"] && ![self identifier])
        [self setIdentifier: [coder decodeObjectForKey: @"NSReuseIdentifierKey"]];
    return self;
}
- (void) dealloc {
    [_bg release];
    [super dealloc];
}
- (BOOL) isFlipped { return YES; }
- (BOOL) isOpaque { return NO; }

- (NSBackgroundStyle) interiorBackgroundStyle {
    return (_selected && _emphasized && _highlight != -1) ? (NSBackgroundStyle) BS_Emphasized : (NSBackgroundStyle) BS_Normal;
}
- (void) _clkUpdateInterior {
    NSBackgroundStyle s = [self interiorBackgroundStyle];
    for (NSView *v in [self subviews])
        if ([v respondsToSelector: @selector(setBackgroundStyle:)]) [(id) v setBackgroundStyle: s];
}
- (BOOL) isSelected { return _selected; }
- (void) setSelected: (BOOL) f {
    if (f == _selected) return;
    _selected = f;
    [self _clkUpdateInterior];
    [self setNeedsDisplay: YES];
}
- (BOOL) isEmphasized { return _emphasized; }
- (void) setEmphasized: (BOOL) f {
    if (f == _emphasized) return;
    _emphasized = f;
    [self _clkUpdateInterior];
    [self setNeedsDisplay: YES];
}
- (BOOL) isGroupRowStyle { return _groupRowStyle; }
- (void) setGroupRowStyle: (BOOL) f { _groupRowStyle = f; [self setNeedsDisplay: YES]; }
- (BOOL) isFloating { return _floating; }
- (void) setFloating: (BOOL) f { _floating = f; }
- (BOOL) isTargetForDropOperation { return _target; }
- (void) setTargetForDropOperation: (BOOL) f { _target = f; [self setNeedsDisplay: YES]; }
- (BOOL) isPreviousRowSelected { return _prevSel; }
- (void) setPreviousRowSelected: (BOOL) f { _prevSel = f; }
- (BOOL) isNextRowSelected { return _nextSel; }
- (void) setNextRowSelected: (BOOL) f { _nextSel = f; }
- (NSInteger) selectionHighlightStyle { return _highlight; }
- (void) setSelectionHighlightStyle: (NSInteger) s { _highlight = s; [self _clkUpdateInterior]; [self setNeedsDisplay: YES]; }
- (NSInteger) draggingDestinationFeedbackStyle { return _dropStyle; }
- (void) setDraggingDestinationFeedbackStyle: (NSInteger) s { _dropStyle = s; }
- (CGFloat) indentationForDropOperation { return _dropIndent; }
- (void) setIndentationForDropOperation: (CGFloat) i { _dropIndent = i; }
- (NSColor *) backgroundColor { return _bg; }
- (void) setBackgroundColor: (NSColor *) c {
    [_bg autorelease];
    _bg = [c retain];
    [self setNeedsDisplay: YES];
}

/* Zellansichten dieser Zeile: Unteransichten in Spaltenreihenfolge (die Tabelle legt sie so an) */
- (NSInteger) numberOfColumns {
    NSTableView *t = (NSTableView *) [self superview];
    return [t isKindOfClass: [NSTableView class]] ? (NSInteger) [[t tableColumns] count] : 0;
}
- (id) viewAtColumn: (NSInteger) column {
    NSTableView *t = (NSTableView *) [self superview];
    if (![t isKindOfClass: [NSTableView class]]) return nil;
    NSInteger row = [t rowForView: self];
    return row < 0 ? nil : [t viewAtColumn: column row: row makeIfNecessary: NO];
}

- (void) drawBackgroundInRect: (NSRect) r {
    if (_bg && [_bg alphaComponent] > 0) {
        [_bg setFill];
        NSRectFill(r);
    }
}
- (void) drawSelectionInRect: (NSRect) r {
    if (_highlight == -1) return;
    [(_emphasized ? [NSColor selectedControlColor] : [NSColor secondarySelectedControlColor]) setFill];
    NSRectFill([self bounds]);
}
- (void) drawSeparatorInRect: (NSRect) r {}
- (void) drawDraggingDestinationFeedbackInRect: (NSRect) r {
    if (!_target) return;
    [[NSColor selectedControlColor] setStroke];
    NSFrameRectWithWidth(NSInsetRect([self bounds], 1, 1), 2);
}
- (void) drawRect: (NSRect) r {
    [self drawBackgroundInRect: r];
    if (_selected) [self drawSelectionInRect: r];
    [self drawSeparatorInRect: r];
    if (_target) [self drawDraggingDestinationFeedbackInRect: r];
}
@end

/* ======================================================================================================================
 * Zustand je Tabelle
 * ====================================================================================================================== */
@interface CLKTableRow : NSObject {
@public
    NSView *rowView;
    NSMutableArray *cells;    /* je Spalte Ansicht oder NSNull */
    BOOL own;                 /* Zeilenansicht selbst angelegt (nicht vom Delegaten) */
}
@end
@implementation CLKTableRow
- (void) dealloc { [rowView release]; [cells release]; [super dealloc]; }
@end

@interface CLKTableVB : NSObject {
@public
    NSMutableDictionary *nibs;     /* Kennung -> NSNib */
    NSMutableDictionary *reuse;    /* Kennung -> NSMutableArray freier Ansichten */
    NSMutableDictionary *rows;     /* NSNumber Zeile -> CLKTableRow */
    NSArray *columns;              /* Spalten beim letzten Abgleich */
    BOOL fromNib, syncing;
    int updates;
}
@end
@implementation CLKTableVB
- (id) init {
    if (!(self = [super init])) return nil;
    nibs = [NSMutableDictionary new];
    reuse = [NSMutableDictionary new];
    rows = [NSMutableDictionary new];
    return self;
}
- (void) dealloc { [nibs release]; [reuse release]; [rows release]; [columns release]; [super dealloc]; }
@end

static char k_table;
static CLKTableVB *tstate(NSTableView *t, BOOL make) {
    CLKTableVB *s = objc_getAssociatedObject(t, &k_table);
    if (!s && make) {
        s = [[CLKTableVB new] autorelease];
        objc_setAssociatedObject(t, &k_table, s, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    }
    return s;
}

static BOOL is_outline(NSTableView *t) { return [t isKindOfClass: [NSOutlineView class]]; }

__attribute__((visibility("hidden"))) BOOL ClkTableIsViewBased(NSTableView *t) {
    CLKTableVB *s = tstate(t, NO);
    if (s && s->fromNib) return YES;
    id d = [t delegate];
    if (!d) return NO;
    return is_outline(t) ? [d respondsToSelector: @selector(outlineView:viewForTableColumn:item:)]
                         : [d respondsToSelector: @selector(tableView:viewForTableColumn:row:)];
}

/* aus -[NSTableView initWithCoder:]: Vorlagen der Nib (Kennung -> NSNib) */
__attribute__((visibility("hidden"))) void ClkTableDecode(NSTableView *t, NSCoder *c) {
    @try {
        if (![c allowsKeyedCoding] || ![c containsValueForKey: @"NSTableViewArchivedReusableViewsKey"]) return;
        NSDictionary *d = [c decodeObjectForKey: @"NSTableViewArchivedReusableViewsKey"];
        if (![d isKindOfClass: [NSDictionary class]] || ![d count]) return;
        CLKTableVB *s = tstate(t, YES);
        s->fromNib = YES;
        for (id k in d) {
            id nib = [d objectForKey: k];
            if ([nib isKindOfClass: [NSNib class]]) [s->nibs setObject: nib forKey: k];
        }
        if (trace()) fprintf(stderr, "[clokeos-table] %p Vorlagen aus der Nib: %s\n", t, [[[[d allKeys] description]
                                       stringByReplacingOccurrencesOfString: @"\n" withString: @" "] UTF8String]);
    } @catch (id e) {
        NSLog(@"NSTableViewArchivedReusableViewsKey: %@", e);
    }
}

/* ---- Rahmen ---- */
static NSRect row_frame(NSTableView *t, NSInteger row) {
    NSRect r = [t rectOfRow: row];
    r.origin.x = 0;
    r.size.width = MAX(r.size.width, [t bounds].size.width);
    return r;
}
/* Zelle in Koordinaten der Tabelle (wie AppKit ohne Zwischenraum) */
static NSRect cell_frame(NSTableView *t, NSInteger col, NSInteger row, NSRect rowRect) {
    NSArray *cols = [t tableColumns];
    NSSize sp = [t intercellSpacing];
    CGFloat x = 0;
    for (NSInteger i = 0; i < col; i++) x += [[cols objectAtIndex: i] width] + sp.width;
    NSRect f = NSMakeRect(x + floor(sp.width / 2), rowRect.origin.y + floor(sp.height / 2), [[cols objectAtIndex: col] width],
                          rowRect.size.height - sp.height);
    if (is_outline(t) && [cols objectAtIndex: col] == [(NSOutlineView *) t outlineTableColumn]) {
        /* Einzug wie Cocotrons Gliederungsdreieck: Ebene × Einzug, dann das Dreieck (Zeilenhöhe breit) */
        CGFloat off = [(NSOutlineView *) t levelForRow: row] * [(NSOutlineView *) t indentationPerLevel] + [t rowHeight];
        f.origin.x += off;
        f.size.width = MAX(0, f.size.width - off);
    }
    if (f.size.height < 0) f.size.height = 0;
    return f;
}

/* ---- Wiederverwendung ---- */
static void enqueue(CLKTableVB *s, NSString *ident, NSView *v) {
    if (!ident || !v) return;
    NSMutableArray *q = [s->reuse objectForKey: ident];
    if (!q) { q = [NSMutableArray array]; [s->reuse setObject: q forKey: ident]; }
    if ([q count] < 64) [q addObject: v];
}
static id dequeue(CLKTableVB *s, NSString *ident) {
    NSMutableArray *q = [s->reuse objectForKey: ident];
    if (![q count]) return nil;
    id v = [[[q lastObject] retain] autorelease];
    [q removeLastObject];
    if ([v respondsToSelector: @selector(prepareForReuse)]) [v prepareForReuse];
    return v;
}

/* ---- Zeilen anlegen, anordnen, entfernen ---- */
static NSView *ask_cell(NSTableView *t, NSInteger col, NSInteger row) {
    NSTableColumn *c = [[t tableColumns] objectAtIndex: col];
    id d = [t delegate], ds = [t dataSource];
    NSView *v = nil;
    id item = nil;
    if (is_outline(t)) {
        item = [(NSOutlineView *) t itemAtRow: row];
        if ([d respondsToSelector: @selector(outlineView:viewForTableColumn:item:)])
            v = [d outlineView: (NSOutlineView *) t viewForTableColumn: c item: item];
        else
            v = [t makeViewWithIdentifier: [c identifier] owner: d];
        if (v && [v respondsToSelector: @selector(setObjectValue:)] &&
            [ds respondsToSelector: @selector(outlineView:objectValueForTableColumn:byItem:)])
            [(id) v setObjectValue: [ds outlineView: (NSOutlineView *) t objectValueForTableColumn: c byItem: item]];
    } else {
        if ([d respondsToSelector: @selector(tableView:viewForTableColumn:row:)])
            v = [d tableView: t viewForTableColumn: c row: row];
        else
            v = [t makeViewWithIdentifier: [c identifier] owner: d];
        if (v && [v respondsToSelector: @selector(setObjectValue:)] &&
            [ds respondsToSelector: @selector(tableView:objectValueForTableColumn:row:)])
            [(id) v setObjectValue: [t dataSourceObjectValueForTableColumn: c row: row]];
    }
    return [v isKindOfClass: [NSView class]] ? v : nil;
}

static void place_cell(NSTableView *t, CLKTableRow *e, NSInteger col, NSInteger row, NSRect rowRect) {
    id v = [e->cells objectAtIndex: col];
    if (v == [NSNull null]) return;
    NSRect f = cell_frame(t, col, row, rowRect);
    f.origin.y -= rowRect.origin.y;
    if (![e->rowView isFlipped]) f.origin.y = rowRect.size.height - f.origin.y - f.size.height;
    if (!NSEqualRects([v frame], f)) [v setFrame: f];
}

static void update_selected(NSTableView *t, CLKTableRow *e, NSInteger row) {
    if (![e->rowView isKindOfClass: [NSTableRowView class]]) return;
    NSTableRowView *rv = (NSTableRowView *) e->rowView;
    [rv setSelected: [t isRowSelected: row]];
    [rv setPreviousRowSelected: row > 0 && [t isRowSelected: row - 1]];
    [rv setNextRowSelected: [t isRowSelected: row + 1]];
    if (e->own) [rv setSelectionHighlightStyle: [t selectionHighlightStyle]];
    /* hervorgehoben wie bei Apple nur, wenn die Tabelle im Schlüsselfenster den Tastaturfokus hat */
    NSWindow *w = [t window];
    id fr = [w firstResponder];
    [rv setEmphasized: [w isKeyWindow] && [fr isKindOfClass: [NSView class]] && [fr isDescendantOf: t]];
}

static void place_row(NSTableView *t, CLKTableRow *e, NSInteger row) {
    NSRect rr = row_frame(t, row);
    if (!NSEqualRects([e->rowView frame], rr)) [e->rowView setFrame: rr];
    for (NSUInteger c = 0; c < [e->cells count]; c++) place_cell(t, e, c, row, rr);
}

static CLKTableRow *make_row(NSTableView *t, CLKTableVB *s, NSInteger row) {
    id d = [t delegate];
    NSView *rv = nil;
    CLKTableRow *e = [[CLKTableRow new] autorelease];
    NSRect rr = row_frame(t, row);
    if (is_outline(t)) {
        if ([d respondsToSelector: @selector(outlineView:rowViewForItem:)])
            rv = [d outlineView: (NSOutlineView *) t rowViewForItem: [(NSOutlineView *) t itemAtRow: row]];
    } else if ([d respondsToSelector: @selector(tableView:rowViewForRow:)])
        rv = [d tableView: t rowViewForRow: row];
    if (![rv isKindOfClass: [NSView class]]) {
        rv = dequeue(s, kRowKey);
        if (!rv) rv = [[[NSTableRowView alloc] initWithFrame: rr] autorelease];
        e->own = YES;
    }
    e->rowView = [rv retain];
    e->cells = [NSMutableArray new];
    [s->rows setObject: e forKey: [NSNumber numberWithInteger: row]];   /* vor den Fragen: Delegaten dürfen viewAtColumn:… rufen */
    [rv setFrame: rr];
    NSUInteger n = [[t tableColumns] count];
    for (NSUInteger c = 0; c < n; c++) [e->cells addObject: [NSNull null]];
    for (NSUInteger c = 0; c < n; c++) {
        NSView *v = ask_cell(t, c, row);
        if (!v) continue;
        if ([e->cells containsObject: v]) continue;   /* dieselbe Ansicht für zwei Spalten: nur einmal */
        [e->cells replaceObjectAtIndex: c withObject: v];
        if ([v superview] != rv) [v removeFromSuperview];
        [rv addSubview: v];
    }
    place_row(t, e, row);
    [t addSubview: rv];
    update_selected(t, e, row);
    if ([rv isKindOfClass: [NSTableRowView class]]) [(NSTableRowView *) rv _clkUpdateInterior];
    if (is_outline(t)) {
        if ([d respondsToSelector: @selector(outlineView:didAddRowView:forRow:)])
            [d outlineView: (NSOutlineView *) t didAddRowView: (NSTableRowView *) rv forRow: row];
    } else if ([d respondsToSelector: @selector(tableView:didAddRowView:forRow:)])
        [d tableView: t didAddRowView: (NSTableRowView *) rv forRow: row];
    if (trace()) fprintf(stderr, "[clokeos-table] %p Zeile %ld angelegt (%s, %lu Ansichten)\n", t, (long) row,
                         class_getName([rv class]), (unsigned long) [[e->rowView subviews] count]);
    return e;
}

/* row < 0: Zeile wurde gelöscht (der Delegat bekommt -1 wie bei Apple) */
static void drop_entry(NSTableView *t, CLKTableVB *s, CLKTableRow *e, NSInteger row, BOOL recycle) {
    [[e retain] autorelease];
    id d = [t delegate];
    if (is_outline(t)) {
        if ([d respondsToSelector: @selector(outlineView:didRemoveRowView:forRow:)])
            [d outlineView: (NSOutlineView *) t didRemoveRowView: (NSTableRowView *) e->rowView forRow: row];
    } else if ([d respondsToSelector: @selector(tableView:didRemoveRowView:forRow:)])
        [d tableView: t didRemoveRowView: (NSTableRowView *) e->rowView forRow: row];
    for (id v in e->cells) {
        if (v == [NSNull null]) continue;
        [[v retain] autorelease];
        [v removeFromSuperview];
        if (recycle) enqueue(s, [v identifier], v);
    }
    [e->rowView removeFromSuperview];
    if (recycle && e->own) {
        if ([e->rowView isKindOfClass: [NSTableRowView class]]) [(NSTableRowView *) e->rowView setSelected: NO];
        enqueue(s, kRowKey, e->rowView);
    }
    if (trace()) fprintf(stderr, "[clokeos-table] %p Zeile %ld entfernt\n", t, (long) row);
}
static void drop_row(NSTableView *t, CLKTableVB *s, NSInteger row, BOOL recycle) {
    NSNumber *k = [NSNumber numberWithInteger: row];
    CLKTableRow *e = [s->rows objectForKey: k];
    if (!e) return;
    [[e retain] autorelease];
    [s->rows removeObjectForKey: k];
    drop_entry(t, s, e, row, recycle);
}
static void drop_all(NSTableView *t, CLKTableVB *s, BOOL recycle) {
    NSArray *keys = [[s->rows allKeys] sortedArrayUsingSelector: @selector(compare:)];
    for (NSNumber *k in keys) drop_row(t, s, [k integerValue], recycle);
}

/* Spalten hinzugefügt, entfernt oder verschoben: alles neu */
static void check_columns(NSTableView *t, CLKTableVB *s) {
    NSArray *cols = [t tableColumns];
    if (s->columns && ![s->columns isEqualToArray: cols]) drop_all(t, s, YES);
    if (s->columns != cols) {
        [s->columns release];
        s->columns = [cols copy];
    }
}

/* Zeile mit dem Erstantwortenden (z. B. gerade bearbeitetes Textfeld) bleibt, auch wenn sie aus dem Bild scrollt */
static BOOL holds_focus(NSTableView *t, CLKTableRow *e) {
    id fr = [[t window] firstResponder];
    return [fr isKindOfClass: [NSView class]] && [fr isDescendantOf: e->rowView];
}

static NSRange visible_rows(NSTableView *t, NSInteger n) {
    NSRect vr = [t visibleRect];
    if (n <= 0 || NSIsEmptyRect(vr)) return NSMakeRange(0, 0);
    /* erste und letzte angeschnittene Zeile (Cocotrons rowsInRect: nimmt eine Zeile zu viel) */
    NSInteger a = [t rowAtPoint: NSMakePoint(NSMinX(vr), NSMinY(vr))];
    NSInteger b = [t rowAtPoint: NSMakePoint(NSMinX(vr), NSMaxY(vr) - 0.5)];
    if (a < 0) return NSMakeRange(0, 0);
    if (b < 0 || b >= n) b = n - 1;
    return NSMakeRange(a, b - a + 1);
}

static void table_sync(NSTableView *t) {
    CLKTableVB *s = tstate(t, NO);
    if (!ClkTableIsViewBased(t)) {
        if (s && [s->rows count] && !s->syncing) drop_all(t, s, NO);
        return;
    }
    s = tstate(t, YES);
    if (s->syncing || s->updates) return;
    s->syncing = YES;
    @try {
        check_columns(t, s);
        NSInteger n = [t numberOfRows];
        NSRange vis = [t isHidden] ? NSMakeRange(0, 0) : visible_rows(t, n);
        for (NSNumber *k in [[s->rows allKeys] sortedArrayUsingSelector: @selector(compare:)]) {
            NSInteger r = [k integerValue];
            CLKTableRow *e = [s->rows objectForKey: k];
            if (r >= n || (!NSLocationInRange(r, vis) && !holds_focus(t, e))) drop_row(t, s, r, YES);
        }
        for (NSUInteger r = vis.location; r < NSMaxRange(vis); r++) {
            CLKTableRow *e = [s->rows objectForKey: [NSNumber numberWithInteger: r]];
            if (!e) make_row(t, s, r);
            else { place_row(t, e, r); update_selected(t, e, r); }
        }
    } @catch (id x) {
        NSLog(@"NSTableView (ansichtsbasiert): %@", x);
    } @finally {
        s->syncing = NO;
    }
}

static void needs_sync(NSTableView *t) {
    if ([t respondsToSelector: @selector(setNeedsLayout:)]) [t setNeedsLayout: YES];
    [t setNeedsDisplay: YES];
}

/* aus -reloadData: alle Ansichten neu erfragen (die alten gehen in die Wiederverwendung). Wie AppKit (auf dem Mac gemessen) hebt
 * reloadData bei ansichtsbasierten Tabellen die Auswahl auf; ohne leere Auswahl ist danach Zeile 0 gewählt. */
__attribute__((visibility("hidden"))) void ClkTableReset(NSTableView *t) {
    CLKTableVB *s = tstate(t, NO);
    if (s && [s->rows count] && !s->syncing) drop_all(t, s, YES);
    if (!ClkTableIsViewBased(t)) return;
    if ([[t selectedRowIndexes] count]) [t selectRowIndexes: [NSIndexSet indexSet] byExtendingSelection: NO];
    needs_sync(t);
}
/* aus -noteNumberOfRowsChanged: Zeilenzahl/-höhen neu; bei NSOutlineView hängen Zeilen an Elementen, darum dort alles neu */
__attribute__((visibility("hidden"))) void ClkTableRowsChanged(NSTableView *t) {
    CLKTableVB *s = tstate(t, NO);
    if (s && is_outline(t) && [s->rows count] && !s->syncing) drop_all(t, s, YES);
    if (s || ClkTableIsViewBased(t)) needs_sync(t);
}
__attribute__((visibility("hidden"))) void ClkTableNeedsLayout(NSTableView *t) {
    if (ClkTableIsViewBased(t)) needs_sync(t);
}
__attribute__((visibility("hidden"))) void ClkTableSelectionChanged(NSTableView *t) {
    CLKTableVB *s = tstate(t, NO);
    if (!s) return;
    for (NSNumber *k in s->rows) update_selected(t, [s->rows objectForKey: k], [k integerValue]);
}
/* aus -editColumn:row:withEvent:select: – bei Ansichten das Textfeld der Zelle zum Erstantwortenden machen */
__attribute__((visibility("hidden"))) BOOL ClkTableEdit(NSTableView *t, NSInteger col, NSInteger row, BOOL select) {
    if (!ClkTableIsViewBased(t)) return NO;
    NSView *v = [t viewAtColumn: col row: row makeIfNecessary: YES];
    NSTextField *tf = [v isKindOfClass: [NSTableCellView class]] ? [(NSTableCellView *) v textField]
                    : [v isKindOfClass: [NSTextField class]] ? (NSTextField *) v : nil;
    if (tf && [tf isEditable] && [[t window] makeFirstResponder: tf] && select) [tf selectText: nil];
    return YES;
}

/* ======================================================================================================================
 * NSTableView: Schnittstelle
 * ====================================================================================================================== */
static BOOL overrides(Class c, SEL sel) {
    return class_getMethodImplementation(c, sel) != class_getMethodImplementation([NSView class], sel);
}
/* Bekommt die getroffene Ansicht den Klick selbst? Sonst die Tabelle (Zeile wählen), wie bei Apple */
static BOOL takes_click(NSTableView *t, NSView *v) {
    if ([v isKindOfClass: [NSTextField class]]) {
        NSTextField *f = (NSTextField *) v;
        if (![f isEditable] && ![f isSelectable]) return NO;
        NSInteger r = [t rowForView: f];
        return r < 0 || [t isRowSelected: r];
    }
    if ([v isKindOfClass: [NSImageView class]]) return [(NSImageView *) v isEditable];
    if ([v isKindOfClass: [NSControl class]]) return [(NSControl *) v isEnabled];
    if ([v isKindOfClass: [NSTableCellView class]] || [v isKindOfClass: [NSTableRowView class]])
    return overrides([v class], @selector(mouseDown:));
}

@implementation NSTableView (CLKViewBased)

- (void) layout {
    table_sync(self);
    [super layout];
}
- (void) viewWillDraw {
    table_sync(self);
    [super viewWillDraw];
}
- (NSView *) hitTest: (NSPoint) p {
    NSView *h = [super hitTest: p];
    if (!h || h == self || ![h isDescendantOf: self] || !ClkTableIsViewBased(self)) return h;
    return takes_click(self, h) ? h : self;
}

- (id) makeViewWithIdentifier: (NSString *) ident owner: (id) owner {
    if (!ident) return nil;
    CLKTableVB *s = tstate(self, YES);
    id v = dequeue(s, ident);
    if (v) return v;
    NSNib *nib = [s->nibs objectForKey: ident];
    if (!nib) return nil;
    NSMutableArray *top = [NSMutableArray array];
    NSMutableDictionary *tbl = [NSMutableDictionary dictionaryWithObject: top forKey: NSNibTopLevelObjects];
    if (owner) [tbl setObject: owner forKey: NSNibOwner];
    BOOL ok = NO;
    @try { ok = [nib instantiateNibWithExternalNameTable: tbl]; }
    @catch (id e) { NSLog(@"makeViewWithIdentifier:%@: %@", ident, e); }
    if (!ok) return nil;
    for (id o in top) if ([o isKindOfClass: [NSView class]]) { v = o; break; }
    [[v retain] autorelease];
    for (id o in top) [o autorelease];   /* Cocotron hält die Objekte der obersten Ebene einmal zusätzlich (alte Schnittstelle) */
    if (v) [v setIdentifier: ident];
    if (trace()) fprintf(stderr, "[clokeos-table] %p Nib-Ansicht %s: %s\n", self, [ident UTF8String], v ? class_getName([v class]) : "keine");
    return v;
}

- (void) registerNib: (NSNib *) nib forIdentifier: (NSString *) ident {
    if (!ident) return;
    CLKTableVB *s = tstate(self, YES);
    if (nib) [s->nibs setObject: nib forKey: ident];
    else [s->nibs removeObjectForKey: ident];
    [s->reuse removeObjectForKey: ident];
}
- (NSDictionary *) registeredNibsByIdentifier {
    CLKTableVB *s = tstate(self, NO);
    return s ? [[s->nibs copy] autorelease] : [NSDictionary dictionary];
}

- (id) rowViewAtRow: (NSInteger) row makeIfNecessary: (BOOL) make {
    if (row < 0 || row >= [self numberOfRows] || !ClkTableIsViewBased(self)) return nil;
    CLKTableVB *s = tstate(self, YES);
    check_columns(self, s);
    CLKTableRow *e = [s->rows objectForKey: [NSNumber numberWithInteger: row]];
    if (!e && make && !s->updates) {
        BOOL was = s->syncing;
        s->syncing = YES;
        @try { e = make_row(self, s, row); } @finally { s->syncing = was; }
    }
    return e ? e->rowView : nil;
}
- (id) viewAtColumn: (NSInteger) col row: (NSInteger) row makeIfNecessary: (BOOL) make {
    if (col < 0 || col >= (NSInteger) [[self tableColumns] count]) return nil;
    if (![self rowViewAtRow: row makeIfNecessary: make]) return nil;
    CLKTableRow *e = [tstate(self, NO)->rows objectForKey: [NSNumber numberWithInteger: row]];
    if ((NSUInteger) col >= [e->cells count]) return nil;
    id v = [e->cells objectAtIndex: col];
    return v == [NSNull null] ? nil : v;
}

- (NSInteger) rowForView: (NSView *) view {
    CLKTableVB *s = tstate(self, NO);
    if (!s) return -1;
    for (NSView *v = view; v; v = [v superview]) {
        if ([v superview] != self) continue;
        for (NSNumber *k in s->rows)
            if (((CLKTableRow *) [s->rows objectForKey: k])->rowView == v) return [k integerValue];
        return -1;
    }
    return -1;
}
- (NSInteger) columnForView: (NSView *) view {
    NSInteger row = [self rowForView: view];
    if (row < 0) return -1;
    CLKTableRow *e = [tstate(self, NO)->rows objectForKey: [NSNumber numberWithInteger: row]];
    for (NSUInteger c = 0; c < [e->cells count]; c++) {
        id cv = [e->cells objectAtIndex: c];
        if (cv != [NSNull null] && [view isDescendantOf: cv]) return c;
    }
    return -1;
}

- (void) enumerateAvailableRowViewsUsingBlock: (void (^)(NSTableRowView *rowView, NSInteger row)) block {
    CLKTableVB *s = tstate(self, NO);
    if (!s || !block) return;
    for (NSNumber *k in [[s->rows allKeys] sortedArrayUsingSelector: @selector(compare:)]) {
        CLKTableRow *e = [s->rows objectForKey: k];
        if (e) block((NSTableRowView *) e->rowView, [k integerValue]);
    }
}

- (void) beginUpdates { tstate(self, YES)->updates++; }
- (void) endUpdates {
    CLKTableVB *s = tstate(self, YES);
    if (s->updates > 0 && --s->updates == 0) needs_sync(self);
}

/* Zeilennummern der vorhandenen Ansichten und die Auswahl verschieben */
static void remap_rows(NSTableView *t, CLKTableVB *s, NSInteger (^map)(NSInteger)) {
    NSMutableDictionary *nr = [NSMutableDictionary dictionary];
    for (NSNumber *k in s->rows) {
        NSInteger r = map([k integerValue]);
        if (r >= 0) [nr setObject: [s->rows objectForKey: k] forKey: [NSNumber numberWithInteger: r]];
    }
    [s->rows setDictionary: nr];
}
static NSIndexSet *remap_set(NSIndexSet *in, NSInteger (^map)(NSInteger)) {
    NSMutableIndexSet *o = [NSMutableIndexSet indexSet];
    for (NSUInteger i = [in firstIndex]; i != NSNotFound; i = [in indexGreaterThanIndex: i]) {
        NSInteger r = map(i);
        if (r >= 0) [o addIndex: r];
    }
    return o;
}
- (void) _clkRemap: (NSInteger (^)(NSInteger)) map {
    CLKTableVB *s = tstate(self, NO);
    if (s) remap_rows(self, s, map);
    NSIndexSet *sel = remap_set(_selectedRowIndexes, map);
    if (![sel isEqualToIndexSet: _selectedRowIndexes]) {
        [_selectedRowIndexes release];
        _selectedRowIndexes = [sel copy];
    }
}

- (void) insertRowsAtIndexes: (NSIndexSet *) idx withAnimation: (NSUInteger) opts {
    if (![idx count]) return;
    [self _clkRemap: ^NSInteger (NSInteger r) {
        for (NSUInteger i = [idx firstIndex]; i != NSNotFound; i = [idx indexGreaterThanIndex: i])
            if ((NSInteger) i <= r) r++;
        return r;
    }];
    [self noteNumberOfRowsChanged];
    ClkTableSelectionChanged(self);
    needs_sync(self);
}
- (void) removeRowsAtIndexes: (NSIndexSet *) idx withAnimation: (NSUInteger) opts {
    if (![idx count]) return;
    CLKTableVB *s = tstate(self, NO);
    if (s) {
        BOOL was = s->syncing;
        s->syncing = YES;
        for (NSUInteger i = [idx firstIndex]; i != NSNotFound; i = [idx indexGreaterThanIndex: i]) {
            NSNumber *k = [NSNumber numberWithInteger: i];
            CLKTableRow *e = [s->rows objectForKey: k];
            if (!e) continue;
            [[e retain] autorelease];
            [s->rows removeObjectForKey: k];
            drop_entry(self, s, e, -1, YES);
        }
        s->syncing = was;
    }
    [self _clkRemap: ^NSInteger (NSInteger r) {
        if ([idx containsIndex: r]) return -1;
        return r - (NSInteger) [idx countOfIndexesInRange: NSMakeRange(0, r)];
    }];
    [self noteNumberOfRowsChanged];
    ClkTableSelectionChanged(self);
    needs_sync(self);
}
- (void) moveRowAtIndex: (NSInteger) from toIndex: (NSInteger) to {
    if (from == to) return;
    [self _clkRemap: ^NSInteger (NSInteger r) {
        if (r == from) return to;
        if (from < to && r > from && r <= to) return r - 1;
        if (to < from && r >= to && r < from) return r + 1;
        return r;
    }];
    ClkTableSelectionChanged(self);
    needs_sync(self);
}
- (void) reloadDataForRowIndexes: (NSIndexSet *) rows columnIndexes: (NSIndexSet *) cols {
    CLKTableVB *s = tstate(self, NO);
    if (s && ClkTableIsViewBased(self) && !s->syncing) {
        s->syncing = YES;
        @try {
            for (NSUInteger r = [rows firstIndex]; r != NSNotFound; r = [rows indexGreaterThanIndex: r]) {
                CLKTableRow *e = [s->rows objectForKey: [NSNumber numberWithInteger: r]];
                if (!e) continue;
                NSRect rr = row_frame(self, r);
                for (NSUInteger c = [cols firstIndex]; c != NSNotFound && c < [e->cells count]; c = [cols indexGreaterThanIndex: c]) {
                    id old = [[[e->cells objectAtIndex: c] retain] autorelease];
                    if (old != [NSNull null]) {
                        [old removeFromSuperview];
                        enqueue(s, [old identifier], old);
                    }
                    [e->cells replaceObjectAtIndex: c withObject: [NSNull null]];
                    NSView *v = ask_cell(self, c, r);
                    if (v && ![e->cells containsObject: v]) {
                        [e->cells replaceObjectAtIndex: c withObject: v];
                        [e->rowView addSubview: v];
                        place_cell(self, e, c, r, rr);
                    }
                }
                if ([e->rowView isKindOfClass: [NSTableRowView class]]) [(NSTableRowView *) e->rowView _clkUpdateInterior];
            }
        } @finally {
            s->syncing = NO;
        }
    }
    [self setNeedsDisplay: YES];
}

- (NSInteger) rowSizeStyle {
    NSNumber *n = objc_getAssociatedObject(self, @selector(rowSizeStyle));
    return n ? [n integerValue] : -1;
}
- (void) setRowSizeStyle: (NSInteger) s {
    objc_setAssociatedObject(self, @selector(rowSizeStyle), [NSNumber numberWithInteger: s], OBJC_ASSOCIATION_RETAIN_NONATOMIC);
}
- (NSInteger) effectiveRowSizeStyle {
    NSInteger s = [self rowSizeStyle];
    return s == -1 ? 2 : s;   /* Vorgabe: NSTableViewRowSizeStyleMedium */
}
- (BOOL) usesAutomaticRowHeights { return NO; }
- (void) setUsesAutomaticRowHeights: (BOOL) f {}
- (BOOL) floatsGroupRows { return NO; }
- (void) setFloatsGroupRows: (BOOL) f {}
@end

/* NSOutlineView: Elemente einfügen/entfernen/verschieben = neu laden (die Zeilen hängen an Elementen) */
@implementation NSOutlineView (CLKViewBased)
- (void) insertItemsAtIndexes: (NSIndexSet *) i inParent: (id) p withAnimation: (NSUInteger) o { [self reloadData]; }
- (void) removeItemsAtIndexes: (NSIndexSet *) i inParent: (id) p withAnimation: (NSUInteger) o { [self reloadData]; }
- (void) moveItemAtIndex: (NSInteger) f inParent: (id) p toIndex: (NSInteger) t inParent: (id) q { [self reloadData]; }
- (void) insertRowsAtIndexes: (NSIndexSet *) idx withAnimation: (NSUInteger) opts { [self reloadData]; }
- (void) removeRowsAtIndexes: (NSIndexSet *) idx withAnimation: (NSUInteger) opts { [self reloadData]; }
@end

/* ======================================================================================================================
 * NSNib, NSView, NSClipView
 * ====================================================================================================================== */
/* -[NSNib instantiateWithOwner:topLevelObjects:] (10.8): die Objekte der obersten Ebene gehören dem Aufrufer (kein zusätzliches Halten) */
static BOOL nib_instantiate(NSNib *self, SEL c, id owner, NSArray **objs) {
    NSMutableArray *top = [NSMutableArray array];
    NSMutableDictionary *tbl = [NSMutableDictionary dictionaryWithObject: top forKey: NSNibTopLevelObjects];
    if (owner) [tbl setObject: owner forKey: NSNibOwner];
    BOOL ok = [self instantiateNibWithExternalNameTable: tbl];
    if (ok) for (id o in top) [o autorelease];
    if (objs) *objs = ok ? [NSArray arrayWithArray: top] : nil;
    return ok;
}

static void view_prepare(id self, SEL c) {}

static IMP o_scroll;
static void h_scroll(NSClipView *self, SEL c, NSPoint p) {
    ((void (*)(id, SEL, NSPoint)) o_scroll)(self, c, p);
    id doc = [self documentView];
    if ([doc isKindOfClass: [NSTableView class]] && tstate(doc, NO)) needs_sync(doc);
}

__attribute__((constructor)) static void clk_tableviews_init(void) {
    class_addMethod([NSNib class], @selector(instantiateWithOwner:topLevelObjects:), (IMP) nib_instantiate, "c@:@^@");
    class_addMethod([NSView class], @selector(prepareForReuse), (IMP) view_prepare, "v@:");
    Method m = class_getInstanceMethod([NSClipView class], @selector(scrollToPoint:));
    if (m) {
        IMP old = method_getImplementation(m);
        if (class_addMethod([NSClipView class], @selector(scrollToPoint:), (IMP) h_scroll, method_getTypeEncoding(m))) o_scroll = old;
        else o_scroll = method_setImplementation(m, (IMP) h_scroll);
    }
}
