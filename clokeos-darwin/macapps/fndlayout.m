/* NSLayoutConstraint und die Anker (NSLayoutAnchor, NSLayoutXAxisAnchor, NSLayoutYAxisAnchor, NSLayoutDimension) für ClokeOS.
 *
 * Darling legt diese Klassen in Foundation an (AppKit re-exportiert sie), aber nur als Platzhalter, die jede Nachricht verschlucken.
 * Hier stehen die Daten einer Bedingung: Elemente, Attribute, Beziehung, Faktor, Konstante, Priorität, aktiv, Kennung; Lesen und
 * Schreiben aus Nibs (Schlüssel wie bei Apple: NSFirstItem, NSFirstAttribute(V2), NSRelation, NSMultiplier, NSConstant(V2),
 * NSPriority, NSSymbolicConstant …), die Fabrikmethoden, activate/deactivate und die Visual Format Language.
 * Gelöst und angewandt werden die Bedingungen in AppKit (macapps/appkit/autolayout.m); Foundation kennt keine Ansichten und spricht
 * sie nur über Nachrichten an (superview, owningView, addConstraint:, removeConstraint:, _clkConstraintChanged:).
 *
 * "Standardabstand" (NSSpace in Nibs, "-" in der Formatsprache) bleibt symbolisch (-_clkStandardSpacing); AppKit setzt ihn je
 * Ansichtspaar ein wie macOS (20 zum Rand, 8 zwischen Geschwistern, Ausnahmen in autolayout.m). */
#import <Foundation/Foundation.h>
#import <Foundation/NSLayoutConstraint.h>
#import <Foundation/NSLayoutAnchor.h>
#import <objc/runtime.h>
#import <objc/message.h>

/* Werte wie in Apples NSLayoutConstraint.h */
enum {
    LA_NotAnAttribute = 0, LA_Left, LA_Right, LA_Top, LA_Bottom, LA_Leading, LA_Trailing, LA_Width, LA_Height, LA_CenterX,
    LA_CenterY, LA_LastBaseline, LA_FirstBaseline
};
enum { LR_LE = -1, LR_EQ = 0, LR_GE = 1 };
enum {
    LF_AlignAllMask = 0xFFFF, LF_DirectionLeftToRight = 1 << 16, LF_DirectionRightToLeft = 2 << 16,
    LF_DirectionMask = 3 << 16
};

static id msg(id o, SEL s) { return o && [o respondsToSelector: s] ? ((id (*)(id, SEL)) objc_msgSend)(o, s) : nil; }

/* Ansicht eines Elements (Ansicht selbst, bei Layout-Hilfslinien die besitzende Ansicht) */
static id item_view(id item) {
    if (!item) return nil;
    if ([item respondsToSelector: @selector(owningView)] && ![item respondsToSelector: @selector(superview)]) return msg(item, @selector(owningView));
    return item;
}
static id common_ancestor(id a, id b) {
    if (!b) return a;
    if (!a) return b;
    for (id x = a; x; x = msg(x, @selector(superview)))
        for (id y = b; y; y = msg(y, @selector(superview)))
            if (x == y) return x;
    return nil;
}

@interface NSLayoutConstraint (CLKPrivate)
- (void) _clkSetStandardSpacing: (BOOL) f;
- (void) _clkSetSpacingFactor: (double) m;
@end

@interface NSLayoutAnchor (CLKPrivate)
- (NSLayoutConstraint *) _c: (NSLayoutAnchor *) o rel: (NSInteger) rel m: (double) m k: (double) k;
@end

@interface NSLayoutConstraint () {
    id _first, _second;           /* schwach (objc_storeWeak) */
    id _container;                /* Ansicht, an der die Bedingung hängt (schwach) */
    NSInteger _fa, _sa, _rel;
    double _mult, _const, _spfac;
    float _prio;
    BOOL _active, _archived, _standard;
    NSString *_ident;
}
@end

@implementation NSLayoutConstraint

+ (instancetype) constraintWithItem: (id) v1 attribute: (NSInteger) a1 relatedBy: (NSInteger) rel toItem: (id) v2
                          attribute: (NSInteger) a2 multiplier: (double) m constant: (double) c {
    NSLayoutConstraint *k = [[[self alloc] init] autorelease];
    objc_storeWeak(&k->_first, v1);
    objc_storeWeak(&k->_second, v2);
    k->_fa = a1; k->_sa = v2 ? a2 : LA_NotAnAttribute; k->_rel = rel; k->_mult = m; k->_const = c;
    return k;
}

- (instancetype) init {
    if ((self = [super init])) { _mult = 1; _spfac = 1; _prio = 1000; _archived = YES; }
    return self;
}

- (void) dealloc {
    objc_storeWeak(&_first, nil);
    objc_storeWeak(&_second, nil);
    objc_storeWeak(&_container, nil);
    [_ident release];
    [super dealloc];
}

- (id) firstItem { return objc_loadWeak(&_first); }
- (id) secondItem { return objc_loadWeak(&_second); }
- (NSInteger) firstAttribute { return _fa; }
- (NSInteger) secondAttribute { return _sa; }
- (NSInteger) relation { return _rel; }
- (double) multiplier { return _mult; }
- (double) constant { return _const; }
- (float) priority { return _prio; }
- (NSString *) identifier { return _ident; }
- (void) setIdentifier: (NSString *) s { [_ident autorelease]; _ident = [s copy]; }
- (BOOL) shouldBeArchived { return _archived; }
- (void) setShouldBeArchived: (BOOL) f { _archived = f; }
- (BOOL) isActive { return _active; }

/* AppKit fragt den symbolischen Standardabstand ab und setzt ihn je Paar ein */
- (BOOL) _clkStandardSpacing { return _standard; }
- (void) _clkSetStandardSpacing: (BOOL) f { _standard = f; }
- (double) _clkSpacingFactor { return _spfac; }
- (void) _clkSetSpacingFactor: (double) m { _spfac = m; }
- (id) _clkContainer { return objc_loadWeak(&_container); }
/* von -[NSView addConstraint:]/-removeConstraint: gesetzt */
- (void) _clkSetContainer: (id) v { objc_storeWeak(&_container, v); _active = v != nil; }

- (void) _clkChanged {
    id c = objc_loadWeak(&_container);
    if (c && [c respondsToSelector: @selector(_clkConstraintChanged:)]) [c performSelector: @selector(_clkConstraintChanged:) withObject: self];
}
- (void) setConstant: (double) c { if (c != _const) { _const = c; _standard = NO; [self _clkChanged]; } }
- (void) setPriority: (float) p {
    if (p != _prio) { _prio = p; [self _clkChanged]; }
}

- (void) setActive: (BOOL) f {
    if (f == _active) return;
    if (f) {
        id v = common_ancestor(item_view([self firstItem]), item_view([self secondItem]));
        if (!v) {
            NSLog(@"NSLayoutConstraint: Elemente ohne gemeinsamen Vorfahren, nicht aktiviert: %@", self);
            return;
        }
        [v performSelector: @selector(addConstraint:) withObject: self];
    } else {
        id v = objc_loadWeak(&_container);
        if (v) [v performSelector: @selector(removeConstraint:) withObject: self];
        else _active = NO;
    }
}

+ (void) activateConstraints: (NSArray *) cs { for (NSLayoutConstraint *c in cs) [c setActive: YES]; }
+ (void) deactivateConstraints: (NSArray *) cs { for (NSLayoutConstraint *c in cs) [c setActive: NO]; }

static id anchor_of(id item, NSInteger a) {
    SEL s = @selector(_clkAnchorForAttribute:);
    return item && [item respondsToSelector: s] ? ((id (*)(id, SEL, NSInteger)) objc_msgSend)(item, s, a) : nil;
}
- (id) firstAnchor { return anchor_of([self firstItem], _fa); }
- (id) secondAnchor { return anchor_of([self secondItem], _sa); }

/* ---- Archiv ---- */
- (id) initWithCoder: (NSCoder *) c {
    if (!(self = [self init])) return nil;
    if (![c allowsKeyedCoding]) return self;
    objc_storeWeak(&_first, [c decodeObjectForKey: @"NSFirstItem"]);
    objc_storeWeak(&_second, [c decodeObjectForKey: @"NSSecondItem"]);
    _fa = [c containsValueForKey: @"NSFirstAttributeV2"] ? [c decodeIntegerForKey: @"NSFirstAttributeV2"] : [c decodeIntegerForKey: @"NSFirstAttribute"];
    _sa = [c containsValueForKey: @"NSSecondAttributeV2"] ? [c decodeIntegerForKey: @"NSSecondAttributeV2"] : [c decodeIntegerForKey: @"NSSecondAttribute"];
    _rel = [c decodeIntegerForKey: @"NSRelation"];
    if ([c containsValueForKey: @"NSMultiplier"]) _mult = [c decodeDoubleForKey: @"NSMultiplier"];
    _const = [c containsValueForKey: @"NSConstantV2"] ? [c decodeDoubleForKey: @"NSConstantV2"] : [c decodeDoubleForKey: @"NSConstant"];
    if ([c containsValueForKey: @"NSPriority"]) _prio = [c decodeIntegerForKey: @"NSPriority"];
    id sym = [c decodeObjectForKey: @"NSSymbolicConstant"];
    if ([sym isKindOfClass: [NSString class]] && [sym isEqualToString: @"NSSpace"]) _standard = YES;
    if ([c containsValueForKey: @"NSShouldBeArchived"]) _archived = [c decodeBoolForKey: @"NSShouldBeArchived"];
    _ident = [[c decodeObjectForKey: @"NSLayoutIdentifier"] copy];
    if (!_ident) _ident = [[c decodeObjectForKey: @"NSIdentifier"] copy];
    if (![self secondItem]) _sa = LA_NotAnAttribute;
    return self;
}

- (void) encodeWithCoder: (NSCoder *) c {
    if (![c allowsKeyedCoding]) return;
    [c encodeConditionalObject: [self firstItem] forKey: @"NSFirstItem"];
    [c encodeInteger: _fa forKey: @"NSFirstAttribute"];
    if ([self secondItem]) {
        [c encodeConditionalObject: [self secondItem] forKey: @"NSSecondItem"];
        [c encodeInteger: _sa forKey: @"NSSecondAttribute"];
    }
    if (_rel) [c encodeInteger: _rel forKey: @"NSRelation"];
    if (_mult != 1) [c encodeDouble: _mult forKey: @"NSMultiplier"];
    [c encodeDouble: _const forKey: @"NSConstant"];
    if (_prio != 1000) [c encodeInteger: (NSInteger) _prio forKey: @"NSPriority"];
    if (_standard) [c encodeObject: @"NSSpace" forKey: @"NSSymbolicConstant"];
    if (_ident) [c encodeObject: _ident forKey: @"NSLayoutIdentifier"];
}

/* ---- Beschreibung wie bei Apple: <NSLayoutConstraint:0x… NSButton:0x….width == 100   (active)> ---- */
static NSString *attr_name(NSInteger a) {
    static NSString *const n[] = { @"notAnAttribute", @"left", @"right", @"top", @"bottom", @"leading", @"trailing", @"width",
                                   @"height", @"centerX", @"centerY", @"lastBaseline", @"firstBaseline" };
    return a >= 0 && a <= LA_FirstBaseline ? n[a] : [NSString stringWithFormat: @"attr%ld", (long) a];
}
- (NSString *) description {
    id a = [self firstItem], b = [self secondItem];
    NSMutableString *s = [NSMutableString stringWithFormat: @"<%@:%p %@:%p.%@ %@", [self class], self, a ? [a class] : nil, a,
                                                            attr_name(_fa), _rel == LR_LE ? @"<=" : _rel == LR_GE ? @">=" : @"=="];
    if (b) {
        [s appendFormat: @" %@:%p.%@", [b class], b, attr_name(_sa)];
        if (_mult != 1) [s appendFormat: @"*%g", _mult];
        if (_standard) [s appendString: @" + NSSpace"];
        else if (_const) [s appendFormat: @" %c %g", _const < 0 ? '-' : '+', fabs(_const)];
    } else [s appendFormat: @" %g", _const];
    if (_prio != 1000) [s appendFormat: @" @%g", _prio];
    if (_active) [s appendString: @"   (active)"];
    [s appendString: @">"];
    return s;
}

/* ---- Visual Format Language ----
 *   [H:|V:] [|] (Verbindung) [ansicht(prädikate)] (Verbindung [ansicht]) … [Verbindung |]
 *   Verbindung: "" (Abstand 0), "-" (Standardabstand), "-zahl-", "-(prädikate)-"
 *   Prädikat: [== | <= | >=] (zahl | metrik | ansicht) [@ priorität]   (mehrere mit Komma) */
typedef struct {
    NSInteger rel;
    double constant;
    id view;      /* Prädikat gegen eine andere Ansicht */
    float prio;
} vfl_pred;

static void vfl_fail(NSString *fmt, NSUInteger pos, NSString *why) {
    [NSException raise: NSInvalidArgumentException format: @"Unable to parse constraint format: %@\n%@ (Position %lu)", why, fmt,
                                                           (unsigned long) pos];
}

typedef struct {
    NSString *fmt;
    NSUInteger i, n;
    NSDictionary *metrics, *views;
} vfl;

static unichar vc(vfl *p) { return p->i < p->n ? [p->fmt characterAtIndex: p->i] : 0; }
static void vws(vfl *p) { while (vc(p) == ' ') p->i++; }
static BOOL vfl_ident_char(unichar c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; }

static NSString *vfl_ident(vfl *p) {
    NSUInteger s = p->i;
    while (vfl_ident_char(vc(p))) p->i++;
    return p->i > s ? [p->fmt substringWithRange: NSMakeRange(s, p->i - s)] : nil;
}
static BOOL vfl_number(vfl *p, double *out) {
    vws(p);
    NSUInteger s = p->i;
    if (vc(p) == '-' || vc(p) == '+') p->i++;
    BOOL digits = NO;
    while ((vc(p) >= '0' && vc(p) <= '9') || vc(p) == '.') { p->i++; digits = YES; }
    if (!digits) { p->i = s; return NO; }
    *out = [[p->fmt substringWithRange: NSMakeRange(s, p->i - s)] doubleValue];
    return YES;
}
/* Zahl oder Metrikname */
static BOOL vfl_value(vfl *p, double *out, id *view) {
    if (vfl_number(p, out)) return YES;
    vws(p);
    NSUInteger s = p->i;
    NSString *name = vfl_ident(p);
    if (!name) return NO;
    id m = [p->metrics objectForKey: name];
    if (m) { *out = [m doubleValue]; return YES; }
    id v = [p->views objectForKey: name];
    if (v && view) { *view = v; *out = 0; return YES; }
    p->i = s;
    vfl_fail(p->fmt, s, [NSString stringWithFormat: @"%@ is not a key in the views or metrics dictionary", name]);
    return NO;
}
static vfl_pred vfl_one_pred(vfl *p, BOOL allowView) {
    vfl_pred r = { LR_EQ, 0, nil, 1000 };
    vws(p);
    if (p->i + 1 < p->n) {
        NSString *two = [p->fmt substringWithRange: NSMakeRange(p->i, 2)];
        if ([two isEqualToString: @"=="]) { r.rel = LR_EQ; p->i += 2; }
        else if ([two isEqualToString: @"<="]) { r.rel = LR_LE; p->i += 2; }
        else if ([two isEqualToString: @">="]) { r.rel = LR_GE; p->i += 2; }
    }
    id v = nil;
    if (!vfl_value(p, &r.constant, allowView ? &v : NULL)) vfl_fail(p->fmt, p->i, @"Expected a number, metric or view");
    r.view = v;
    vws(p);
    if (vc(p) == '@') {
        p->i++;
        double pr = 1000;
        if (!vfl_value(p, &pr, NULL)) vfl_fail(p->fmt, p->i, @"Expected a priority");
        r.prio = pr;
    }
    return r;
}
/* "(p1,p2,…)" oder eine einzelne Zahl/Metrik; n = Anzahl */
static int vfl_preds(vfl *p, vfl_pred *out, int max, BOOL allowView) {
    vws(p);
    int n = 0;
    if (vc(p) == '(') {
        p->i++;
        for (;;) {
            if (n < max) out[n++] = vfl_one_pred(p, allowView);
            vws(p);
            if (vc(p) == ',') { p->i++; continue; }
            if (vc(p) == ')') { p->i++; break; }
            vfl_fail(p->fmt, p->i, @"Expected ')' or ','");
        }
    } else {
        out[0] = vfl_one_pred(p, allowView);
        n = 1;
    }
    return n;
}

static NSLayoutConstraint *mk(id a, NSInteger aa, NSInteger rel, id b, NSInteger ba, double c, float prio) {
    NSLayoutConstraint *k = [NSLayoutConstraint constraintWithItem: a attribute: aa relatedBy: rel toItem: b attribute: ba
                                                        multiplier: 1 constant: c];
    k->_prio = prio;
    return k;
}

+ (NSArray *) constraintsWithVisualFormat: (NSString *) fmt options: (NSUInteger) opts metrics: (NSDictionary *) metrics
                                    views: (NSDictionary *) views {
    vfl P = { fmt, 0, [fmt length], metrics, views }, *p = &P;
    NSMutableArray *out = [NSMutableArray array];
    BOOL vert = NO;
    vws(p);
    if (p->n >= 2 && ([fmt hasPrefix: @"H:"] || [fmt hasPrefix: @"V:"])) { vert = [fmt characterAtIndex: 0] == 'V'; p->i = 2; }
    BOOL ltr = (opts & LF_DirectionMask) == LF_DirectionLeftToRight;
    NSInteger lead = vert ? LA_Top : ltr ? LA_Left : LA_Leading, trail = vert ? LA_Bottom : ltr ? LA_Right : LA_Trailing;
    NSInteger size = vert ? LA_Height : LA_Width;
    NSMutableArray *chain = [NSMutableArray array];
    id prev = nil;               /* vorige Ansicht, oder NSNull für den Rand am Anfang */
    BOOL havePrev = NO, prevIsEdge = NO;
    vfl_pred conn[8]; int nconn = -1;   /* -1 = noch keine Verbindung; 0 = direkt anliegend */
    BOOL connStd = NO;
    vws(p);
    if (vc(p) == '|') { p->i++; havePrev = YES; prevIsEdge = YES; }
    for (;;) {
        vws(p);
        unichar c = vc(p);
        /* Verbindung */
        nconn = 0; connStd = NO;
        if (c == '-') {
            p->i++;
            vws(p);
            if (vc(p) == '[' || vc(p) == '|') connStd = YES;
            else {
                nconn = vfl_preds(p, conn, 8, NO);
                vws(p);
                if (vc(p) != '-') vfl_fail(fmt, p->i, @"Expected '-'");
                p->i++;
            }
            vws(p);
            c = vc(p);
        }
        if (c == '|') {
            p->i++;
            if (!havePrev || prevIsEdge) vfl_fail(fmt, p->i, @"Edge after edge");
            id sv = msg(prev, @selector(superview));
            if (!sv) vfl_fail(fmt, p->i, @"Unable to interpret '|' character, because the related view doesn't have a superview");
            if (connStd) {
                NSLayoutConstraint *k = mk(sv, trail, LR_EQ, prev, trail, 0, 1000);
                k->_standard = YES;
                [out addObject: k];
            } else if (!nconn) [out addObject: mk(sv, trail, LR_EQ, prev, trail, 0, 1000)];
            else for (int i = 0; i < nconn; i++) [out addObject: mk(sv, trail, conn[i].rel, prev, trail, conn[i].constant, conn[i].prio)];
            break;
        }
        if (c == 0) {
            if (connStd || nconn) vfl_fail(fmt, p->i, @"Expected a view or '|'");
            break;
        }
        if (c != '[') vfl_fail(fmt, p->i, @"Expected a view");
        p->i++;
        vws(p);
        NSString *name = vfl_ident(p);
        id v = name ? [views objectForKey: name] : nil;
        if (!v) vfl_fail(fmt, p->i, [NSString stringWithFormat: @"%@ is not a key in the views dictionary", name]);
        vws(p);
        if (vc(p) == '(') {
            vfl_pred sp[8];
            int ns = vfl_preds(p, sp, 8, YES);
            for (int i = 0; i < ns; i++) {
                if (sp[i].view) [out addObject: mk(v, size, sp[i].rel, sp[i].view, size, 0, sp[i].prio)];
                else [out addObject: mk(v, size, sp[i].rel, nil, LA_NotAnAttribute, sp[i].constant, sp[i].prio)];
            }
            vws(p);
        }
        if (vc(p) != ']') vfl_fail(fmt, p->i, @"Expected ']'");
        p->i++;
        /* Verbindung zur vorigen Ansicht bzw. zum Rand */
        if (havePrev) {
            id a = prevIsEdge ? msg(v, @selector(superview)) : prev;
            if (!a) vfl_fail(fmt, p->i, @"Unable to interpret '|' character, because the related view doesn't have a superview");
            NSInteger aa = prevIsEdge ? lead : trail;
            if (connStd) {
                NSLayoutConstraint *k = mk(v, lead, LR_EQ, a, aa, 0, 1000);
                k->_standard = YES;
                [out addObject: k];
            } else if (!nconn) [out addObject: mk(v, lead, LR_EQ, a, aa, 0, 1000)];
            else for (int i = 0; i < nconn; i++) [out addObject: mk(v, lead, conn[i].rel, a, aa, conn[i].constant, conn[i].prio)];
        }
        [chain addObject: v];
        prev = v; havePrev = YES; prevIsEdge = NO;
    }
    /* Ausrichtung aller Ansichten an der ersten (Optionen NSLayoutFormatAlignAll…) */
    NSUInteger align = opts & LF_AlignAllMask;
    for (NSInteger a = 1; a <= 16 && [chain count] > 1; a++) {
        if (!(align & (1u << a))) continue;
        for (NSUInteger i = 1; i < [chain count]; i++)
            [out addObject: mk([chain objectAtIndex: i], a, LR_EQ, [chain objectAtIndex: 0], a, 0, 1000)];
    }
    return out;
}

@end

/* ---- Anker ---- */
@interface NSLayoutAnchor () {
    id _item;   /* schwach */
    NSInteger _attr;
}
@end

@interface NSLayoutXAxisAnchor : NSLayoutAnchor
@end
@interface NSLayoutYAxisAnchor : NSLayoutAnchor
@end

@implementation NSLayoutAnchor

/* von AppKit benutzt: -[NSView leadingAnchor] usw. */
+ (instancetype) _clkAnchorWithItem: (id) item attribute: (NSInteger) a {
    Class k = a == LA_Width || a == LA_Height ? [NSLayoutDimension class]
            : (a == LA_Top || a == LA_Bottom || a == LA_CenterY || a == LA_FirstBaseline || a == LA_LastBaseline) ? [NSLayoutYAxisAnchor class]
            : [NSLayoutXAxisAnchor class];
    NSLayoutAnchor *r = [[[k alloc] init] autorelease];
    objc_storeWeak(&r->_item, item);
    r->_attr = a;
    return r;
}
- (void) dealloc { objc_storeWeak(&_item, nil); [super dealloc]; }
- (id) item { return objc_loadWeak(&_item); }
- (NSInteger) _clkAttribute { return _attr; }
- (NSString *) name { return attr_name(_attr); }
- (NSString *) description { return [NSString stringWithFormat: @"%@<%p> %@.%@", [self class], self, [self item], [self name]]; }
- (BOOL) isEqual: (id) o {
    return [o isKindOfClass: [NSLayoutAnchor class]] && [(NSLayoutAnchor *) o item] == [self item] && ((NSLayoutAnchor *) o)->_attr == _attr;
}
- (NSUInteger) hash { return (NSUInteger) [self item] ^ (NSUInteger) _attr; }

- (NSLayoutConstraint *) _c: (NSLayoutAnchor *) o rel: (NSInteger) rel m: (double) m k: (double) k {
    return [NSLayoutConstraint constraintWithItem: [self item] attribute: _attr relatedBy: rel toItem: [o item]
                                        attribute: o ? o->_attr : LA_NotAnAttribute multiplier: m constant: k];
}
- (NSLayoutConstraint *) constraintEqualToAnchor: (NSLayoutAnchor *) o { return [self _c: o rel: LR_EQ m: 1 k: 0]; }
- (NSLayoutConstraint *) constraintGreaterThanOrEqualToAnchor: (NSLayoutAnchor *) o { return [self _c: o rel: LR_GE m: 1 k: 0]; }
- (NSLayoutConstraint *) constraintLessThanOrEqualToAnchor: (NSLayoutAnchor *) o { return [self _c: o rel: LR_LE m: 1 k: 0]; }
- (NSLayoutConstraint *) constraintEqualToAnchor: (NSLayoutAnchor *) o constant: (double) k { return [self _c: o rel: LR_EQ m: 1 k: k]; }
- (NSLayoutConstraint *) constraintGreaterThanOrEqualToAnchor: (NSLayoutAnchor *) o constant: (double) k {
    return [self _c: o rel: LR_GE m: 1 k: k];
}
- (NSLayoutConstraint *) constraintLessThanOrEqualToAnchor: (NSLayoutAnchor *) o constant: (double) k {
    return [self _c: o rel: LR_LE m: 1 k: k];
}
@end

@implementation NSLayoutXAxisAnchor
/* Systemabstand: symbolischer Standardabstand mal Faktor (AppKit setzt den Abstand ein) */
- (NSLayoutConstraint *) _sys: (NSLayoutAnchor *) o rel: (NSInteger) rel m: (double) m {
    NSLayoutConstraint *k = [self _c: o rel: rel m: 1 k: 0];
    [k _clkSetStandardSpacing: YES];
    [k _clkSetSpacingFactor: m];
    return k;
}
- (NSLayoutConstraint *) constraintEqualToSystemSpacingAfterAnchor: (NSLayoutAnchor *) o multiplier: (double) m {
    return [self _sys: o rel: LR_EQ m: m];
}
- (NSLayoutConstraint *) constraintGreaterThanOrEqualToSystemSpacingAfterAnchor: (NSLayoutAnchor *) o multiplier: (double) m {
    return [self _sys: o rel: LR_GE m: m];
}
- (NSLayoutConstraint *) constraintLessThanOrEqualToSystemSpacingAfterAnchor: (NSLayoutAnchor *) o multiplier: (double) m {
    return [self _sys: o rel: LR_LE m: m];
}
@end

@implementation NSLayoutYAxisAnchor
- (NSLayoutConstraint *) constraintEqualToSystemSpacingBelowAnchor: (NSLayoutAnchor *) o multiplier: (double) m {
    NSLayoutConstraint *k = [self _c: o rel: LR_EQ m: 1 k: 0];
    [k _clkSetStandardSpacing: YES];
    return k;
}
- (NSLayoutConstraint *) constraintGreaterThanOrEqualToSystemSpacingBelowAnchor: (NSLayoutAnchor *) o multiplier: (double) m {
    NSLayoutConstraint *k = [self _c: o rel: LR_GE m: 1 k: 0];
    [k _clkSetStandardSpacing: YES];
    return k;
}
@end

@implementation NSLayoutDimension
- (NSLayoutConstraint *) constraintEqualToConstant: (double) k { return [self _c: nil rel: LR_EQ m: 1 k: k]; }
- (NSLayoutConstraint *) constraintGreaterThanOrEqualToConstant: (double) k { return [self _c: nil rel: LR_GE m: 1 k: k]; }
- (NSLayoutConstraint *) constraintLessThanOrEqualToConstant: (double) k { return [self _c: nil rel: LR_LE m: 1 k: k]; }
- (NSLayoutConstraint *) constraintEqualToAnchor: (NSLayoutAnchor *) o multiplier: (double) m { return [self _c: o rel: LR_EQ m: m k: 0]; }
- (NSLayoutConstraint *) constraintGreaterThanOrEqualToAnchor: (NSLayoutAnchor *) o multiplier: (double) m {
    return [self _c: o rel: LR_GE m: m k: 0];
}
- (NSLayoutConstraint *) constraintLessThanOrEqualToAnchor: (NSLayoutAnchor *) o multiplier: (double) m {
    return [self _c: o rel: LR_LE m: m k: 0];
}
- (NSLayoutConstraint *) constraintEqualToAnchor: (NSLayoutAnchor *) o multiplier: (double) m constant: (double) k {
    return [self _c: o rel: LR_EQ m: m k: k];
}
- (NSLayoutConstraint *) constraintGreaterThanOrEqualToAnchor: (NSLayoutAnchor *) o multiplier: (double) m constant: (double) k {
    return [self _c: o rel: LR_GE m: m k: k];
}
- (NSLayoutConstraint *) constraintLessThanOrEqualToAnchor: (NSLayoutAnchor *) o multiplier: (double) m constant: (double) k {
    return [self _c: o rel: LR_LE m: m k: k];
}
@end
