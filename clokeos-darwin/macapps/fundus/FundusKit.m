// FundusKit: Finder-Ansicht (Seitenleiste, Icons, Liste, Pfadleiste, Statuszeile) und Öffnen/Sichern-Panel.
// Nur AppKit-Grundbausteine, die Darlings AppKit kann: eigene NSViews mit drawRect:, NSScrollView, NSTextField,
// NSMenu. Dateisystem nur über FundusCore (fcore.h). Siehe docs/fundus.md.
#import "FundusKit.h"
#include "fcore.h"
#include <dlfcn.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// ------------------------------------------------------------------------------------------------ Hilfen
// Dateiprüfungen über FundusCore statt NSFileManager (Darlings fileExistsAtPath:isDirectory: hing in der VM)
BOOL fkExists(NSString *path, BOOL *isDir) {
	int d = 0;
	int r = path ? fc_exists([path UTF8String], &d) : 0;
	if (isDir) *isDir = d;
	return r;
}
static NSString *gResources;
static NSString *resources(void) {
	if (!gResources) {
		Dl_info d;
		if (dladdr((const void *)&resources, &d) && d.dli_fname) {
			// …/FundusKit.framework/Versions/A/FundusKit -> …/Versions/A/Resources
			NSString *bin = [NSString stringWithUTF8String:d.dli_fname];
			gResources = [[bin stringByDeletingLastPathComponent] stringByAppendingPathComponent:@"Resources"];
		}
		if (!gResources) gResources = @"/Library/ClokeOS/MacFrameworks/FundusKit.framework/Resources";
	}
	return gResources;
}

NSString *FKLanguage(void) {
	static NSString *lang;
	if (lang) return lang;
	const char *e = getenv("CLOKE_LANG");
	char buf[16] = "";
	if (e && *e) snprintf(buf, sizeof buf, "%s", e);
	else {
		FILE *f = fopen("/etc/clokeos-language", "r");
		if (f) { if (!fgets(buf, sizeof buf, f)) buf[0] = 0; fclose(f); }
	}
	buf[2] = 0;
	NSString *l = [NSString stringWithUTF8String:buf];
	NSArray *known = @[ @"en", @"de", @"fr", @"es", @"it" ];
	lang = [known containsObject:l] ? l : @"en";
	return lang;
}

NSString *FKLocalized(NSString *key) {
	static NSDictionary *tab, *en;
	if (!tab) {
		NSString *dir = resources();
		en = [NSDictionary dictionaryWithContentsOfFile:[dir stringByAppendingPathComponent:@"en.lproj/Localizable.strings"]];
		tab = [NSDictionary dictionaryWithContentsOfFile:[dir stringByAppendingPathComponent:
			[NSString stringWithFormat:@"%@.lproj/Localizable.strings", FKLanguage()]]];
		if (!tab) tab = en ? en : @{};
		if (!en) en = @{};
	}
	NSString *v = tab[key];
	if (!v) v = en[key];
	return v ? v : key;
}
#define L(k) FKLocalized(@k)
#define TR(...) do { if (getenv("FUNDUS_TRACE")) { fprintf(stderr, "fundus: " __VA_ARGS__); fputc(10, stderr); } } while (0)

static NSColor *rgb(int r, int g, int b, CGFloat a) {
	return [NSColor colorWithCalibratedRed:r / 255.0 green:g / 255.0 blue:b / 255.0 alpha:a];
}
static NSColor *accent(void) { return rgb(124, 92, 255, 1); }

// Hell/dunkel wie das Programm (unser AppKit: CLOKEOS_DARK von CloakServer bzw. AppleInterfaceStyle; macOS: Systemeinstellung).
// Farben nach Finder-Aufnahmen (macOS 27): Seitenleiste etwas dunkler als der Inhalt, Zebrastreifen, Kapseln in der Leiste.
static BOOL dark(void) {
	NSAppearance *a = [NSApp respondsToSelector:@selector(effectiveAppearance)] ? [NSApp effectiveAppearance] : nil;
	return a && [[a name] rangeOfString:@"Dark"].location != NSNotFound;
}
#define PAL(light, darkc) (dark() ? (darkc) : (light))
static NSColor *fkText(void) { return PAL(rgb(29, 29, 31, 1), rgb(255, 255, 255, 0.9)); }
static NSColor *fkSecondary(void) { return PAL(rgb(60, 60, 67, 0.62), rgb(235, 235, 245, 0.62)); }
static NSColor *fkTertiary(void) { return PAL(rgb(60, 60, 67, 0.42), rgb(235, 235, 245, 0.4)); }
static NSColor *fkContent(void) { return PAL(rgb(255, 255, 255, 1), rgb(31, 33, 32, 1)); }
static NSColor *fkSidebar(void) { return PAL(rgb(242, 242, 245, 1), rgb(38, 40, 39, 1)); }
static NSColor *fkAltRow(void) { return PAL(rgb(244, 244, 247, 1), rgb(42, 45, 43, 1)); }
static NSColor *fkPill(void) { return PAL(rgb(0, 0, 0, 0.055), rgb(255, 255, 255, 0.11)); }
static NSColor *fkPillSel(void) { return PAL(rgb(0, 0, 0, 0.1), rgb(255, 255, 255, 0.18)); }
static NSColor *fkSideSel(void) { return PAL(rgb(0, 0, 0, 0.085), rgb(255, 255, 255, 0.1)); }
static NSColor *fkIconSel(void) { return PAL(rgb(0, 0, 0, 0.08), rgb(255, 255, 255, 0.12)); }
static NSColor *fkGlyph(void) { return PAL(rgb(60, 60, 67, 0.85), rgb(235, 235, 245, 0.85)); }
static NSColor *fkLine(void) { return PAL(rgb(0, 0, 0, 0.08), rgb(255, 255, 255, 0.08)); }
static void pill(NSRect r, NSColor *c) {
	[c setFill];
	CGFloat rad = NSHeight(r) / 2;
	[[NSBezierPath bezierPathWithRoundedRect:r xRadius:rad yRadius:rad] fill];
}

static NSMutableDictionary *gImages;
static NSImage *image(NSString *name) {
	if (!gImages) gImages = [NSMutableDictionary dictionary];
	NSImage *i = gImages[name];
	if (!i) {
		i = [[NSImage alloc] initWithContentsOfFile:[resources() stringByAppendingPathComponent:[name stringByAppendingString:@".png"]]];
		if (i) gImages[name] = i;
	}
	return i;
}

static NSString *specialIconName(NSString *path) {
	static NSDictionary *map;
	if (!map) {
		NSMutableDictionary *m = [NSMutableDictionary dictionary];
		struct { fc_special s; NSString *icon; } t[] = {
			{ FC_HOME, @"folder-home" }, { FC_DESKTOP, @"folder-desktop" }, { FC_DOCUMENTS, @"folder-documents" },
			{ FC_DOWNLOADS, @"folder-downloads" }, { FC_MOVIES, @"folder-movies" }, { FC_MUSIC, @"folder-music" },
			{ FC_PICTURES, @"folder-pictures" }, { FC_PUBLIC, @"folder-public" }, { FC_LIBRARY, @"folder-library" },
			{ FC_APPLICATIONS, @"folder-applications" }, { FC_ROOT, @"disk" } };
		for (size_t i = 0; i < sizeof t / sizeof t[0]; i++) m[[NSString stringWithUTF8String:fc_special_path(t[i].s)]] = t[i].icon;
		m[@"/System/Applications"] = @"folder-applications";
		m[@"/Library"] = @"folder-library";
		m[@"/Users"] = @"folder-public";
		map = m;
	}
	return map[path];
}

static NSString *kindIconName(fc_kind k) {
	switch (k) {
	case FC_FOLDER: return @"folder-folder";
	case FC_APP: return @"app";
	case FC_IMAGE: return @"file-image";
	case FC_AUDIO: return @"file-audio";
	case FC_VIDEO: return @"file-video";
	case FC_TEXT: return @"file-text";
	case FC_CODE: return @"file-code";
	case FC_PDF: return @"file-pdf";
	case FC_ARCHIVE: return @"file-archive";
	case FC_DISKIMAGE: return @"file-diskimage";
	case FC_EXE: return @"file-exe";
	case FC_DOCUMENT: return @"file-document";
	case FC_PACKAGE: return @"file-package";
	case FC_EXECUTABLE: return @"file-executable";
	default: return @"file-file";
	}
}

// Programm-Icon aus dem Paket (PNG; ClokeOS-Pakete haben AppIcon.png neben AppIcon.svg), sonst das allgemeine
static NSImage *appIcon(NSString *path, BOOL small) {
	static NSMutableDictionary *cache;
	if (!cache) cache = [NSMutableDictionary dictionary];
	id c = cache[path];
	if (c) return c == [NSNull null] ? nil : c;
	NSString *res = [path stringByAppendingPathComponent:@"Contents/Resources"];
	NSImage *img = nil;
	for (NSString *n in @[ @"AppIcon.png", @"icon.png" ]) {
		img = [[NSImage alloc] initWithContentsOfFile:[res stringByAppendingPathComponent:n]];
		if (img) break;
	}
	if (!img) {
		NSDictionary *info = [NSDictionary dictionaryWithContentsOfFile:[path stringByAppendingPathComponent:@"Contents/Info.plist"]];
		NSString *f = info[@"CFBundleIconFile"];
		if ([f length]) {
			if (![[f pathExtension] length]) f = [f stringByAppendingPathExtension:@"icns"];
			img = [[NSImage alloc] initWithContentsOfFile:[res stringByAppendingPathComponent:f]];
			if (img && [img size].width < 1) img = nil;
		}
	}
	cache[path] = img ? img : [NSNull null];
	(void)small;
	return img;
}

static NSImage *iconFor(NSString *path, fc_kind kind, BOOL small) {
	NSString *n = specialIconName(path);
	if (kind == FC_APP) { NSImage *a = appIcon(path, small); if (a) return a; }
	if ([path isEqualToString:[NSString stringWithUTF8String:fc_special_path(FC_TRASH)]])
		n = fc_trash_count() > 0 ? @"trash-full" : @"trash-empty";
	if (!n) n = kindIconName(kind);
	if (small) { NSImage *s = image([n stringByAppendingString:@"-32"]); if (s) return s; }
	return image(n);
}

static NSDictionary *textAttrs(CGFloat size, BOOL bold, NSColor *color, NSTextAlignment align) {
	NSMutableParagraphStyle *p = [[NSMutableParagraphStyle alloc] init];
	[p setAlignment:align];
	[p setLineBreakMode:NSLineBreakByTruncatingMiddle];
	return @{ NSFontAttributeName: bold ? [NSFont boldSystemFontOfSize:size] : [NSFont systemFontOfSize:size],
		NSForegroundColorAttributeName: color, NSParagraphStyleAttributeName: p };
}

static void drawText(NSString *s, NSRect r, CGFloat size, BOOL bold, NSColor *c, NSTextAlignment a) {
	[s drawInRect:r withAttributes:textAttrs(size, bold, c, a)];
}

// Anzeigename: Standardordner übersetzt wie im Finder ("Documents" heißt auf Deutsch "Dokumente")
static NSString *nameOf(NSString *path) {
	if ([path isEqualToString:@"/"]) return @"Cloke HD";
	fc_special sp[] = { FC_DESKTOP, FC_DOCUMENTS, FC_DOWNLOADS, FC_MOVIES, FC_MUSIC, FC_PICTURES, FC_PUBLIC, FC_LIBRARY,
		FC_APPLICATIONS, FC_TRASH };
	for (size_t i = 0; i < sizeof sp / sizeof sp[0]; i++)
		if ([path isEqualToString:[NSString stringWithUTF8String:fc_special_path(sp[i])]])
			return FKLocalized([NSString stringWithUTF8String:fc_special_key(sp[i])]);
	if ([path isEqualToString:@"/System/Applications"]) return L("sidebar.applications");
	return [path lastPathComponent];
}

static NSString *dateString(time_t t) {
	static NSDateFormatter *day, *tf;
	if (!t) return @"--";
	BOOL en = [FKLanguage() isEqualToString:@"en"];
	if (!day) {
		day = [[NSDateFormatter alloc] init];
		[day setDateFormat:en ? @"MMM d, yyyy 'at' h:mm a" : @"dd.MM.yyyy, HH:mm"];
		tf = [[NSDateFormatter alloc] init];
		[tf setDateFormat:en ? @"h:mm a" : @"HH:mm"];
	}
	time_t now = (time_t)[[NSDate date] timeIntervalSince1970];
	struct tm a, b;
	localtime_r(&t, &a);
	localtime_r(&now, &b);
	NSDate *d = [NSDate dateWithTimeIntervalSince1970:t];
	if (a.tm_year == b.tm_year && a.tm_yday == b.tm_yday) return [NSString stringWithFormat:L("date.today"), [tf stringFromDate:d]];
	if (a.tm_year == b.tm_year && a.tm_yday == b.tm_yday - 1) return [NSString stringWithFormat:L("date.yesterday"), [tf stringFromDate:d]];
	return [day stringFromDate:d];
}

static NSString *sizeString(int64_t b) {
	char s[64]; fc_format_size(b, ![FKLanguage() isEqualToString:@"en"], s, sizeof s);
	return [NSString stringWithUTF8String:s];
}

// Keine eigene Fensterleiste (wie der Finder): Inhalt bis oben, die Ampel liegt darüber. Unter ClokeOS setzt
// macapps/appkit/titlebar.m daraus _CLOKE_TITLEBAR = overlay; unter macOS macht AppKit dasselbe.
void FKHideTitlebar(NSWindow *w) {
	[w setStyleMask:[w styleMask] | NSWindowStyleMaskFullSizeContentView];
	if ([w respondsToSelector:@selector(setTitlebarAppearsTransparent:)]) [w setTitlebarAppearsTransparent:YES];
	if ([w respondsToSelector:@selector(setTitleVisibility:)]) [w setTitleVisibility:NSWindowTitleHidden];
}

// ------------------------------------------------------------------------------------------------ Eintrag
@interface FKItem : NSObject
@property (copy) NSString *name, *path;
@property fc_kind kind;
@property BOOL isDir, hidden, dimmed;
@property int64_t size;
@property time_t mtime;
@end
@implementation FKItem
@end

// ------------------------------------------------------------------------------------------------ Vorschauen
// Wie im Finder zeigen Bilder ihr eigenes Bild als Icon, Text und Quellcode
// eine kleine Seite mit dem Anfang der Datei. Einmal pro Datei und Änderungszeit erzeugt und im Speicher gehalten.
#define FK_MAX_PREVIEW_BYTES (40 * 1024 * 1024)
static NSMutableDictionary *gThumbs, *gSnippets;

static NSString *previewKey(FKItem *it) { return [NSString stringWithFormat:@"%@|%ld", it.path, (long)it.mtime]; }

static BOOL hasImagePreview(FKItem *it) {
	// PDF noch nicht: Darlings NSImage stürzt im PDF-Leser von Onyx2D ab (SIGTRAP), siehe docs/fundus.md
	if (it.kind != FC_IMAGE) return NO;
	NSString *e = [[it.path pathExtension] lowercaseString];
	return [@[ @"png", @"jpg", @"jpeg", @"gif", @"tif", @"tiff", @"bmp" ] containsObject:e] &&
	       it.size > 0 && it.size < FK_MAX_PREVIEW_BYTES;
}

static NSImage *loadPreviewImage(NSString *path) {
	NSImage *src = nil;
	@try { src = [[NSImage alloc] initWithContentsOfFile:path]; } @catch (NSException *e) { src = nil; }
	if (!src) return nil;
	NSSize sz = [src size];
	if (sz.width < 1 || sz.height < 1) return nil;
	return src;
}

static NSImage *thumbnail(FKItem *it) {
	if (!hasImagePreview(it)) return nil;
	if (!gThumbs) gThumbs = [NSMutableDictionary dictionary];
	NSString *k = previewKey(it);
	id t = gThumbs[k];
	if (t) return t == [NSNull null] ? nil : t;
	NSImage *src = loadPreviewImage(it.path);
	NSImage *small = nil;
	if (src) {   // auf höchstens 256 Pixel verkleinern, damit große Fotos nicht bei jedem Zeichnen skaliert werden
		NSSize sz = [src size];
		CGFloat f = MIN(1.0, 256.0 / MAX(sz.width, sz.height));
		NSSize ts = NSMakeSize(MAX(1, round(sz.width * f)), MAX(1, round(sz.height * f)));
		@try {
			small = [[NSImage alloc] initWithSize:ts];
			[small lockFocus];
			[src drawInRect:NSMakeRect(0, 0, ts.width, ts.height) fromRect:NSZeroRect operation:NSCompositeSourceOver fraction:1];
			[small unlockFocus];
		} @catch (NSException *e) { small = src; }
	}
	gThumbs[k] = small ? small : [NSNull null];
	return small;
}

static BOOL hasTextPreview(FKItem *it) { return (it.kind == FC_TEXT || it.kind == FC_CODE) && it.size > 0; }

static NSString *textSnippet(FKItem *it, NSUInteger maxBytes) {
	if (!hasTextPreview(it)) return nil;
	if (!gSnippets) gSnippets = [NSMutableDictionary dictionary];
	NSString *k = [previewKey(it) stringByAppendingFormat:@"|%lu", (unsigned long)maxBytes];
	NSString *t = gSnippets[k];
	if (t) return t;
	FILE *f = fopen([it.path UTF8String], "rb");
	if (!f) return nil;
	char *buf = malloc(maxBytes + 1);
	size_t n = fread(buf, 1, maxBytes, f);
	fclose(f);
	buf[n] = 0;
	t = [[NSString alloc] initWithBytes:buf length:n encoding:NSUTF8StringEncoding];
	if (!t) t = [[NSString alloc] initWithBytes:buf length:n encoding:NSISOLatin1StringEncoding];
	free(buf);
	if (!t) t = @"";
	gSnippets[k] = t;
	return t;
}

static NSRect fitRect(NSSize s, NSRect box) {
	CGFloat f = MIN(NSWidth(box) / s.width, NSHeight(box) / s.height);
	NSSize d = NSMakeSize(s.width * f, s.height * f);
	return NSMakeRect(NSMidX(box) - d.width / 2, NSMidY(box) - d.height / 2, d.width, d.height);
}

// Vorschau eines Eintrags in box zeichnen; NO, wenn es keine gibt (dann das normale Icon)
static BOOL drawPreview(FKItem *it, NSRect box, CGFloat alpha, BOOL small) {
	static int mode = -1;   // FUNDUS_PREVIEW=0 aus, =img nur Bilder, =text nur Text (zur Fehlersuche)
	if (mode < 0) { const char *e = getenv("FUNDUS_PREVIEW"); mode = !e ? 3 : !strcmp(e, "0") ? 0 : !strcmp(e, "img") ? 1 : !strcmp(e, "text") ? 2 : 3; }
	if (!mode) return NO;
	TR("preview %s", [it.name UTF8String]);
	NSImage *t = (mode & 1) ? thumbnail(it) : nil;
	if (t) {
		NSRect r = fitRect([t size], NSInsetRect(box, small ? 1 : 3, small ? 1 : 3));
		[t drawInRect:r fromRect:NSZeroRect operation:NSCompositeSourceOver fraction:alpha respectFlipped:YES hints:nil];
		return YES;
	}
	if ((mode & 2) && hasTextPreview(it) && !small) {
		NSString *txt = textSnippet(it, 1200);
		if (!txt) return NO;
		CGFloat h = NSHeight(box) - 4, w = round(h * 0.78);
		NSRect page = NSMakeRect(NSMidX(box) - w / 2, NSMinY(box) + 2, w, h);
		[[NSColor whiteColor] setFill];
		NSRectFill(page);
		[rgb(0, 0, 0, 0.2) setStroke];
		[NSBezierPath strokeRect:NSInsetRect(page, 0.5, 0.5)];
		NSMutableParagraphStyle *ps = [[NSMutableParagraphStyle alloc] init];
		[ps setLineBreakMode:NSLineBreakByCharWrapping];
		NSFont *font = it.kind == FC_CODE ? [NSFont userFixedPitchFontOfSize:4] : [NSFont systemFontOfSize:4];
		if (!font) font = [NSFont systemFontOfSize:4];
		[txt drawInRect:NSInsetRect(page, 4, 4) withAttributes:@{ NSFontAttributeName: font,
			NSForegroundColorAttributeName: rgb(40, 40, 45, alpha), NSParagraphStyleAttributeName: ps }];
		return YES;
	}
	return NO;
}

// ------------------------------------------------------------------------------------------------ kleine Knöpfe
typedef NS_ENUM(NSInteger, FKGlyph) { FKGlyphBack, FKGlyphForward, FKGlyphIcons, FKGlyphList, FKGlyphNewFolder, FKGlyphUp, FKGlyphSearch };

@interface FKToolButton : NSView
@property FKGlyph glyph;
@property BOOL enabled, selected, pressed;
@property (weak) id target;
@property SEL action;
@end
@implementation FKToolButton
- (BOOL)isFlipped { return YES; }
- (BOOL)mouseDownCanMoveWindow { return NO; }
- (void)drawRect:(NSRect)dirty {
	NSRect b = [self bounds];
	if (self.selected || self.pressed) pill(NSInsetRect(b, 3, 3), self.pressed ? fkPillSel() : fkPillSel());
	[(self.enabled ? fkGlyph() : fkTertiary()) set];
	CGFloat mx = NSMidX(b), my = NSMidY(b);
	NSBezierPath *p = [NSBezierPath bezierPath];
	[p setLineWidth:1.8];
	[p setLineCapStyle:NSRoundLineCapStyle];
	[p setLineJoinStyle:NSRoundLineJoinStyle];
	switch (self.glyph) {
	case FKGlyphBack: [p moveToPoint:NSMakePoint(mx + 3, my - 7)]; [p lineToPoint:NSMakePoint(mx - 4, my)]; [p lineToPoint:NSMakePoint(mx + 3, my + 7)]; [p stroke]; break;
	case FKGlyphForward: [p moveToPoint:NSMakePoint(mx - 3, my - 7)]; [p lineToPoint:NSMakePoint(mx + 4, my)]; [p lineToPoint:NSMakePoint(mx - 3, my + 7)]; [p stroke]; break;
	case FKGlyphUp: [p moveToPoint:NSMakePoint(mx - 7, my + 3)]; [p lineToPoint:NSMakePoint(mx, my - 4)]; [p lineToPoint:NSMakePoint(mx + 7, my + 3)]; [p stroke]; break;
	case FKGlyphIcons:
		for (int i = 0; i < 4; i++) {
			NSBezierPath *q = [NSBezierPath bezierPathWithRoundedRect:NSMakeRect(mx - 7.5 + (i % 2) * 8.5, my - 7.5 + (i / 2) * 8.5, 6.5, 6.5) xRadius:1.8 yRadius:1.8];
			[q setLineWidth:1.5]; [q stroke];
		}
		break;
	case FKGlyphList:
		for (int i = 0; i < 3; i++) {
			[[NSBezierPath bezierPathWithOvalInRect:NSMakeRect(mx - 8.5, my - 6.6 + i * 5.5, 2.6, 2.6)] fill];
			[p moveToPoint:NSMakePoint(mx - 3.5, my - 5.3 + i * 5.5)]; [p lineToPoint:NSMakePoint(mx + 8.5, my - 5.3 + i * 5.5)];
		}
		[p setLineWidth:1.6]; [p stroke];
		break;
	case FKGlyphNewFolder: {
		NSBezierPath *f = [NSBezierPath bezierPath];
		[f moveToPoint:NSMakePoint(mx - 9, my + 6)]; [f lineToPoint:NSMakePoint(mx - 9, my - 6)]; [f lineToPoint:NSMakePoint(mx - 3, my - 6)];
		[f lineToPoint:NSMakePoint(mx - 1, my - 4)]; [f lineToPoint:NSMakePoint(mx + 9, my - 4)]; [f lineToPoint:NSMakePoint(mx + 9, my + 6)]; [f closePath];
		[f setLineWidth:1.5]; [f setLineJoinStyle:NSRoundLineJoinStyle]; [f stroke];
		[p setLineWidth:1.5];
		[p moveToPoint:NSMakePoint(mx, my - 1.5)]; [p lineToPoint:NSMakePoint(mx, my + 3.5)];
		[p moveToPoint:NSMakePoint(mx - 2.5, my + 1)]; [p lineToPoint:NSMakePoint(mx + 2.5, my + 1)]; [p stroke];
		break; }
	case FKGlyphSearch: {
		NSBezierPath *c = [NSBezierPath bezierPathWithOvalInRect:NSMakeRect(mx - 7, my - 7, 11, 11)];
		[c setLineWidth:1.8]; [c stroke];
		[p moveToPoint:NSMakePoint(mx + 2.5, my + 2.5)]; [p lineToPoint:NSMakePoint(mx + 7, my + 7)]; [p stroke];
		break; }
	}
}
- (void)mouseDown:(NSEvent *)e { if (self.enabled) { self.pressed = YES; [self setNeedsDisplay:YES]; } }
- (void)mouseUp:(NSEvent *)e {
	if (!self.pressed) return;
	self.pressed = NO; [self setNeedsDisplay:YES];
	NSPoint p = [self convertPoint:[e locationInWindow] fromView:nil];
	if (NSPointInRect(p, [self bounds]) && self.target && self.action) [NSApp sendAction:self.action to:self.target from:self];
}
@end

// Leiste oben: Hintergrund und Kapseln hinter den Knopfgruppen (wie in der Finder-Leiste von macOS 27)
@interface FKHeaderView : NSView
@property (strong) NSArray *pills;   // NSValue (NSRect)
@end
@implementation FKHeaderView
- (BOOL)isFlipped { return YES; }
- (void)drawRect:(NSRect)dirty {
	for (NSValue *v in self.pills) pill([v rectValue], fkPill());
	if ([self.pills count]) {   // Trennstrich in der Zurück/Vor-Kapsel
		NSRect r = [self.pills[0] rectValue];
		[fkLine() setFill];
		NSRectFill(NSMakeRect(NSMidX(r), NSMinY(r) + 9, 1, NSHeight(r) - 18));
	}
}
@end

// ------------------------------------------------------------------------------------------------ Seitenleiste
@interface FKSidebarView : NSView
@property (weak) FKBrowserView *browser;
@property (strong) NSArray *rows;   // je Zeile: @{title, path, icon} oder @{header}
@property (copy) NSString *current;
@property NSInteger dropRow;   // Ziehen: Zeile unter dem Zeiger (-1 keine)
- (NSInteger)rowAtPoint:(NSPoint)p;
@end

@interface FKItemsView : NSView <NSTextFieldDelegate>
@property (weak) FKBrowserView *browser;
@property (strong) NSArray *items;
@property (strong) NSMutableIndexSet *selection;
@property FKViewMode mode;
@property NSInteger anchor;
@property (strong) NSTextField *editor;
@property NSInteger editing;
@property (copy) NSString *typeAhead;
@property NSTimeInterval typeTime;
@property NSInteger dropIndex;   // Ziehen: Ordner unter dem Zeiger (-1 keiner)
@property BOOL dropOnView;       // Ziehen: Ablage in den angezeigten Ordner
- (void)layoutForWidth:(CGFloat)w;
- (NSRect)rectOfItem:(NSInteger)i;
- (void)fkMoveForQuickLook:(NSEvent *)e;
@end

@interface FKPathBar : NSView
@property (weak) FKBrowserView *browser;
@property BOOL statusOnly;   // Statusleiste: nur der Text in der Mitte
@property (copy) NSString *path;
@property (copy) NSString *status;
@end

@interface FKBrowserView () <NSTextFieldDelegate>
@property (strong) FKSidebarView *sidebar;
@property (strong) NSScrollView *scroll;
@property (strong) FKItemsView *itemsView;
@property (strong) FKPathBar *pathBar;
@property (strong) FKHeaderView *header;
@property (strong) FKToolButton *backButton, *forwardButton, *iconsButton, *listButton, *folderButton, *searchButton;
@property (strong) FKPathBar *statusBar;
@property BOOL searchOpen;
@property (strong) NSTextField *search, *titleLabel;
@property (strong) NSMutableArray *history;
@property NSInteger historyPos;
@property (copy, readwrite) NSString *directory;
@property fc_sort sortBy;
@property BOOL sortDesc;
- (void)itemsDoubleClicked:(NSInteger)index;
- (void)selectionChanged;
- (void)sortByColumn:(fc_sort)s;
- (NSMenu *)contextMenuForItem:(NSInteger)index;
- (void)toggleQuickLook;
- (BOOL)quickLookVisible;
@end

static void fkRunDrag(FKItemsView *src, NSEvent *ev, NSPoint startInWindow, NSInteger grabbed);
static NSMutableArray *gClipboard;
static BOOL gClipboardCut;

@implementation FKSidebarView
- (BOOL)isFlipped { return YES; }
- (void)rebuild {
	NSMutableArray *r = [NSMutableArray array];
	[r addObject:@{ @"header": L("sidebar.favorites") }];
	fc_special fav[] = { FC_APPLICATIONS, FC_DESKTOP, FC_DOCUMENTS, FC_DOWNLOADS, FC_HOME, FC_MOVIES, FC_MUSIC, FC_PICTURES };
	static const char *glyph[FC_SPECIAL_COUNT];
	glyph[FC_APPLICATIONS] = "side-applications"; glyph[FC_DESKTOP] = "side-desktop"; glyph[FC_DOCUMENTS] = "side-documents";
	glyph[FC_DOWNLOADS] = "side-downloads"; glyph[FC_HOME] = "side-home"; glyph[FC_MOVIES] = "side-movies";
	glyph[FC_MUSIC] = "side-music"; glyph[FC_PICTURES] = "side-pictures";
	for (size_t i = 0; i < sizeof fav / sizeof fav[0]; i++) {
		NSString *p = [NSString stringWithUTF8String:fc_special_path(fav[i])];
		TR("s fav %zu %s", i, fc_special_path(fav[i]));
		BOOL isDir = NO;
		if (!fkExists(p, &isDir) || !isDir) continue;
		NSString *t = fav[i] == FC_HOME ? [p lastPathComponent] : FKLocalized([NSString stringWithUTF8String:fc_special_key(fav[i])]);
		[r addObject:@{ @"title": t, @"path": p, @"icon": [NSString stringWithUTF8String:glyph[fav[i]]] }];
	}
	TR("s locations");
	[r addObject:@{ @"header": L("sidebar.locations") }];
	[r addObject:@{ @"title": @"Cloke HD", @"path": @"/", @"icon": @"side-disk" }];
	fc_list vl;
	if (fc_read_dir("/Volumes", 0, &vl)) vl.n = 0, vl.v = NULL;
	for (int k = 0; k < vl.n; k++) {
		NSString *v = [NSString stringWithUTF8String:vl.v[k].name];
		if (!v || !vl.v[k].is_dir) continue;
		NSString *p = [@"/Volumes" stringByAppendingPathComponent:v];
		[r addObject:@{ @"title": v, @"path": p, @"icon": @"side-volumes" }];
	}
	fc_list_free(&vl);
	TR("s volumes done");
	BOOL d = NO;
	if (fkExists(@"/NT", &d) && d)
		[r addObject:@{ @"title": L("sidebar.nt"), @"path": @"/NT", @"icon": @"side-nt" }];
	if (fkExists(@"/TUX", &d) && d)
		[r addObject:@{ @"title": L("sidebar.tux"), @"path": @"/TUX", @"icon": @"side-tux" }];
	[r addObject:@{ @"title": L("sidebar.trash"), @"path": [NSString stringWithUTF8String:fc_special_path(FC_TRASH)], @"icon": @"side-trash" }];
	TR("s rows");
	self.rows = r;
	[self setNeedsDisplay:YES];
	TR("s done");
}
- (CGFloat)yOfRow:(NSInteger)i {
	CGFloat y = 52;   // darüber sitzt die Ampel (keine eigene Fensterleiste, wie im Finder)
	for (NSInteger k = 0; k < i; k++) y += self.rows[k][@"header"] ? (k ? 34 : 26) : 30;
	return y;
}
- (void)drawRect:(NSRect)dirty {
	[fkSidebar() setFill];
	NSRectFill([self bounds]);
	for (NSInteger i = 0; i < (NSInteger)[self.rows count]; i++) {
		NSDictionary *row = self.rows[i];
		CGFloat y = [self yOfRow:i];
		if (row[@"header"]) {
			drawText(row[@"header"], NSMakeRect(18, y + (i ? 14 : 6), NSWidth([self bounds]) - 24, 16), 11, YES, fkTertiary(), NSLeftTextAlignment);
			continue;
		}
		NSRect r = NSMakeRect(10, y, NSWidth([self bounds]) - 20, 28);
		BOOL drop = i == self.dropRow && self.dropRow > 0;
		if (drop || [row[@"path"] isEqualToString:self.current]) {
			[(drop ? accent() : fkSideSel()) setFill];
			[[NSBezierPath bezierPathWithRoundedRect:r xRadius:8 yRadius:8] fill];
		}
		NSImage *g = image(row[@"icon"]);
		if (g) [g drawInRect:NSMakeRect(19, y + 5, 18, 18) fromRect:NSZeroRect operation:NSCompositeSourceOver fraction:1 respectFlipped:YES hints:nil];
		drawText(row[@"title"], NSMakeRect(45, y + 6, NSWidth(r) - 40, 18), 13, NO, drop ? [NSColor whiteColor] : fkText(), NSLeftTextAlignment);
	}
}
- (NSInteger)rowAtPoint:(NSPoint)p {
	for (NSInteger i = 0; i < (NSInteger)[self.rows count]; i++) {
		CGFloat y = [self yOfRow:i];
		if (p.y >= y && p.y < y + 30 && self.rows[i][@"path"]) return i;
	}
	return -1;
}
- (void)mouseDown:(NSEvent *)e {
	NSInteger i = [self rowAtPoint:[self convertPoint:[e locationInWindow] fromView:nil]];
	if (i >= 0) [self.browser goTo:self.rows[i][@"path"]];
}
@end

// ------------------------------------------------------------------------------------------------ Pfadleiste
@implementation FKPathBar
- (BOOL)isFlipped { return YES; }
- (NSArray *)parts {
	NSMutableArray *a = [NSMutableArray array];
	NSString *p = self.path;
	while ([p length]) {
		[a insertObject:p atIndex:0];
		if ([p isEqualToString:@"/"]) break;
		p = [p stringByDeletingLastPathComponent];
		if (![p length]) p = @"/";
	}
	return a;
}
- (void)drawRect:(NSRect)dirty {
	[fkContent() setFill];
	NSRectFill([self bounds]);
	[fkLine() setFill];
	NSRectFill(NSMakeRect(0, 0, NSWidth([self bounds]), 1));
	if (self.statusOnly) {
		drawText(self.status ? self.status : @"", NSMakeRect(10, 4, NSWidth([self bounds]) - 20, 15), 11, NO, fkSecondary(), NSCenterTextAlignment);
		return;
	}
	CGFloat x = 12;
	NSArray *parts = [self parts];
	for (NSUInteger i = 0; i < [parts count]; i++) {
		NSString *n = nameOf(parts[i]);
		NSImage *ic = iconFor(parts[i], FC_FOLDER, YES);
		if (ic) [ic drawInRect:NSMakeRect(x, 5, 14, 14) fromRect:NSZeroRect operation:NSCompositeSourceOver fraction:1 respectFlipped:YES hints:nil];
		x += 17;
		NSDictionary *at = textAttrs(11, NO, fkSecondary(), NSLeftTextAlignment);
		CGFloat w = MIN([n sizeWithAttributes:at].width + 2, 160);
		[n drawInRect:NSMakeRect(x, 4, w, 15) withAttributes:at];
		x += w + 4;
		if (i + 1 < [parts count]) { drawText(@"›", NSMakeRect(x, 3, 10, 16), 12, NO, fkTertiary(), NSLeftTextAlignment); x += 12; }
	}
}
- (void)mouseDown:(NSEvent *)e {
	NSPoint p = [self convertPoint:[e locationInWindow] fromView:nil];
	CGFloat x = 10;
	NSArray *parts = [self parts];
	for (NSUInteger i = 0; i < [parts count]; i++) {
		NSString *n = nameOf(parts[i]);
		CGFloat w = MIN([n sizeWithAttributes:textAttrs(11, NO, [NSColor blackColor], NSLeftTextAlignment)].width + 2, 160);
		if (p.x >= x && p.x < x + 17 + w + 4) { [self.browser goTo:parts[i]]; return; }
		x += 17 + w + 4 + 12;
	}
}
@end

// ------------------------------------------------------------------------------------------------ Icons und Liste
#define ICON_W 112
#define ICON_H 112
#define LIST_ROW 20
#define LIST_HEAD 30

@implementation FKItemsView
- (BOOL)isFlipped { return YES; }
- (BOOL)acceptsFirstResponder { return YES; }
- (instancetype)initWithFrame:(NSRect)f {
	if ((self = [super initWithFrame:f])) { _selection = [NSMutableIndexSet indexSet]; _editing = -1; _anchor = -1; _dropIndex = -1; }
	return self;
}
// macOS: Klick in ein Hintergrundfenster geht gleich an die Ansicht, das Fenster kommt erst beim Loslassen nach vorn
// (und gar nicht, wenn gezogen wurde: preventWindowOrdering). Unter ClokeOS macht das CloakServer (pending_focus).
- (BOOL)acceptsFirstMouse:(NSEvent *)e { return YES; }
- (BOOL)shouldDelayWindowOrderingForEvent:(NSEvent *)e {
	return [self indexAtPoint:[self convertPoint:[e locationInWindow] fromView:nil]] >= 0;
}
- (NSInteger)columns { return MAX(1, (NSInteger)((NSWidth([[self enclosingScrollView] contentView].bounds) - 16) / ICON_W)); }
- (void)layoutForWidth:(CGFloat)w {
	CGFloat h;
	NSInteger n = [self.items count];
	if (self.mode == FKViewModeIcons) {
		NSInteger cols = MAX(1, (NSInteger)((w - 16) / ICON_W));
		h = 12 + ((n + cols - 1) / cols) * ICON_H + 12;
	} else h = LIST_HEAD + n * LIST_ROW + 8;
	NSSize vis = [[self enclosingScrollView] contentSize];
	[self setFrameSize:NSMakeSize(w, MAX(h, vis.height))];
	[self setNeedsDisplay:YES];
}
- (NSRect)rectOfItem:(NSInteger)i {
	if (self.mode == FKViewModeIcons) {
		NSInteger cols = [self columns];
		CGFloat w = NSWidth([self bounds]);
		CGFloat left = MAX(8, (w - cols * ICON_W) / 2);
		return NSMakeRect(left + (i % cols) * ICON_W, 10 + (i / cols) * ICON_H, ICON_W, ICON_H);
	}
	return NSMakeRect(0, LIST_HEAD + i * LIST_ROW, NSWidth([self bounds]), LIST_ROW);
}
- (NSInteger)indexAtPoint:(NSPoint)p {
	for (NSInteger i = 0; i < (NSInteger)[self.items count]; i++) {
		NSRect r = [self rectOfItem:i];
		if (self.mode == FKViewModeIcons) r = NSInsetRect(r, 8, 2);
		if (NSPointInRect(p, r)) return i;
	}
	return -1;
}
- (CGFloat)col:(int)c {   // Listenspalten: Name, Änderungsdatum, Größe (rechtsbündig bis col:3), Art
	CGFloat w = NSWidth([self bounds]);
	CGFloat x[] = { 14, MAX(240, w * 0.48), MAX(400, w * 0.69), MAX(500, w * 0.82) };
	return x[c];
}
- (void)drawRect:(NSRect)dirty {
	[fkContent() setFill];
	NSRectFill(dirty);
	CGFloat W = NSWidth([self bounds]);
	if (self.mode == FKViewModeList) {
		// Zebrastreifen bis zum unteren Rand, abgerundet und eingerückt wie im Finder
		NSRect vis = [self visibleRect];
		NSInteger rows = MAX((NSInteger)[self.items count], (NSInteger)((NSMaxY(vis) - LIST_HEAD) / LIST_ROW) + 1);
		for (NSInteger i = 1; i < rows; i += 2) {
			NSRect r = NSMakeRect(10, LIST_HEAD + i * LIST_ROW, W - 20, LIST_ROW);
			if (!NSIntersectsRect(r, dirty)) continue;
			[fkAltRow() setFill];
			[[NSBezierPath bezierPathWithRoundedRect:r xRadius:6 yRadius:6] fill];
		}
		// Kopf mit Spaltentrennern und Sortierpfeil
		NSString *heads[] = { L("column.name"), L("column.date"), L("column.size"), L("column.kind") };
		fc_sort sorts[] = { FC_SORT_NAME, FC_SORT_DATE, FC_SORT_SIZE, FC_SORT_KIND };
		for (int c = 0; c < 4; c++) {
			CGFloat x0 = c ? [self col:c] : [self col:0] + 22, x1 = c < 3 ? [self col:c + 1] - 10 : W - 14;
			BOOL sorted = self.browser.sortBy == sorts[c];
			if (c == 2) drawText(heads[c], NSMakeRect(x0, 8, x1 - x0 - (sorted ? 16 : 0), 16), 12, sorted, sorted ? fkText() : fkSecondary(), NSRightTextAlignment);
			else drawText(heads[c], NSMakeRect(x0, 8, x1 - x0 - 16, 16), 12, sorted, sorted ? fkText() : fkSecondary(), NSLeftTextAlignment);
			if (sorted) {
				NSBezierPath *p = [NSBezierPath bezierPath];
				CGFloat ax = x1 - 8, ay = 16;
				CGFloat d = self.browser.sortDesc ? 1 : -1;
				[p moveToPoint:NSMakePoint(ax - 4, ay - 2 * d)]; [p lineToPoint:NSMakePoint(ax, ay + 2 * d)]; [p lineToPoint:NSMakePoint(ax + 4, ay - 2 * d)];
				[p setLineWidth:1.4]; [fkSecondary() setStroke]; [p stroke];
			}
			if (c) { [fkLine() setFill]; NSRectFill(NSMakeRect([self col:c] - 6, 7, 1, 16)); }
		}
		[fkLine() setFill];
		NSRectFill(NSMakeRect(0, LIST_HEAD - 1, W, 1));
	}
	for (NSInteger i = 0; i < (NSInteger)[self.items count]; i++) {
		NSRect r = [self rectOfItem:i];
		if (!NSIntersectsRect(r, dirty)) continue;
		FKItem *it = self.items[i];
		BOOL drop = i == self.dropIndex;
		BOOL sel = [self.selection containsIndex:i] || drop;
		CGFloat alpha = it.dimmed || it.hidden ? 0.45 : 1;
		BOOL key = drop || [[self window] isKeyWindow];
		NSColor *selFill = key ? accent() : fkPillSel();
		if (self.mode == FKViewModeIcons) {
			NSRect ir = NSMakeRect(NSMidX(r) - 32, r.origin.y + 6, 64, 64);
			if (sel) {
				[fkIconSel() setFill];
				[[NSBezierPath bezierPathWithRoundedRect:NSInsetRect(ir, -5, -5) xRadius:8 yRadius:8] fill];
			}
			if (!drawPreview(it, ir, alpha, NO)) {
				NSImage *img = iconFor(it.path, it.kind, NO);
				[img drawInRect:ir fromRect:NSZeroRect operation:NSCompositeSourceOver fraction:alpha respectFlipped:YES hints:nil];
			}
			if (i == self.editing) continue;
			NSDictionary *at = textAttrs(13, NO, sel && key ? [NSColor whiteColor] : (it.dimmed ? fkTertiary() : fkText()), NSCenterTextAlignment);
			NSSize ts = [it.name sizeWithAttributes:at];
			CGFloat ty = r.origin.y + 79;
			if (sel) {
				CGFloat tw = MIN(ts.width + 12, ICON_W - 6);
				[selFill setFill];
				[[NSBezierPath bezierPathWithRoundedRect:NSMakeRect(NSMidX(r) - tw / 2, ty - 1, tw, 19) xRadius:5 yRadius:5] fill];
			}
			[it.name drawInRect:NSMakeRect(r.origin.x + 6, ty, ICON_W - 12, 18) withAttributes:at];
		} else {
			NSRect rr = NSMakeRect(10, r.origin.y, W - 20, LIST_ROW);
			if (sel) { [selFill setFill]; [[NSBezierPath bezierPathWithRoundedRect:rr xRadius:6 yRadius:6] fill]; }
			NSColor *tc = sel && key ? [NSColor whiteColor] : (it.dimmed ? fkTertiary() : fkText());
			NSColor *sc = sel && key ? rgb(255, 255, 255, 0.85) : (it.dimmed ? fkTertiary() : fkSecondary());
			NSRect ib = NSMakeRect([self col:0] + 2, r.origin.y + 2, 16, 16);
			if (!drawPreview(it, ib, alpha, YES)) {
				NSImage *img = iconFor(it.path, it.kind, YES);
				[img drawInRect:ib fromRect:NSZeroRect operation:NSCompositeSourceOver fraction:alpha respectFlipped:YES hints:nil];
			}
			if (i != self.editing)
				drawText(it.name, NSMakeRect([self col:0] + 24, r.origin.y + 1, [self col:1] - [self col:0] - 34, 18), 13, NO, tc, NSLeftTextAlignment);
			drawText(dateString(it.mtime), NSMakeRect([self col:1], r.origin.y + 1, [self col:2] - [self col:1] - 10, 18), 13, NO, sc, NSLeftTextAlignment);
			drawText(it.isDir && it.kind == FC_FOLDER ? @"--" : sizeString(it.size), NSMakeRect([self col:2], r.origin.y + 1, [self col:3] - [self col:2] - 16, 18), 13, NO, sc, NSRightTextAlignment);
			drawText(FKLocalized([NSString stringWithUTF8String:fc_kind_key(it.kind)]), NSMakeRect([self col:3], r.origin.y + 1, W - [self col:3] - 20, 18), 13, NO, sc, NSLeftTextAlignment);
		}
	}
	if (self.dropOnView) {   // Ablage in den angezeigten Ordner: Rahmen wie im Finder
		NSBezierPath *b = [NSBezierPath bezierPathWithRoundedRect:NSInsetRect([self visibleRect], 3, 3) xRadius:8 yRadius:8];
		[b setLineWidth:3];
		[accent() setStroke];
		[b stroke];
	}
	if (![self.items count] && self.mode == FKViewModeIcons) {
		NSRect vis = [self visibleRect];
		drawText(L("status.empty"), NSMakeRect(0, NSMidY(vis) - 10, NSWidth(vis), 20), 13, NO, fkTertiary(), NSCenterTextAlignment);
	}
}
- (void)scrollToItem:(NSInteger)i { if (i >= 0) [self scrollRectToVisible:NSInsetRect([self rectOfItem:i], 0, -6)]; }
- (void)setSelectionTo:(NSIndexSet *)s {
	[self.selection removeAllIndexes];
	[self.selection addIndexes:s];
	[self setNeedsDisplay:YES];
	[self.browser selectionChanged];
}
- (void)fkMoveForQuickLook:(NSEvent *)e { [self keyDown:e]; }
- (void)mouseDown:(NSEvent *)e {
	[self fkCommitEditing];
	[[self window] makeFirstResponder:self];
	NSPoint p = [self convertPoint:[e locationInWindow] fromView:nil];
	if (self.mode == FKViewModeList && p.y < LIST_HEAD) {
		fc_sort s = p.x < [self col:1] ? FC_SORT_NAME : p.x < [self col:2] ? FC_SORT_DATE : p.x < [self col:3] ? FC_SORT_SIZE : FC_SORT_KIND;
		[self.browser sortByColumn:s];
		return;
	}
	NSInteger i = [self indexAtPoint:p];
	NSUInteger mods = [e modifierFlags];
	if (i < 0) { [self setSelectionTo:[NSIndexSet indexSet]]; return; }
	if ([e clickCount] >= 2 && !(mods & (NSShiftKeyMask | NSCommandKeyMask))) { [self.browser itemsDoubleClicked:i]; return; }
	NSMutableIndexSet *s = [self.selection mutableCopy];
	BOOL multi = self.browser.purpose == FKPurposeWindow || self.browser.allowsMultipleSelection;
	if ((mods & NSCommandKeyMask) && multi) {
		if ([s containsIndex:i]) [s removeIndex:i]; else [s addIndex:i];
		self.anchor = i;
	} else if ((mods & NSShiftKeyMask) && multi && self.anchor >= 0) {
		[s removeAllIndexes];
		[s addIndexesInRange:NSMakeRange(MIN(self.anchor, i), labs(self.anchor - i) + 1)];
	} else if ([s containsIndex:i] && [s count] > 1) {
		// Klick in eine Mehrfachauswahl: erst beim Loslassen ohne Ziehen nur diesen auswählen (Finder)
		[self setSelectionTo:s];
		if (![self fkTrackDrag:e grabbed:i]) { [s removeAllIndexes]; [s addIndex:i]; self.anchor = i; [self setSelectionTo:s]; }
		return;
	} else {
		BOOL rename = NO;
		if ([s containsIndex:i] && [s count] == 1 && [e clickCount] == 1 && self.browser.purpose == FKPurposeWindow &&
		    [NSDate timeIntervalSinceReferenceDate] - self.typeTime > 0.6) {
			// zweiter einfacher Klick auf den Namen eines schon ausgewählten Eintrags: umbenennen (wie im Finder)
			NSRect r = [self rectOfItem:i];
			rename = self.mode == FKViewModeIcons ? p.y > r.origin.y + 76 : (p.x > [self col:0] + 22 && p.x < [self col:1]);
		}
		[s removeAllIndexes];
		[s addIndex:i];
		self.anchor = i;
		[self setSelectionTo:s];
		if (![self fkTrackDrag:e grabbed:i] && rename) [self performSelector:@selector(beginEditingDelayed:) withObject:@(i) afterDelay:0.5];
		return;
	}
	[self setSelectionTo:s];
}
// Taste gedrückt auf einem ausgewählten Eintrag: ab 4 Punkten Weg wird gezogen. YES = es wurde gezogen.
- (BOOL)fkTrackDrag:(NSEvent *)down grabbed:(NSInteger)i {
	if (self.browser.purpose != FKPurposeWindow) return NO;
	NSPoint start = [down locationInWindow];
	for (;;) {
		NSEvent *ev = [[self window] nextEventMatchingMask:NSLeftMouseDraggedMask | NSLeftMouseUpMask untilDate:[NSDate distantFuture]
		                                             inMode:NSEventTrackingRunLoopMode dequeue:YES];
		if (!ev || [ev type] == NSLeftMouseUp) return NO;
		NSPoint q = [ev locationInWindow];
		if (fabs(q.x - start.x) + fabs(q.y - start.y) >= 4) {
			[NSApp preventWindowOrdering];   // Hintergrundfenster bleibt hinten (macOS), auch nach dem Loslassen
			fkRunDrag(self, ev, start, i);
			return YES;
		}
	}
}
- (void)beginEditingDelayed:(NSNumber *)n {
	if ([self.selection count] == 1 && [self.selection firstIndex] == [n unsignedIntegerValue]) [self beginEditing:[n integerValue]];
}
- (NSMenu *)menuForEvent:(NSEvent *)e {
	NSPoint p = [self convertPoint:[e locationInWindow] fromView:nil];
	NSInteger i = [self indexAtPoint:p];
	if (i >= 0 && ![self.selection containsIndex:i]) [self setSelectionTo:[NSIndexSet indexSetWithIndex:i]];
	if (i < 0) [self setSelectionTo:[NSIndexSet indexSet]];
	return [self.browser contextMenuForItem:i];
}
- (void)moveSelectionBy:(NSInteger)d extend:(BOOL)extend {
	NSInteger n = [self.items count];
	if (!n) return;
	NSInteger cur = [self.selection count] ? ([self.selection lastIndex] == (NSUInteger)self.anchor ? [self.selection firstIndex] : [self.selection lastIndex]) : -1;
	if (!extend) cur = [self.selection count] ? (d > 0 ? (NSInteger)[self.selection lastIndex] : (NSInteger)[self.selection firstIndex]) : -1;
	NSInteger next = cur < 0 ? 0 : MAX(0, MIN(n - 1, cur + d));
	if (extend && self.anchor >= 0) {
		NSMutableIndexSet *s = [NSMutableIndexSet indexSet];
		[s addIndexesInRange:NSMakeRange(MIN(self.anchor, next), labs(self.anchor - next) + 1)];
		[self setSelectionTo:s];
	} else {
		self.anchor = next;
		[self setSelectionTo:[NSIndexSet indexSetWithIndex:next]];
	}
	[self scrollToItem:next];
}
- (void)keyDown:(NSEvent *)e {
	NSString *c = [e charactersIgnoringModifiers];
	unichar k = [c length] ? [c characterAtIndex:0] : 0;
	NSUInteger mods = [e modifierFlags];
	BOOL cmd = (mods & NSCommandKeyMask) != 0, shift = (mods & NSShiftKeyMask) != 0;
	NSInteger cols = self.mode == FKViewModeIcons ? [self columns] : 1;
	switch (k) {
	case NSUpArrowFunctionKey:
		if (cmd) { [self.browser goUp]; return; }
		[self moveSelectionBy:-cols extend:shift]; return;
	case NSDownArrowFunctionKey:
		if (cmd) { [self.browser openSelection]; return; }
		[self moveSelectionBy:cols extend:shift]; return;
	case NSLeftArrowFunctionKey: if (self.mode == FKViewModeIcons) [self moveSelectionBy:-1 extend:shift]; return;
	case NSRightArrowFunctionKey: if (self.mode == FKViewModeIcons) [self moveSelectionBy:1 extend:shift]; return;
	case NSHomeFunctionKey: [self moveSelectionBy:-(NSInteger)[self.items count] extend:NO]; return;
	case NSEndFunctionKey: [self moveSelectionBy:[self.items count] extend:NO]; return;
	case '\r': case 3:
		if (self.browser.purpose == FKPurposeWindow) [self.browser renameSelection]; else [self.browser openSelection];
		return;
	case 127: case NSDeleteFunctionKey:
		if (cmd && self.browser.purpose == FKPurposeWindow) [self.browser trashSelection];
		return;
	case 27: if ([self.browser quickLookVisible]) [self.browser toggleQuickLook]; else [self setSelectionTo:[NSIndexSet indexSet]]; return;
	case ' ': [self.browser toggleQuickLook]; return;
	}
	if (cmd || !k || k >= 0xF700) { [super keyDown:e]; return; }
	// Tippen springt zum ersten passenden Namen
	NSTimeInterval now = [NSDate timeIntervalSinceReferenceDate];
	self.typeAhead = now - self.typeTime < 1.0 ? [self.typeAhead stringByAppendingString:c] : c;
	self.typeTime = now;
	for (NSInteger i = 0; i < (NSInteger)[self.items count]; i++) {
		if ([[self.items[i] name] rangeOfString:self.typeAhead options:NSCaseInsensitiveSearch | NSAnchoredSearch].location != NSNotFound) {
			self.anchor = i;
			[self setSelectionTo:[NSIndexSet indexSetWithIndex:i]];
			[self scrollToItem:i];
			return;
		}
	}
}
// Umbenennen in einem Textfeld über dem Namen
- (void)beginEditing:(NSInteger)i {
	if (i < 0 || i >= (NSInteger)[self.items count]) return;
	[self fkCommitEditing];
	FKItem *it = self.items[i];
	NSRect r = [self rectOfItem:i];
	NSRect fr = self.mode == FKViewModeIcons ? NSMakeRect(r.origin.x + 2, r.origin.y + 77, ICON_W - 4, 21)
	                                         : NSMakeRect([self col:0] + 22, r.origin.y, [self col:1] - [self col:0] - 26, LIST_ROW);
	NSTextField *f = [[NSTextField alloc] initWithFrame:fr];
	NSString *real = [it.path lastPathComponent];   // umbenannt wird der echte Dateiname, nicht der übersetzte
	[f setStringValue:real];
	[f setFont:[NSFont systemFontOfSize:13]];
	[f setAlignment:self.mode == FKViewModeIcons ? NSCenterTextAlignment : NSLeftTextAlignment];
	[f setDelegate:self];
	[f setTarget:self];
	[f setAction:@selector(editorAction:)];
	self.editor = f;
	self.editing = i;
	[self addSubview:f];
	[[self window] makeFirstResponder:f];
	// Name ohne Endung auswählen, wie der Finder
	NSText *ed = [[self window] fieldEditor:YES forObject:f];
	NSString *ext = [real pathExtension];
	NSUInteger len = [real length] - ([ext length] && !it.isDir ? [ext length] + 1 : 0);
	if ([ed respondsToSelector:@selector(setSelectedRange:)]) [ed setSelectedRange:NSMakeRange(0, len)];
	[self setNeedsDisplay:YES];
}
- (void)editorAction:(id)sender { [self fkCommitEditing]; [[self window] makeFirstResponder:self]; }
- (void)controlTextDidEndEditing:(NSNotification *)n { [self fkCommitEditing]; }
- (BOOL)control:(NSControl *)c textView:(NSTextView *)tv doCommandBySelector:(SEL)sel {
	if (sel == @selector(cancelOperation:)) { [self fkCancelEditing]; return YES; }
	return NO;
}
- (void)fkCancelEditing {
	if (!self.editor) return;
	[self.editor removeFromSuperview];
	self.editor = nil;
	self.editing = -1;
	[[self window] makeFirstResponder:self];
	[self setNeedsDisplay:YES];
}
- (void)fkCommitEditing {
	if (!self.editor || self.editing < 0) return;
	NSTextField *f = self.editor;
	NSInteger i = self.editing;
	self.editor = nil;
	self.editing = -1;
	NSString *name = [f stringValue];
	[f removeFromSuperview];
	FKItem *it = i < (NSInteger)[self.items count] ? self.items[i] : nil;
	if (it && [name length] && ![name isEqualToString:[it.path lastPathComponent]]) {
		char out[PATH_MAX];
		int r = fc_rename([it.path UTF8String], [name UTF8String], out, sizeof out);
		if (r) NSBeep();
		[self.browser reload];
		if (!r) [self.browser selectPath:[NSString stringWithUTF8String:out]];
	}
	[self setNeedsDisplay:YES];
}
@end


// ------------------------------------------------------------------------------------------------ Quick Look
// Leertaste: Vorschau des ausgewählten Objekts (Bild, PDF, Text/Code, sonst großes Icon mit Infos). Pfeiltasten
// blättern weiter (gehen an die Ansicht), Leertaste oder Esc schließen. Ohne eigene Fensterleiste.
@interface FKQuickLookView : NSView
@property (weak) FKItemsView *items;
@property (strong) FKItem *item;
@property (strong) NSImage *image;
@property (strong) NSScrollView *textScroll;
@end

@implementation FKQuickLookView
- (BOOL)isFlipped { return YES; }
- (BOOL)acceptsFirstResponder { return YES; }
- (void)showItem:(FKItem *)it {
	self.item = it;
	self.image = hasImagePreview(it) ? loadPreviewImage(it.path) : nil;
	[self.textScroll removeFromSuperview];
	self.textScroll = nil;
	NSString *txt = !self.image && hasTextPreview(it) ? textSnippet(it, 256 * 1024) : nil;
	if (txt) {
		NSRect b = NSMakeRect(0, 44, NSWidth([self bounds]), NSHeight([self bounds]) - 44);
		NSScrollView *sv = [[NSScrollView alloc] initWithFrame:b];
		[sv setHasVerticalScroller:YES];
		[sv setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
		NSTextView *tv = [[NSTextView alloc] initWithFrame:NSMakeRect(0, 0, NSWidth(b), NSHeight(b))];
		[tv setEditable:NO];
		[tv setSelectable:YES];
		[tv setTextContainerInset:NSMakeSize(14, 12)];
		NSFont *f = it.kind == FC_CODE ? [NSFont userFixedPitchFontOfSize:12] : [NSFont systemFontOfSize:13];
		[tv setFont:f ? f : [NSFont systemFontOfSize:13]];
		[tv setString:txt];
		[sv setDocumentView:tv];
		[self addSubview:sv];
		self.textScroll = sv;
	}
	[[self window] setTitle:it.name];
	[self setNeedsDisplay:YES];
}
- (void)drawRect:(NSRect)dirty {
	NSRect b = [self bounds];
	[rgb(250, 250, 252, 1) setFill];
	NSRectFill(b);
	[rgb(0, 0, 0, 0.08) setFill];
	NSRectFill(NSMakeRect(0, 43, NSWidth(b), 1));
	if (!self.item) return;
	drawText(self.item.name, NSMakeRect(90, 13, NSWidth(b) - 180, 18), 13, YES, rgb(29, 29, 31, 1), NSCenterTextAlignment);
	if (self.textScroll) return;
	NSRect body = NSMakeRect(0, 44, NSWidth(b), NSHeight(b) - 44);
	if (self.image) {
		NSRect r = fitRect([self.image size], NSInsetRect(body, 20, 20));
		if (NSWidth(r) > [self.image size].width && NSHeight(r) > [self.image size].height)
			r = fitRect([self.image size], NSMakeRect(NSMidX(body) - [self.image size].width / 2, NSMidY(body) - [self.image size].height / 2,
			                                          [self.image size].width, [self.image size].height));
		[self.image drawInRect:r fromRect:NSZeroRect operation:NSCompositeSourceOver fraction:1 respectFlipped:YES hints:nil];
		return;
	}
	// Infos: großes Icon, Name, Art, Größe, Änderungsdatum
	NSImage *ic = iconFor(self.item.path, self.item.kind, NO);
	CGFloat cy = NSMidY(body) - 70;
	[ic drawInRect:NSMakeRect(NSMidX(body) - 64, cy - 40, 128, 128) fromRect:NSZeroRect operation:NSCompositeSourceOver fraction:1 respectFlipped:YES hints:nil];
	drawText(self.item.name, NSMakeRect(20, cy + 100, NSWidth(b) - 40, 24), 18, YES, rgb(29, 29, 31, 1), NSCenterTextAlignment);
	NSString *kind = FKLocalized([NSString stringWithUTF8String:fc_kind_key(self.item.kind)]);
	NSString *info = self.item.isDir && self.item.kind == FC_FOLDER ? kind : [NSString stringWithFormat:@"%@ – %@", kind, sizeString(self.item.size)];
	drawText(info, NSMakeRect(20, cy + 130, NSWidth(b) - 40, 18), 13, NO, rgb(60, 60, 67, 0.75), NSCenterTextAlignment);
	drawText(dateString(self.item.mtime), NSMakeRect(20, cy + 150, NSWidth(b) - 40, 18), 12, NO, rgb(60, 60, 67, 0.55), NSCenterTextAlignment);
}
- (void)keyDown:(NSEvent *)e {
	NSString *c = [e charactersIgnoringModifiers];
	unichar k = [c length] ? [c characterAtIndex:0] : 0;
	if (k == ' ' || k == 27) { [self.items.browser toggleQuickLook]; return; }
	if (k == NSUpArrowFunctionKey || k == NSDownArrowFunctionKey || k == NSLeftArrowFunctionKey || k == NSRightArrowFunctionKey) {
		[self.items fkMoveForQuickLook:e];
		return;
	}
	[super keyDown:e];
}
@end

// ------------------------------------------------------------------------------------------------ Browser
@implementation FKBrowserView

- (BOOL)isFlipped { return YES; }

- (instancetype)initWithFrame:(NSRect)frame purpose:(FKPurpose)purpose {
	if (!(self = [super initWithFrame:frame])) return nil;
	_purpose = purpose;
	_canChooseFiles = YES;
	_history = [NSMutableArray array];
	_historyPos = -1;
	_sortBy = FC_SORT_NAME;
	[self setAutoresizesSubviews:NO];

	TR("b0");
	_header = [[FKHeaderView alloc] initWithFrame:NSZeroRect];
	[self addSubview:_header];
	_backButton = [self button:FKGlyphBack action:@selector(goBack)];
	_forwardButton = [self button:FKGlyphForward action:@selector(goForward)];
	_iconsButton = [self button:FKGlyphIcons action:@selector(showIcons)];
	_listButton = [self button:FKGlyphList action:@selector(showList)];
	_folderButton = [self button:FKGlyphNewFolder action:@selector(newFolder)];
	_searchButton = [self button:FKGlyphSearch action:@selector(openSearch)];
	_titleLabel = [[NSTextField alloc] initWithFrame:NSZeroRect];
	[_titleLabel setEditable:NO]; [_titleLabel setSelectable:NO]; [_titleLabel setBordered:NO]; [_titleLabel setDrawsBackground:NO];
	[_titleLabel setFont:[NSFont boldSystemFontOfSize:15]];
	[_titleLabel setTextColor:fkText()];
	[_header addSubview:_titleLabel];
	_search = [[NSTextField alloc] initWithFrame:NSZeroRect];
	[[_search cell] setPlaceholderString:L("search.placeholder")];
	[_search setFont:[NSFont systemFontOfSize:13]];
	[_search setBordered:NO];
	[_search setDrawsBackground:NO];
	[_search setFocusRingType:NSFocusRingTypeNone];
	[_search setTextColor:fkText()];
	[_search setDelegate:self];
	[_search setHidden:YES];
	[_header addSubview:_search];

	TR("b1 header");
	_sidebar = [[FKSidebarView alloc] initWithFrame:NSZeroRect];
	_sidebar.browser = self;
	[self addSubview:_sidebar];
	_scroll = [[NSScrollView alloc] initWithFrame:NSZeroRect];
	[_scroll setHasVerticalScroller:YES];
	[_scroll setBorderType:NSNoBorder];
	[_scroll setDrawsBackground:YES];
	[_scroll setBackgroundColor:fkContent()];
	_itemsView = [[FKItemsView alloc] initWithFrame:NSMakeRect(0, 0, 400, 300)];
	_itemsView.browser = self;
	[_scroll setDocumentView:_itemsView];
	[self addSubview:_scroll];
	_pathBar = [[FKPathBar alloc] initWithFrame:NSZeroRect];
	_pathBar.browser = self;
	[self addSubview:_pathBar];
	_statusBar = [[FKPathBar alloc] initWithFrame:NSZeroRect];
	_statusBar.browser = self;
	_statusBar.statusOnly = YES;
	[self addSubview:_statusBar];
	TR("b2 views");
	[_sidebar rebuild];
	TR("b3 sidebar");
	[self layoutViews];
	TR("b4 layout");
	return self;
}

- (instancetype)initWithFrame:(NSRect)frame { return [self initWithFrame:frame purpose:FKPurposeWindow]; }

- (FKToolButton *)button:(FKGlyph)g action:(SEL)a {
	FKToolButton *b = [[FKToolButton alloc] initWithFrame:NSZeroRect];
	b.glyph = g; b.enabled = YES; b.target = self; b.action = a;
	[_header addSubview:b];
	return b;
}

- (void)setFrameSize:(NSSize)s { [super setFrameSize:s]; [self layoutViews]; }
- (void)resizeSubviewsWithOldSize:(NSSize)old { [self layoutViews]; }

- (void)layoutViews {
	NSRect b = [self bounds];
	CGFloat W = NSWidth(b), H = NSHeight(b), side = W > 600 ? 165 : 150, head = 52;
	CGFloat pathH = _showsPathBar ? 24 : 0, statH = _showsStatusBar ? 22 : 0, foot = pathH + statH;
	CGFloat hw = W - side;
	[_header setFrame:NSMakeRect(side, 0, hw, head)];
	// links: Zurück/Vor in einer Kapsel, daneben der Ordnername
	[_backButton setFrame:NSMakeRect(14, 9, 36, 34)];
	[_forwardButton setFrame:NSMakeRect(50, 9, 36, 34)];
	// rechts: Suche (rund), Neuer Ordner, Darstellung (Kapsel mit Auswahl)
	CGFloat x = hw - 14;
	NSMutableArray *pills = [NSMutableArray arrayWithObject:[NSValue valueWithRect:NSMakeRect(14, 9, 72, 34)]];
	if (_searchOpen) {
		NSRect sr = NSMakeRect(x - 200, 9, 200, 34);
		[pills addObject:[NSValue valueWithRect:sr]];
		[_searchButton setFrame:NSMakeRect(NSMinX(sr) + 2, 9, 32, 34)];
		[_search setFrame:NSMakeRect(NSMinX(sr) + 32, 17, 160, 20)];
		x = NSMinX(sr) - 10;
	} else {
		[_searchButton setFrame:NSMakeRect(x - 34, 9, 34, 34)];
		[pills addObject:[NSValue valueWithRect:NSMakeRect(x - 34, 9, 34, 34)]];
		x -= 44;
	}
	[_search setHidden:!_searchOpen];
	if (_purpose != FKPurposeOpen) {
		[_folderButton setFrame:NSMakeRect(x - 40, 9, 40, 34)];
		[pills addObject:[NSValue valueWithRect:NSMakeRect(x - 40, 9, 40, 34)]];
		x -= 50;
	}
	[_folderButton setHidden:_purpose == FKPurposeOpen];
	[_listButton setFrame:NSMakeRect(x - 40, 9, 40, 34)];
	[_iconsButton setFrame:NSMakeRect(x - 80, 9, 40, 34)];
	[pills addObject:[NSValue valueWithRect:NSMakeRect(x - 80, 9, 80, 34)]];
	_header.pills = pills;
	[_titleLabel setFrame:NSMakeRect(98, 15, MAX(40, x - 80 - 98 - 10), 22)];
	[_sidebar setFrame:NSMakeRect(0, 0, side, H - foot)];
	[_scroll setFrame:NSMakeRect(side, head, hw, H - head - foot)];
	[_pathBar setHidden:!_showsPathBar];
	[_statusBar setHidden:!_showsStatusBar];
	[_pathBar setFrame:NSMakeRect(0, H - foot, W, pathH)];
	[_statusBar setFrame:NSMakeRect(0, H - statH, W, statH)];
	[_itemsView layoutForWidth:[_scroll contentSize].width];
	[_header setNeedsDisplay:YES];
	[self setNeedsDisplay:YES];
}

- (void)setShowsPathBar:(BOOL)v { _showsPathBar = v; [self layoutViews]; }
- (void)setShowsStatusBar:(BOOL)v { _showsStatusBar = v; [self layoutViews]; }

- (void)openSearch {
	if (_searchOpen) { [[self window] makeFirstResponder:_search]; return; }
	_searchOpen = YES;
	[self layoutViews];
	[[self window] makeFirstResponder:_search];
}
- (void)closeSearch {
	if (!_searchOpen) return;
	_searchOpen = NO;
	[_search setStringValue:@""];
	[self layoutViews];
	[self reload];
	[[self window] makeFirstResponder:_itemsView];
}
- (void)controlTextDidEndEditing:(NSNotification *)n {
	if ([n object] == _search && ![[_search stringValue] length]) [self closeSearch];
}

- (void)drawRect:(NSRect)dirty {
	[fkContent() setFill];
	NSRectFill(dirty);
}

- (void)updateButtons {
	_backButton.enabled = [self canGoBack];
	_forwardButton.enabled = [self canGoForward];
	_iconsButton.selected = _viewMode == FKViewModeIcons;
	_listButton.selected = _viewMode == FKViewModeList;
	for (NSView *v in @[ _backButton, _forwardButton, _iconsButton, _listButton ]) [v setNeedsDisplay:YES];
}

- (NSString *)title { return nameOf(_directory ? _directory : @"/"); }

- (void)showDirectory:(NSString *)path {
	BOOL isDir = NO;
	if (!fkExists(path, &isDir) || !isDir) { NSBeep(); return; }
	self.directory = [path stringByStandardizingPath];
	[_search setStringValue:@""];
	[self reload];
	[_itemsView scrollPoint:NSZeroPoint];
	[_titleLabel setStringValue:[self title]];
	_sidebar.current = self.directory;
	[_sidebar setNeedsDisplay:YES];
	_pathBar.path = self.directory;
	[self updateButtons];
	if ([_delegate respondsToSelector:@selector(browserDidChangeDirectory:)]) [_delegate browserDidChangeDirectory:self];
}

- (void)goTo:(NSString *)path {
	path = [path stringByStandardizingPath];
	if ([path isEqualToString:_directory]) return;
	if (_historyPos + 1 < (NSInteger)[_history count]) [_history removeObjectsInRange:NSMakeRange(_historyPos + 1, [_history count] - _historyPos - 1)];
	[_history addObject:path];
	_historyPos = [_history count] - 1;
	[self showDirectory:path];
}
- (BOOL)canGoBack { return _historyPos > 0; }
- (BOOL)canGoForward { return _historyPos + 1 < (NSInteger)[_history count]; }
- (void)goBack { if ([self canGoBack]) { NSString *from = _directory; _historyPos--; [self showDirectory:_history[_historyPos]]; [self selectPath:from]; } }
- (void)goForward { if ([self canGoForward]) { _historyPos++; [self showDirectory:_history[_historyPos]]; } }
- (void)goUp {
	if (!_directory || [_directory isEqualToString:@"/"]) return;
	NSString *from = _directory;
	[self goTo:[_directory stringByDeletingLastPathComponent]];
	[self selectPath:from];
}

- (void)setViewMode:(FKViewMode)m {
	_viewMode = m;
	_itemsView.mode = m;
	[_itemsView layoutForWidth:[_scroll contentSize].width];
	[self updateButtons];
}
- (void)showIcons { [self setViewMode:FKViewModeIcons]; }
- (void)showList { [self setViewMode:FKViewModeList]; }
- (void)setShowHidden:(BOOL)h { _showHidden = h; [self reload]; }
- (void)sortByColumn:(fc_sort)s {
	if (_sortBy == s) _sortDesc = !_sortDesc; else { _sortBy = s; _sortDesc = s == FC_SORT_DATE || s == FC_SORT_SIZE; }
	[self reload];
}

- (BOOL)allowed:(FKItem *)it {
	if (_purpose == FKPurposeWindow) return YES;
	if (it.isDir && it.kind == FC_FOLDER) return YES;            // Ordner bleiben navigierbar
	if (it.isDir && !_canChooseDirectories && it.kind != FC_PACKAGE && it.kind != FC_APP) return YES;
	if (!_canChooseFiles && !it.isDir) return NO;
	if (![_allowedExtensions count]) return YES;
	NSString *e = [[it.name pathExtension] lowercaseString];
	for (NSString *a in _allowedExtensions) if ([[a lowercaseString] isEqualToString:e]) return YES;
	return NO;
}

- (void)reload {
	if (!_directory) return;
	NSArray *before = [self selectedPaths];
	fc_list l;
	if (fc_read_dir([_directory UTF8String], _showHidden, &l)) { l.v = NULL; l.n = 0; }
	fc_filter(&l, [[_search stringValue] UTF8String]);
	fc_sort_list(&l, _sortBy, _sortDesc, 1);
	NSMutableArray *items = [NSMutableArray arrayWithCapacity:l.n];
	for (int i = 0; i < l.n; i++) {
		FKItem *it = [[FKItem alloc] init];
		it.path = [NSString stringWithUTF8String:l.v[i].path];
		it.name = it.path ? nameOf(it.path) : nil;
		if (!it.name || !it.path) continue;
		it.kind = l.v[i].kind; it.isDir = l.v[i].is_dir; it.hidden = l.v[i].is_hidden;
		it.size = l.v[i].size; it.mtime = l.v[i].mtime;
		it.dimmed = ![self allowed:it];
		[items addObject:it];
	}
	fc_list_free(&l);
	_itemsView.items = items;
	[_itemsView.selection removeAllIndexes];
	for (NSString *p in before) [self selectPath:p];
	[_itemsView layoutForWidth:[_scroll contentSize].width];
	[self updateStatus];
}

- (void)updateStatus {
	int64_t fr = 0;
	fc_free_space([_directory UTF8String], &fr, NULL);
	NSUInteger n = [_itemsView.items count], s = [_itemsView.selection count];
	NSString *items = n == 1 ? L("status.item") : [NSString stringWithFormat:L("status.items"), (unsigned long)n];
	if (s) items = [NSString stringWithFormat:L("status.selected"), (unsigned long)s, (unsigned long)n];
	_statusBar.status = [NSString stringWithFormat:L("status.available"), items, sizeString(fr)];
	[_statusBar setNeedsDisplay:YES];
}

- (void)controlTextDidChange:(NSNotification *)n { if ([n object] == _search) [self reload]; }

- (NSArray *)selectedPaths {
	NSMutableArray *a = [NSMutableArray array];
	NSUInteger i = [_itemsView.selection firstIndex];
	while (i != NSNotFound) {
		if (i < [_itemsView.items count]) [a addObject:[_itemsView.items[i] path]];
		i = [_itemsView.selection indexGreaterThanIndex:i];
	}
	return a;
}
- (void)selectPath:(NSString *)path {
	for (NSUInteger i = 0; i < [_itemsView.items count]; i++)
		if ([[_itemsView.items[i] path] isEqualToString:path]) {
			[_itemsView.selection addIndex:i];
			_itemsView.anchor = i;
			[_itemsView scrollToItem:i];
		}
	[_itemsView setNeedsDisplay:YES];
	[self selectionChanged];
}
static NSWindow *gQLWindow;
static FKQuickLookView *gQLView;

- (BOOL)quickLookVisible { return gQLWindow && [gQLWindow isVisible]; }

- (void)toggleQuickLook {
	if ([self quickLookVisible]) {
		[gQLWindow orderOut:nil];
		[[self window] makeKeyAndOrderFront:nil];
		[[self window] makeFirstResponder:_itemsView];
		return;
	}
	NSArray *sel = [_itemsView.items objectsAtIndexes:_itemsView.selection];
	if (![sel count]) { NSBeep(); return; }
	if (!gQLWindow) {
		NSRect wr = [[self window] frame];
		gQLWindow = [[NSWindow alloc] initWithContentRect:NSMakeRect(NSMidX(wr) - 330, NSMidY(wr) - 230, 660, 500)
		                                        styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskResizable
		                                          backing:NSBackingStoreBuffered defer:NO];
		FKHideTitlebar(gQLWindow);
		[gQLWindow setReleasedWhenClosed:NO];
		gQLView = [[FKQuickLookView alloc] initWithFrame:[[gQLWindow contentView] bounds]];
		[gQLView setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
		[[gQLWindow contentView] addSubview:gQLView];
	}
	gQLView.items = _itemsView;
	[gQLView showItem:[sel lastObject]];
	[gQLWindow makeKeyAndOrderFront:nil];
	[gQLWindow makeFirstResponder:gQLView];
}

- (void)selectionChanged {
	if ([self quickLookVisible] && [_itemsView.selection count])
		[gQLView showItem:_itemsView.items[[_itemsView.selection lastIndex]]];
	[self updateStatus];
	if ([_delegate respondsToSelector:@selector(browserSelectionDidChange:)]) [_delegate browserSelectionDidChange:self];
}
- (void)selectAll {
	if (_purpose != FKPurposeWindow && !_allowsMultipleSelection) return;
	[_itemsView setSelectionTo:[NSIndexSet indexSetWithIndexesInRange:NSMakeRange(0, [_itemsView.items count])]];
}

- (void)openPath:(FKItem *)it {
	if (it.isDir && (it.kind == FC_FOLDER || (_purpose != FKPurposeWindow && it.kind == FC_PACKAGE && !_canChooseDirectories))) {
		[self goTo:it.path];
		return;
	}
	if (it.dimmed) { NSBeep(); return; }
	if ([_delegate respondsToSelector:@selector(browser:openFile:)] && [_delegate browser:self openFile:it.path]) return;
	if (fc_open([it.path UTF8String], NULL)) NSBeep();
}
- (void)itemsDoubleClicked:(NSInteger)i { if (i >= 0 && i < (NSInteger)[_itemsView.items count]) [self openPath:_itemsView.items[i]]; }
- (void)openSelection {
	NSArray *sel = [[_itemsView.items objectsAtIndexes:_itemsView.selection] copy];
	if ([sel count] == 1) { [self openPath:sel[0]]; return; }
	for (FKItem *it in sel) if (!it.isDir || it.kind != FC_FOLDER) fc_open([it.path UTF8String], NULL);
}

- (void)newFolder {
	if (!_directory) return;
	char out[PATH_MAX];
	if (fc_mkdir_unique([_directory UTF8String], [L("newfolder.name") UTF8String], out, sizeof out)) { NSBeep(); return; }
	[self reload];
	NSString *p = [NSString stringWithUTF8String:out];
	[_itemsView.selection removeAllIndexes];
	[self selectPath:p];
	[_itemsView beginEditing:[_itemsView.selection firstIndex]];
}
- (void)renameSelection { if ([_itemsView.selection count] == 1) [_itemsView beginEditing:[_itemsView.selection firstIndex]]; }
- (void)trashSelection {
	NSArray *paths = [self selectedPaths];
	if (![paths count]) return;
	BOOL inTrash = [_directory isEqualToString:[NSString stringWithUTF8String:fc_special_path(FC_TRASH)]];
	for (NSString *p in paths) if (inTrash ? 0 : fc_trash([p UTF8String])) NSBeep();
	[self reload];
	[_sidebar setNeedsDisplay:YES];
}
- (void)emptyTrash { fc_empty_trash(); [self reload]; }
- (void)duplicateSelection {
	for (NSString *p in [self selectedPaths]) {
		char out[PATH_MAX];
		if (fc_copy([p UTF8String], [[p stringByDeletingLastPathComponent] UTF8String], out, sizeof out)) NSBeep();
	}
	[self reload];
}
- (void)copySelection { gClipboard = [[self selectedPaths] mutableCopy]; gClipboardCut = NO; }
- (void)cutSelection { gClipboard = [[self selectedPaths] mutableCopy]; gClipboardCut = YES; }
- (void)paste {
	if (![gClipboard count] || !_directory) return;
	[_itemsView.selection removeAllIndexes];
	NSMutableArray *done = [NSMutableArray array];
	for (NSString *p in gClipboard) {
		char out[PATH_MAX];
		int r = gClipboardCut ? fc_move([p UTF8String], [_directory UTF8String], out, sizeof out)
		                      : fc_copy([p UTF8String], [_directory UTF8String], out, sizeof out);
		if (r) NSBeep(); else [done addObject:[NSString stringWithUTF8String:out]];
	}
	if (gClipboardCut) gClipboard = nil;
	[self reload];
	for (NSString *p in done) [self selectPath:p];
}
- (void)focusSearch { [self openSearch]; }
- (void)openInTerminal {
	NSString *dir = _directory;
	NSArray *sel = [self selectedPaths];
	if ([sel count] == 1) { BOOL d = NO; if (fkExists(sel[0], &d) && d) dir = sel[0]; }
	if (!dir) return;
	NSTask *t = [[NSTask alloc] init];
	[t setLaunchPath:@"/bin/sh"];
	[t setArguments:@[ @"-c", @"cd \"$1\" && exec xfce4-terminal --working-directory=\"$1\"", @"sh", dir ]];
	@try { [t launch]; } @catch (NSException *e) { NSBeep(); }
}

- (NSMenu *)contextMenuForItem:(NSInteger)index {
	NSMenu *m = [[NSMenu alloc] initWithTitle:@""];
	if (_purpose != FKPurposeWindow) {
		[[m addItemWithTitle:L("menu.newfolder") action:@selector(newFolder) keyEquivalent:@""] setTarget:self];
		return m;
	}
	if (index >= 0) {
		[[m addItemWithTitle:L("menu.open") action:@selector(openSelection) keyEquivalent:@""] setTarget:self];
		[m addItem:[NSMenuItem separatorItem]];
		[[m addItemWithTitle:L("menu.trash") action:@selector(trashSelection) keyEquivalent:@""] setTarget:self];
		[m addItem:[NSMenuItem separatorItem]];
		[[m addItemWithTitle:L("menu.rename") action:@selector(renameSelection) keyEquivalent:@""] setTarget:self];
		[[m addItemWithTitle:L("menu.duplicate") action:@selector(duplicateSelection) keyEquivalent:@""] setTarget:self];
		[[m addItemWithTitle:L("menu.copy") action:@selector(copySelection) keyEquivalent:@""] setTarget:self];
		[m addItem:[NSMenuItem separatorItem]];
		[[m addItemWithTitle:L("menu.terminal") action:@selector(openInTerminal) keyEquivalent:@""] setTarget:self];
	} else {
		[[m addItemWithTitle:L("menu.newfolder") action:@selector(newFolder) keyEquivalent:@""] setTarget:self];
		if ([gClipboard count]) [[m addItemWithTitle:L("menu.paste") action:@selector(paste) keyEquivalent:@""] setTarget:self];
		[m addItem:[NSMenuItem separatorItem]];
		[[m addItemWithTitle:_viewMode == FKViewModeIcons ? L("menu.aslist") : L("menu.asicons")
		              action:_viewMode == FKViewModeIcons ? @selector(showList) : @selector(showIcons) keyEquivalent:@""] setTarget:self];
		[[m addItemWithTitle:L("menu.terminal") action:@selector(openInTerminal) keyEquivalent:@""] setTarget:self];
		if ([_directory isEqualToString:[NSString stringWithUTF8String:fc_special_path(FC_TRASH)]])
			[[m addItemWithTitle:L("menu.emptytrash") action:@selector(emptyTrash) keyEquivalent:@""] setTarget:self];
	}
	return m;
}
@end

// ------------------------------------------------------------------------------------------------ Öffnen/Sichern
@interface FKPanelController : NSObject <FKBrowserDelegate, NSWindowDelegate>
@property (strong) NSWindow *window;
@property (strong) FKBrowserView *browser;
@property (strong) NSTextField *nameField;
@property (strong) NSButton *okButton;
@property BOOL save;
@property (strong) NSArray *types;
@property (strong) NSArray *result;
@end

@implementation FKPanelController
- (NSString *)savePath {
	NSString *name = [[self.nameField stringValue] stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
	if (![name length]) return nil;
	if ([self.types count] && ![[name pathExtension] length]) name = [name stringByAppendingPathExtension:self.types[0]];
	return [self.browser.directory stringByAppendingPathComponent:name];
}
- (void)ok:(id)sender {
	if (self.save) {
		NSString *p = [self savePath];
		if (!p) { NSBeep(); return; }
		self.result = @[ p ];
	} else {
		NSArray *sel = [self.browser selectedPaths];
		if (![sel count] && self.browser.canChooseDirectories) sel = @[ self.browser.directory ];
		if ([sel count] == 1) {
			BOOL d = NO;
			fkExists(sel[0], &d);
			if (d && !self.browser.canChooseDirectories && fc_kind_of([sel[0] UTF8String], 1) == FC_FOLDER) {
				[self.browser goTo:sel[0]];   // Ordner öffnen statt wählen
				return;
			}
		}
		if (![sel count]) { NSBeep(); return; }
		self.result = sel;
	}
	[NSApp stopModalWithCode:NSModalResponseOK];
}
- (void)cancel:(id)sender { self.result = nil; [NSApp stopModalWithCode:NSModalResponseCancel]; }
- (BOOL)windowShouldClose:(id)sender { [self cancel:sender]; return YES; }
- (BOOL)browser:(FKBrowserView *)b openFile:(NSString *)path {
	if (self.save) { [self.nameField setStringValue:[path lastPathComponent]]; [self ok:nil]; return YES; }
	self.result = @[ path ];
	[NSApp stopModalWithCode:NSModalResponseOK];
	return YES;
}
- (void)browserSelectionDidChange:(FKBrowserView *)b {
	if (!self.save) return;
	NSArray *sel = [b selectedPaths];
	BOOL d = NO;
	if ([sel count] == 1 && fkExists(sel[0], &d) && !d)
		[self.nameField setStringValue:[sel[0] lastPathComponent]];
}
@end

@implementation FKPanel
+ (NSArray *)fk_runPanelWithOptions:(NSDictionary *)o {
	FKPanelController *c = [[FKPanelController alloc] init];
	c.save = [o[@"purpose"] isEqualToString:@"save"];
	c.types = o[@"types"];
	NSView *acc = o[@"accessory"];
	if (acc) {   // manche Zusatzansichten (Logic) melden einen kleineren Rahmen als ihre Unteransichten belegen
		NSSize sz = [acc frame].size;
		for (NSView *v in [acc subviews]) { sz.width = MAX(sz.width, NSMaxX([v frame])); sz.height = MAX(sz.height, NSMaxY([v frame])); }
		[acc setFrameSize:sz];
	}
	CGFloat accH = acc ? NSHeight([acc frame]) + 8 : 0;
	CGFloat W = 820, H = 520 + accH;
	NSWindow *w = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, W, H)
	                                          styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskResizable |
	                                                    NSWindowStyleMaskFullSizeContentView
	                                            backing:NSBackingStoreBuffered defer:NO];
	FKHideTitlebar(w);
	[w setTitle:[o[@"title"] length] ? o[@"title"] : (c.save ? L("panel.save") : L("panel.open"))];
	[w setReleasedWhenClosed:NO];
	[w setDelegate:c];
	c.window = w;
	NSView *content = [w contentView];
	CGFloat bottom = 52 + accH;
	CGFloat top = 0;
	if (c.save) {   // "Sichern unter:" unten links neben den Knöpfen (oben liegt die Ampel)
		NSTextField *lab = [[NSTextField alloc] initWithFrame:NSMakeRect(16, 17, 100, 20)];
		[lab setStringValue:L("panel.saveas")]; [lab setEditable:NO]; [lab setBordered:NO]; [lab setDrawsBackground:NO];
		[lab setAlignment:NSRightTextAlignment];
		[content addSubview:lab];
		c.nameField = [[NSTextField alloc] initWithFrame:NSMakeRect(122, 14, W - 122 - 250, 24)];
		[c.nameField setStringValue:[o[@"name"] length] ? o[@"name"] : L("panel.untitled")];
		[content addSubview:c.nameField];
	}
	c.browser = [[FKBrowserView alloc] initWithFrame:NSMakeRect(0, bottom, W, H - bottom - top) purpose:c.save ? FKPurposeSave : FKPurposeOpen];
	[c.browser setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
	c.browser.delegate = c;
	c.browser.allowedExtensions = c.save ? nil : o[@"types"];
	c.browser.allowsMultipleSelection = [o[@"multiple"] boolValue];
	c.browser.canChooseFiles = o[@"files"] ? [o[@"files"] boolValue] : YES;
	c.browser.canChooseDirectories = [o[@"dirs"] boolValue];
	[content addSubview:c.browser];
	if (acc) {
		[acc setFrameOrigin:NSMakePoint((W - NSWidth([acc frame])) / 2, 52)];
		[content addSubview:acc];
	}
	if ([o[@"message"] length] && !c.save) {
		NSTextField *m = [[NSTextField alloc] initWithFrame:NSMakeRect(20, 16, W - 260, 20)];
		[m setStringValue:o[@"message"]]; [m setEditable:NO]; [m setBordered:NO]; [m setDrawsBackground:NO];
		[content addSubview:m];
	}
	NSButton *cancel = [[NSButton alloc] initWithFrame:NSMakeRect(W - 226, 12, 100, 28)];
	[cancel setTitle:L("panel.cancel")]; [cancel setBezelStyle:NSRoundedBezelStyle];
	[cancel setTarget:c]; [cancel setAction:@selector(cancel:)];
	[cancel setKeyEquivalent:@"\033"];
	[content addSubview:cancel];
	c.okButton = [[NSButton alloc] initWithFrame:NSMakeRect(W - 118, 12, 100, 28)];
	[c.okButton setTitle:[o[@"prompt"] length] ? o[@"prompt"] : (c.save ? L("panel.savebutton") : L("panel.openbutton"))];
	[c.okButton setBezelStyle:NSRoundedBezelStyle];
	[c.okButton setTarget:c]; [c.okButton setAction:@selector(ok:)];
	[c.okButton setKeyEquivalent:@"\r"];
	[content addSubview:c.okButton];
	NSString *dir = o[@"directory"];
	BOOL isDir = NO;
	if (![dir length] || !fkExists(dir, &isDir) || !isDir)
		dir = [NSString stringWithUTF8String:fc_special_path(c.save ? FC_DOCUMENTS : FC_HOME)];
	[c.browser goTo:dir];
	[w center];
	[w makeKeyAndOrderFront:nil];
	if (c.save) [w makeFirstResponder:c.nameField];
	[NSApp runModalForWindow:w];
	[w orderOut:nil];
	if (acc) [acc removeFromSuperview];
	return c.result;
}
@end

// ------------------------------------------------------------------------------------------------ Ziehen und Ablegen
// Wie im Finder: ausgewählte Einträge auf einen Ordner, in ein anderes Fundus-Fenster oder auf die Seitenleiste ziehen.
// Bewegen auf demselben Volume, sonst kopieren; ⌥ kopiert immer (grünes Plus am Ziehbild); Papierkorb = in den Papierkorb,
// Programm = damit öffnen. Fensterfokus wie bei macOS: Das Quellfenster bleibt, wo es ist (preventWindowOrdering),
// das Zielfenster wird nicht aktiviert, nur die abgelegten Einträge sind dort danach ausgewählt.
// Die Schleife läuft im Programm selbst (eigenes Ziehbild-Fenster): Darlings NSDragging geht nicht über Fenster hinweg.
// Ziehen in andere Programme, ins Dock und auf den Schreibtisch (XDND) kommt später.

@interface FKDragImageView : NSView
@property (strong) FKItem *item;
@property NSInteger count;
@property BOOL copying, list, solid;   // solid: ohne Alphakanal im Fenster (ClokeOS), dafür ausgeschnitten
@property NSRect iconRect;
@end
@implementation FKDragImageView
- (BOOL)isFlipped { return YES; }
- (BOOL)isOpaque { return NO; }
- (void)drawRect:(NSRect)dirty {
	[[NSColor clearColor] setFill];
	NSRectFill([self bounds]);
	NSRect ir = self.iconRect;
	CGFloat alpha = self.solid ? 1 : 0.75;
	if (!drawPreview(self.item, ir, alpha, self.list)) {
		NSImage *img = iconFor(self.item.path, self.item.kind, self.list);
		[img drawInRect:ir fromRect:NSZeroRect operation:NSCompositeSourceOver fraction:alpha respectFlipped:YES hints:nil];
	}
	NSDictionary *at = textAttrs(13, NO, [NSColor whiteColor], self.list ? NSLeftTextAlignment : NSCenterTextAlignment);
	NSSize ts = [self.item.name sizeWithAttributes:at];
	NSRect tr = self.list ? NSMakeRect(NSMaxX(ir) + 6, NSMinY(ir) - 1, MIN(ts.width + 12, NSWidth([self bounds]) - NSMaxX(ir) - 10), 19)
	                      : NSMakeRect(NSMidX([self bounds]) - MIN(ts.width + 12, NSWidth([self bounds]) - 4) / 2, NSMaxY(ir) + 9,
	                                   MIN(ts.width + 12, NSWidth([self bounds]) - 4), 19);
	[rgb(124, 92, 255, self.solid ? 1 : 0.85) setFill];
	[[NSBezierPath bezierPathWithRoundedRect:tr xRadius:5 yRadius:5] fill];
	[self.item.name drawInRect:NSInsetRect(tr, 6, 1) withAttributes:at];
	// Zähler (mehrere Einträge) und grünes Plus (kopieren) oben rechts am Symbol, wie bei macOS
	CGFloat bx = NSMaxX(ir) - 6, by = NSMinY(ir) - 6;
	if (self.copying) {
		NSRect b = NSMakeRect(bx, by, 18, 18);
		[rgb(52, 199, 89, 1) setFill];
		[[NSBezierPath bezierPathWithOvalInRect:b] fill];
		NSBezierPath *p = [NSBezierPath bezierPath];
		[p moveToPoint:NSMakePoint(NSMidX(b), NSMinY(b) + 4.5)]; [p lineToPoint:NSMakePoint(NSMidX(b), NSMaxY(b) - 4.5)];
		[p moveToPoint:NSMakePoint(NSMinX(b) + 4.5, NSMidY(b))]; [p lineToPoint:NSMakePoint(NSMaxX(b) - 4.5, NSMidY(b))];
		[p setLineWidth:2]; [[NSColor whiteColor] setStroke]; [p stroke];
		bx -= 20;
	}
	if (self.count > 1) {
		NSString *n = [NSString stringWithFormat:@"%ld", (long)self.count];
		NSDictionary *bat = textAttrs(12, YES, [NSColor whiteColor], NSCenterTextAlignment);
		CGFloat w = MAX(18, [n sizeWithAttributes:bat].width + 10);
		NSRect b = NSMakeRect(bx - w + 18, by, w, 18);
		[rgb(255, 59, 48, 1) setFill];
		[[NSBezierPath bezierPathWithRoundedRect:b xRadius:9 yRadius:9] fill];
		[n drawInRect:NSMakeRect(NSMinX(b), NSMinY(b) + 1, w, 16) withAttributes:bat];
	}
}
@end

// FKBrowserView in einem Fenster finden (das Fenster selbst oder ein Teil davon)
static FKBrowserView *fkBrowserIn(NSView *v) {
	if ([v isKindOfClass:[FKBrowserView class]]) return (FKBrowserView *)v;
	for (NSView *s in [v subviews]) {
		FKBrowserView *b = fkBrowserIn(s);
		if (b) return b;
	}
	return nil;
}

typedef struct {
	FKItemsView *items;   // Hervorhebung in einer Elementansicht
	FKSidebarView *side;  // … oder in der Seitenleiste
	NSInteger index;      // Eintrag/Zeile (-1 = Ordner der Ansicht)
	__unsafe_unretained NSString *dir;   // Zielordner (nil = kein Ziel)
	BOOL app, trash;
} fk_drop;

static void fkSetHighlight(fk_drop *d, BOOL on) {
	if (d->items) {
		d->items.dropIndex = on ? d->index : -1;
		d->items.dropOnView = on && d->index < 0;
		[d->items setNeedsDisplay:YES];
	}
	if (d->side) { d->side.dropRow = on ? d->index : -1; [d->side setNeedsDisplay:YES]; }
}

// Ziel am Bildschirmpunkt. Nur Fundus-Fenster dieses Programms; ein fremdes Fenster darüber verdeckt sie.
static fk_drop fkDropAt(NSPoint screen, NSArray *paths, NSWindow *dragWin) {
	fk_drop d = { nil, nil, -1, nil, NO, NO };
	NSInteger num = [NSWindow windowNumberAtPoint:screen belowWindowWithWindowNumber:0];
	if (num && num == [dragWin windowNumber]) num = [NSWindow windowNumberAtPoint:screen belowWindowWithWindowNumber:num];
	NSWindow *w = num ? [NSApp windowWithWindowNumber:num] : nil;
	FKBrowserView *b = w ? fkBrowserIn([w contentView]) : nil;
	if (!b || b.purpose != FKPurposeWindow) return d;
	NSPoint base = [w convertScreenToBase:screen];
	NSPoint ps = [b.sidebar convertPoint:base fromView:nil];
	if (NSPointInRect(ps, [b.sidebar bounds])) {
		NSInteger r = [b.sidebar rowAtPoint:ps];
		if (r < 0) return d;
		d.side = b.sidebar; d.index = r; d.dir = b.sidebar.rows[r][@"path"];
		d.trash = [d.dir isEqualToString:[NSString stringWithUTF8String:fc_special_path(FC_TRASH)]];
	} else {
		NSPoint pi = [b.itemsView convertPoint:base fromView:nil];
		if (!NSPointInRect(pi, [b.itemsView visibleRect]) || (b.itemsView.mode == FKViewModeList && pi.y < LIST_HEAD)) return d;
		NSInteger i = [b.itemsView indexAtPoint:pi];
		FKItem *it = i >= 0 ? b.itemsView.items[i] : nil;
		d.items = b.itemsView;
		if (it && ![paths containsObject:it.path] && (it.kind == FC_FOLDER || it.kind == FC_APP)) {
			d.index = i; d.dir = it.path; d.app = it.kind == FC_APP;
		} else d.dir = b.directory;
		d.trash = [d.dir isEqualToString:[NSString stringWithUTF8String:fc_special_path(FC_TRASH)]];
	}
	// nicht in den eigenen Ordner, nicht in sich selbst oder einen Unterordner davon
	for (NSString *p in paths) {
		if (!d.app && [[p stringByDeletingLastPathComponent] isEqualToString:d.dir]) { d.dir = nil; break; }
		if ([d.dir isEqualToString:p] || [d.dir hasPrefix:[p stringByAppendingString:@"/"]]) { d.dir = nil; break; }
	}
	if (!d.dir) d.items = nil, d.side = nil;
	return d;
}

static BOOL fkCopies(NSArray *paths, NSString *dir, NSUInteger mods) {
	if (mods & NSAlternateKeyMask) return YES;
	for (NSString *p in paths)
		if (!fc_same_volume([[p stringByDeletingLastPathComponent] UTF8String], [dir UTF8String])) return YES;
	return NO;
}

static NSString *fkBundleID(NSString *app) {
	NSDictionary *info = [NSDictionary dictionaryWithContentsOfFile:[app stringByAppendingPathComponent:@"Contents/Info.plist"]];
	return info[@"CFBundleIdentifier"];
}

// ClokeOS: Fenster ohne Alphakanal. Bild in eine Bitmap zeichnen und nur die deckenden Pixel stehen lassen.
static void fkShapeDragWindow(NSWindow *dw, FKDragImageView *iv) {
	if (!iv.solid) return;
	{
		NSInteger W = (NSInteger)NSWidth([iv bounds]), H = (NSInteger)NSHeight([iv bounds]);
		NSBitmapImageRep *bm = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:NULL pixelsWide:W pixelsHigh:H bitsPerSample:8
		                       samplesPerPixel:4 hasAlpha:YES isPlanar:NO colorSpaceName:NSDeviceRGBColorSpace bytesPerRow:W * 4 bitsPerPixel:32];
		[NSGraphicsContext saveGraphicsState];
		[NSGraphicsContext setCurrentContext:[NSGraphicsContext graphicsContextWithBitmapImageRep:bm]];
		NSAffineTransform *flip = [NSAffineTransform transform];
		[flip translateXBy:0 yBy:H];
		[flip scaleXBy:1 yBy:-1];
		[flip concat];
		[iv drawRect:[iv bounds]];
		[NSGraphicsContext restoreGraphicsState];
		NSMutableArray *rects = [NSMutableArray array];
		unsigned char *px = [bm bitmapData];
		NSInteger bpr = [bm bytesPerRow];
		for (NSInteger y = 0; px && y < H; y++)   // Zeile y der Bitmap = Fensterzeile y von oben
			for (NSInteger x = 0; x < W;) {
				while (x < W && px[y * bpr + x * 4 + 3] < 100) x++;
				NSInteger x0 = x;
				while (x < W && px[y * bpr + x * 4 + 3] >= 100) x++;
				if (x > x0) [rects addObject:[NSValue valueWithRect:NSMakeRect(x0, y, x - x0, 1)]];
			}
		[dw performSelector:@selector(clk_setShapeRects:) withObject:rects];
	}
}

static void fkRunDrag(FKItemsView *src, NSEvent *ev, NSPoint startInWindow, NSInteger grabbed) {
	NSWindow *sw = [src window];
	NSArray *paths = [src.browser selectedPaths];
	if (![paths count] || grabbed < 0 || grabbed >= (NSInteger)[src.items count]) return;
	BOOL list = src.mode == FKViewModeList;

	// Ziehbild: Symbol und Name des gepackten Eintrags, an derselben Stelle unter dem Zeiger wie beim Packen
	NSRect r = [src rectOfItem:grabbed];
	NSRect iconInView = list ? NSMakeRect([src col:0] + 2, r.origin.y + 2, 16, 16) : NSMakeRect(NSMidX(r) - 32, r.origin.y + 6, 64, 64);
	NSSize size = list ? NSMakeSize(300, 28) : NSMakeSize(150, 112);
	FKDragImageView *iv = [[FKDragImageView alloc] initWithFrame:NSMakeRect(0, 0, size.width, size.height)];
	iv.item = src.items[grabbed];
	iv.count = [paths count];
	iv.list = list;
	iv.iconRect = list ? NSMakeRect(4, 6, 16, 16) : NSMakeRect((size.width - 64) / 2, 14, 64, 64);
	NSPoint grabInView = [src convertPoint:startInWindow fromView:nil];
	// Abstand Zeiger -> linke obere Ecke des Ziehbilds (Bildschirm: y nach oben)
	CGFloat offX = grabInView.x - NSMinX(iconInView) + NSMinX(iv.iconRect);
	CGFloat offY = grabInView.y - NSMinY(iconInView) + NSMinY(iv.iconRect);
	NSWindow *dw = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, size.width, size.height) styleMask:NSBorderlessWindowMask
	                                             backing:NSBackingStoreBuffered defer:NO];
	[dw setOpaque:NO];
	[dw setBackgroundColor:[NSColor clearColor]];
	[dw setHasShadow:NO];
	[dw setIgnoresMouseEvents:YES];
	[dw setLevel:NSPopUpMenuWindowLevel];
	[dw setReleasedWhenClosed:NO];
	[dw setContentView:iv];
	iv.solid = [dw respondsToSelector:@selector(clk_setShapeRects:)];
	fkShapeDragWindow(dw, iv);
	NSPoint (^place)(NSPoint) = ^NSPoint(NSPoint screen) {
		NSPoint o = NSMakePoint(screen.x - offX, screen.y + offY - size.height);
		[dw setFrameOrigin:o];
		return o;
	};
	NSPoint startScreen = [sw convertBaseToScreen:startInWindow];
	NSPoint home = NSMakePoint(startScreen.x - offX, startScreen.y + offY - size.height);

	fk_drop cur = { nil, nil, -1, nil, NO, NO };
	NSUInteger mods = [ev modifierFlags];
	BOOL first = YES;
	for (;;) {
		NSEventType t = [ev type];
		if (t == NSLeftMouseDragged || t == NSFlagsChanged || first) {
			if (t != NSFlagsChanged) {
				NSPoint screen = [sw convertBaseToScreen:[ev locationInWindow]];
				place(screen);
				if (first) [dw orderFront:nil];
				first = NO;
				fk_drop d = fkDropAt(screen, paths, dw);
				if (d.items != cur.items || d.side != cur.side || d.index != cur.index || (d.dir != cur.dir && ![d.dir isEqualToString:cur.dir])) {
					fkSetHighlight(&cur, NO);
					cur = d;
					fkSetHighlight(&cur, YES);
				}
			}
			mods = [ev modifierFlags];
			BOOL copying = cur.dir && !cur.app && !cur.trash && fkCopies(paths, cur.dir, mods);
			if (copying != iv.copying) { iv.copying = copying; fkShapeDragWindow(dw, iv); [iv setNeedsDisplay:YES]; }
		} else if (t == NSLeftMouseUp) break;
		ev = [sw nextEventMatchingMask:NSLeftMouseDraggedMask | NSLeftMouseUpMask | NSFlagsChangedMask untilDate:[NSDate distantFuture]
		                        inMode:NSEventTrackingRunLoopMode dequeue:YES];
		if (!ev) break;
	}
	fkSetHighlight(&cur, NO);

	if (!cur.dir) {   // kein Ziel: Ziehbild gleitet zurück
		NSPoint from = [dw frame].origin;
		for (int k = 1; k <= 8; k++) {
			CGFloat f = k / 8.0;
			[dw setFrameOrigin:NSMakePoint(from.x + (home.x - from.x) * f, from.y + (home.y - from.y) * f)];
			[dw display];
			usleep(14000);
		}
		[dw orderOut:nil];
		return;
	}
	[dw orderOut:nil];

	NSMutableArray *results = [NSMutableArray array];
	if (cur.app) {
		NSString *bid = fkBundleID(cur.dir);
		for (NSString *p in paths) fc_open([p UTF8String], [bid UTF8String]);
		return;
	}
	BOOL copying = !cur.trash && fkCopies(paths, cur.dir, mods);
	NSMutableSet *dirs = [NSMutableSet setWithObject:cur.dir];
	for (NSString *p in paths) {
		char out[PATH_MAX] = "";
		int err = cur.trash ? fc_trash([p UTF8String])
		        : copying ? fc_copy([p UTF8String], [cur.dir UTF8String], out, sizeof out)
		                  : fc_move([p UTF8String], [cur.dir UTF8String], out, sizeof out);
		if (err) { NSBeep(); TR("ablegen %s: %s", [p UTF8String], strerror(err)); continue; }
		[dirs addObject:[p stringByDeletingLastPathComponent]];
		if (*out) [results addObject:[NSString stringWithUTF8String:out]];
	}
	// alle Fenster mit Quelle oder Ziel neu lesen; im Zielfenster die abgelegten Einträge auswählen, ohne es zu aktivieren
	for (NSWindow *w in [NSApp windows]) {
		FKBrowserView *b = fkBrowserIn([w contentView]);
		if (!b || ![dirs containsObject:b.directory]) continue;
		[b reload];
		if ([b.directory isEqualToString:cur.dir] && [results count]) {
			[b.itemsView.selection removeAllIndexes];
			for (NSString *p in results) [b selectPath:p];
		}
	}
}
