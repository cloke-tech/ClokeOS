// Fundus.app: der Finder von ClokeOS (docs/fundus.md). Fenster mit FKBrowserView, Menüs mit den Finder-Kurzbefehlen.
//   Fundus [ordner|datei …]    öffnet die Ordner (Dateien: ihren Ordner, mit Auswahl)
#import "FundusKit.h"
#include "fcore.h"

#define L(k) FKLocalized(@k)
#define TR(...) do { if (getenv("FUNDUS_TRACE")) { fprintf(stderr, "fundus: " __VA_ARGS__); fputc(10, stderr); } } while (0)

@interface FundusWindow : NSObject <FKBrowserDelegate, NSWindowDelegate>
@property (strong) NSWindow *window;
@property (strong) FKBrowserView *browser;
@end

@interface FundusApp : NSObject <NSApplicationDelegate>
@property (strong) NSMutableArray *windows;
@property (strong) NSArray *startPaths;
- (void)windowClosed:(FundusWindow *)w;
@end

static FundusApp *gApp;

@implementation FundusWindow
- (instancetype)initWithPath:(NSString *)path select:(NSString *)sel {
	if (!(self = [super init])) return nil;
	static NSPoint cascade;
	TR("window");
	_window = [[NSWindow alloc] initWithContentRect:NSMakeRect(160, 140, 960, 600)
	                                      styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
	                                        backing:NSBackingStoreBuffered defer:NO];
	[_window setReleasedWhenClosed:NO];
	TR("w1");
	[_window setMinSize:NSMakeSize(520, 320)];
	FKHideTitlebar(_window);
	TR("w2 titlebar");
	[_window setDelegate:self];
	cascade = [_window cascadeTopLeftFromPoint:cascade];
	TR("w3 cascade");
	_browser = [[FKBrowserView alloc] initWithFrame:[[_window contentView] bounds] purpose:FKPurposeWindow];
	[_browser setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
	_browser.delegate = self;
	[[_window contentView] addSubview:_browser];
	TR("browser created");
	[_browser goTo:path];
	TR("goTo done");
	if (sel) [_browser selectPath:sel];
	[_window makeKeyAndOrderFront:nil];
	TR("window shown");
	return self;
}
- (void)browserDidChangeDirectory:(FKBrowserView *)b { [_window setTitle:[b title]]; }
- (void)windowWillClose:(NSNotification *)n { [gApp windowClosed:self]; }
- (void)windowDidResize:(NSNotification *)n { [_browser setFrame:[[_window contentView] bounds]]; }
@end

@implementation FundusApp

- (FundusWindow *)current {
	for (FundusWindow *w in self.windows) if ([w.window isKeyWindow] || [w.window isMainWindow]) return w;
	return [self.windows lastObject];
}
- (FKBrowserView *)cb { return [[self current] browser]; }

- (void)openWindowAt:(NSString *)path select:(NSString *)sel {
	FundusWindow *w = [[FundusWindow alloc] initWithPath:path select:sel];
	[self.windows addObject:w];
}
- (void)windowClosed:(FundusWindow *)w {
	[self.windows removeObject:w];
	if (![self.windows count]) [NSApp terminate:nil];   // wie ein Programm ohne Desktop: das letzte Fenster beendet Fundus
}

static NSMenuItem *item(NSMenu *m, NSString *title, SEL a, NSString *key, NSUInteger mods) {
	NSMenuItem *i = [m addItemWithTitle:title action:a keyEquivalent:key ? key : @""];
	if (mods) [i setKeyEquivalentModifierMask:mods];
	return i;
}
static NSMenu *submenu(NSMenu *bar, NSString *title) {
	NSMenuItem *top = [[NSMenuItem alloc] initWithTitle:title action:NULL keyEquivalent:@""];
	NSMenu *m = [[NSMenu alloc] initWithTitle:title];
	[top setSubmenu:m];
	[bar addItem:top];
	return m;
}

- (NSMenu *)mainMenu {
	NSMenu *bar = [[NSMenu alloc] initWithTitle:@"Fundus"];
	NSUInteger C = NSCommandKeyMask, S = NSShiftKeyMask, O = NSAlternateKeyMask;
	NSMenu *app = submenu(bar, @"Fundus");
	item(app, L("menu.about"), @selector(orderFrontStandardAboutPanel:), nil, 0);
	[app addItem:[NSMenuItem separatorItem]];
	item(app, L("menu.emptytrash"), @selector(emptyTrash:), @"\b", C | S);
	[app addItem:[NSMenuItem separatorItem]];
	item(app, L("menu.hide"), @selector(hide:), @"h", C);
	item(app, L("menu.quit"), @selector(terminate:), @"q", C);

	NSMenu *file = submenu(bar, L("menu.file"));
	item(file, L("menu.newwindow"), @selector(newWindow:), @"n", C);
	item(file, L("menu.newfolder"), @selector(newFolder:), @"n", C | S);
	item(file, L("menu.open"), @selector(openSel:), @"o", C);
	item(file, L("menu.close"), @selector(performClose:), @"w", C);
	[file addItem:[NSMenuItem separatorItem]];
	item(file, L("menu.rename"), @selector(renameSel:), nil, 0);
	item(file, L("menu.duplicate"), @selector(duplicateSel:), @"d", C);
	item(file, L("menu.trash"), @selector(trashSel:), @"\b", C);
	[file addItem:[NSMenuItem separatorItem]];
	item(file, L("menu.terminal"), @selector(terminal:), nil, 0);
	item(file, L("menu.find"), @selector(find:), @"f", C);

	NSMenu *edit = submenu(bar, L("menu.edit"));
	item(edit, L("menu.cut"), @selector(cutSel:), @"x", C);
	item(edit, L("menu.copy"), @selector(copySel:), @"c", C);
	item(edit, L("menu.paste"), @selector(pasteSel:), @"v", C);
	item(edit, L("menu.selectall"), @selector(selectAllItems:), @"a", C);

	NSMenu *view = submenu(bar, L("menu.view"));
	item(view, L("menu.asicons"), @selector(asIcons:), @"1", C);
	item(view, L("menu.aslist"), @selector(asList:), @"2", C);
	[view addItem:[NSMenuItem separatorItem]];
	item(view, L("menu.hidden"), @selector(toggleHidden:), @".", C | S);
	item(view, L("menu.pathbar"), @selector(togglePathBar:), @"p", C | O);
	item(view, L("menu.statusbar"), @selector(toggleStatusBar:), @"/", C);
	item(view, L("menu.reload"), @selector(reloadView:), @"r", C);

	NSMenu *go = submenu(bar, L("menu.go"));
	item(go, L("menu.back"), @selector(back:), @"[", C);
	item(go, L("menu.forward"), @selector(forward:), @"]", C);
	item(go, L("menu.enclosing"), @selector(up:), [NSString stringWithFormat:@"%C", (unichar)NSUpArrowFunctionKey], C);
	[go addItem:[NSMenuItem separatorItem]];
	struct { const char *key; SEL s; NSString *k; NSUInteger m; } gos[] = {
		{ "sidebar.home", @selector(goHome:), @"h", C | S }, { "sidebar.desktop", @selector(goDesktop:), @"d", C | S },
		{ "sidebar.documents", @selector(goDocuments:), @"o", C | S }, { "sidebar.downloads", @selector(goDownloads:), @"l", C | O },
		{ "sidebar.applications", @selector(goApplications:), @"a", C | S }, { "sidebar.library", @selector(goLibrary:), @"l", C | S },
		{ "sidebar.root", @selector(goRoot:), @"c", C | S } };
	for (size_t i = 0; i < sizeof gos / sizeof gos[0]; i++) item(go, FKLocalized([NSString stringWithUTF8String:gos[i].key]), gos[i].s, gos[i].k, gos[i].m);
	[go addItem:[NSMenuItem separatorItem]];
	item(go, L("menu.gotofolder"), @selector(goToFolder:), @"g", C | S);

	NSMenu *win = submenu(bar, L("menu.window"));
	item(win, L("menu.minimize"), @selector(performMiniaturize:), @"m", C);
	return bar;
}

- (void)newWindow:(id)s { [self openWindowAt:[self cb].directory ? [self cb].directory : [NSString stringWithUTF8String:fc_special_path(FC_HOME)] select:nil]; }
- (void)newFolder:(id)s { [[self cb] newFolder]; }
- (void)openSel:(id)s { [[self cb] openSelection]; }
- (void)renameSel:(id)s { [[self cb] renameSelection]; }
- (void)duplicateSel:(id)s { [[self cb] duplicateSelection]; }
- (void)trashSel:(id)s { [[self cb] trashSelection]; }
- (void)terminal:(id)s { [[self cb] openInTerminal]; }
- (void)find:(id)s { [[self cb] focusSearch]; }
- (void)cutSel:(id)s { [[self cb] cutSelection]; }
- (void)copySel:(id)s { [[self cb] copySelection]; }
- (void)pasteSel:(id)s { [[self cb] paste]; }
- (void)selectAllItems:(id)s { [[self cb] selectAll]; }
- (void)asIcons:(id)s { [self cb].viewMode = FKViewModeIcons; }
- (void)asList:(id)s { [self cb].viewMode = FKViewModeList; }
- (void)toggleHidden:(id)s { [self cb].showHidden = ![self cb].showHidden; }
- (void)togglePathBar:(id)s { [self cb].showsPathBar = ![self cb].showsPathBar; }
- (void)toggleStatusBar:(id)s { [self cb].showsStatusBar = ![self cb].showsStatusBar; }
- (void)reloadView:(id)s { [[self cb] reload]; }
- (void)back:(id)s { [[self cb] goBack]; }
- (void)forward:(id)s { [[self cb] goForward]; }
- (void)up:(id)s { [[self cb] goUp]; }
- (void)goSpecial:(fc_special)sp { [[self cb] goTo:[NSString stringWithUTF8String:fc_special_path(sp)]]; }
- (void)goHome:(id)s { [self goSpecial:FC_HOME]; }
- (void)goDesktop:(id)s { [self goSpecial:FC_DESKTOP]; }
- (void)goDocuments:(id)s { [self goSpecial:FC_DOCUMENTS]; }
- (void)goDownloads:(id)s { [self goSpecial:FC_DOWNLOADS]; }
- (void)goApplications:(id)s { [self goSpecial:FC_APPLICATIONS]; }
- (void)goLibrary:(id)s { [self goSpecial:FC_LIBRARY]; }
- (void)goRoot:(id)s { [self goSpecial:FC_ROOT]; }
- (void)emptyTrash:(id)s { fc_empty_trash(); for (FundusWindow *w in self.windows) [w.browser reload]; }

// Gehe zu Ordner: kleines Fenster mit Textfeld
- (void)goToFolder:(id)s {
	NSWindow *w = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 460, 110) styleMask:NSWindowStyleMaskTitled
	                                            backing:NSBackingStoreBuffered defer:NO];
	[w setTitle:L("menu.gotofolder")];
	[w setReleasedWhenClosed:NO];
	NSTextField *f = [[NSTextField alloc] initWithFrame:NSMakeRect(20, 60, 420, 24)];
	[f setStringValue:[self cb].directory ? [self cb].directory : @"/"];
	[[w contentView] addSubview:f];
	NSButton *ok = [[NSButton alloc] initWithFrame:NSMakeRect(340, 14, 100, 28)];
	[ok setTitle:L("panel.go")]; [ok setBezelStyle:NSRoundedBezelStyle]; [ok setKeyEquivalent:@"\r"];
	[ok setTarget:NSApp]; [ok setAction:@selector(stopModal)];
	NSButton *cancel = [[NSButton alloc] initWithFrame:NSMakeRect(232, 14, 100, 28)];
	[cancel setTitle:L("panel.cancel")]; [cancel setBezelStyle:NSRoundedBezelStyle]; [cancel setKeyEquivalent:@"\033"];
	[cancel setTarget:NSApp]; [cancel setAction:@selector(abortModal)];
	[[w contentView] addSubview:ok];
	[[w contentView] addSubview:cancel];
	[w center];
	[w makeKeyAndOrderFront:nil];
	NSInteger r = [NSApp runModalForWindow:w];
	[w orderOut:nil];
	if (r == NSModalResponseStop) {
		NSString *p = [[f stringValue] stringByExpandingTildeInPath];
		if ([p length]) [[self cb] goTo:p];
	}
}

- (void)applicationDidFinishLaunching:(NSNotification *)n {
	TR("didFinishLaunching");
	[NSApp setMainMenu:[self mainMenu]];
	TR("menu");
	self.windows = [NSMutableArray array];
	for (NSString *p in self.startPaths) {
		BOOL d = NO;
		if (!fkExists(p, &d)) continue;
		if (d && fc_kind_of([p UTF8String], 1) == FC_FOLDER) [self openWindowAt:p select:nil];
		else [self openWindowAt:[p stringByDeletingLastPathComponent] select:p];
	}
	if (![self.windows count]) [self openWindowAt:[NSString stringWithUTF8String:fc_special_path(FC_HOME)] select:nil];
	[NSApp activateIgnoringOtherApps:YES];
}
@end

int main(int argc, const char **argv) {
	@autoreleasepool {
		TR("main");
		NSApplication *app = [NSApplication sharedApplication];
		gApp = [[FundusApp alloc] init];
		NSMutableArray *paths = [NSMutableArray array];
		for (int i = 1; i < argc; i++) {
			if (!strcmp(argv[i], "-NSOpen") && i + 1 < argc) { i++; }   // so übergibt CloakServer Dokumente an Mac-Programme
			else if (argv[i][0] == '-') { if (i + 1 < argc && argv[i + 1][0] != '-') i++; continue; }   // -psn_… und andere Schalter
			NSString *p = [[NSString stringWithUTF8String:argv[i]] stringByExpandingTildeInPath];
			if ([p hasPrefix:@"file://"]) p = [[NSURL URLWithString:p] path];
			if (p) [paths addObject:[p stringByStandardizingPath]];
		}
		gApp.startPaths = paths;
		[app setDelegate:gApp];
		TR("run");
		[app run];
	}
	return 0;
}
