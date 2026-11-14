// Öffnen/Sichern-Panels aller Mac-Programme über Fundus (docs/fundus.md): Cocotron fragt den Display-Treiber mit
// -implementsCustomPanelForClass:, ob er die Panels selbst zeigt (so macht es das Windows-Backend mit nativen Dialogen).
// Hier antwortet X11Display mit Ja, sobald FundusKit.framework da ist, und zeigt +[FKPanel fk_runPanelWithOptions:].
// Das NSSavePanel/NSOpenPanel-Objekt bleibt Darlings: Programme lesen Ergebnis und Einstellungen wie gewohnt dort.
// Dazu die Block-APIs, die Darling nicht hat (beginWithCompletionHandler:, beginSheetModalForWindow:completionHandler:,
// -[NSDocumentController beginOpenPanelWithCompletionHandler:] und beginOpenPanel:forTypes:completionHandler:).
// CLOKE_FUNDUS_PANEL=0 schaltet zurück auf Darlings eigenes Panel; CLOKE_SAVEPANEL_DIR (Tests) gilt vorher.
#import <AppKit/AppKit.h>
#import <AppKit/NSDisplay.h>
#include <dlfcn.h>
#include <objc/runtime.h>
#include <objc/message.h>
#include <stdlib.h>
#include <string.h>

#define FUNDUSKIT "/Library/ClokeOS/MacFrameworks/FundusKit.framework/Versions/A/FundusKit"

static Class fkPanel(void) {
	static Class c;
	static int tried;
	if (c || tried) return c;
	tried = 1;
	const char *off = getenv("CLOKE_FUNDUS_PANEL");
	if (off && !strcmp(off, "0")) return nil;
	if (!dlopen(FUNDUSKIT, RTLD_NOW | RTLD_GLOBAL)) { NSLog(@"FundusKit nicht geladen: %s", dlerror()); return nil; }
	c = objc_getClass("FKPanel");
	return c;
}

static BOOL clk_implementsCustomPanelForClass(id self, SEL _cmd, Class panelClass) {
	if (![panelClass isSubclassOfClass:[NSSavePanel class]]) return NO;
	return fkPanel() != nil;
}

static NSArray *runFundus(NSSavePanel *panel, BOOL open, NSString *dir, NSString *file, NSArray *types) {
	NSMutableDictionary *o = [NSMutableDictionary dictionary];
	o[@"purpose"] = open ? @"open" : @"save";
	if ([dir length]) o[@"directory"] = dir;
	if ([file length]) {
		o[@"name"] = [file lastPathComponent];
		if (![dir length] && [file isAbsolutePath]) o[@"directory"] = [file stringByDeletingLastPathComponent];
	} else if (!open && [[panel nameFieldStringValue] length]) o[@"name"] = [panel nameFieldStringValue];
	NSArray *t = types ? types : [panel allowedFileTypes];
	if (!open && ![t count] && [[panel requiredFileType] length]) t = @[ [panel requiredFileType] ];
	if ([t count]) o[@"types"] = t;
	if ([[panel title] length]) o[@"title"] = [panel title];
	if ([[panel prompt] length]) o[@"prompt"] = [panel prompt];
	if ([[panel message] length]) o[@"message"] = [panel message];
	if ([panel accessoryView]) o[@"accessory"] = [panel accessoryView];
	if (open) {
		NSOpenPanel *op = (NSOpenPanel *)panel;
		o[@"multiple"] = @([op allowsMultipleSelection]);
		o[@"files"] = @([op canChooseFiles]);
		o[@"dirs"] = @([op canChooseDirectories]);
	}
	NSArray *(*run)(id, SEL, NSDictionary *) = (NSArray * (*)(id, SEL, NSDictionary *))objc_msgSend;
	return run(fkPanel(), @selector(fk_runPanelWithOptions:), o);
}

static int clk_savePanel(id self, SEL _cmd, NSSavePanel *panel, NSString *dir, NSString *file) {
	NSArray *r = runFundus(panel, NO, dir, file, nil);
	if (![r count]) return NSCancelButton;
	NSString *p = r[0];
	[panel setDirectory:[p stringByDeletingLastPathComponent]];
	[panel setNameFieldStringValue:[p lastPathComponent]];
	[panel performSelector:@selector(_setFilename:) withObject:p];
	return NSOKButton;
}

static int clk_openPanel(id self, SEL _cmd, NSOpenPanel *panel, NSString *dir, NSString *file, NSArray *types) {
	NSArray *r = runFundus(panel, YES, dir, file, types);
	if (![r count]) return NSCancelButton;
	[panel setDirectory:[r[0] stringByDeletingLastPathComponent]];
	[panel performSelector:@selector(_setFilename:) withObject:r[0]];
	Ivar iv = class_getInstanceVariable([NSOpenPanel class], "_filenames");   // mehrere Dateien
	if (iv) {
		NSArray *old = object_getIvar(panel, iv);
		object_setIvar(panel, iv, [r copy]);
		[old release];
	}
	return NSOKButton;
}

// Block-APIs: Apple ruft den Block nach dem Panel auf, ohne den Aufrufer zu blockieren; hier im nächsten Durchlauf
// der Ereignisschleife, mit einem modalen Panel (für das Programm sieht das wie ein Sheet aus, das sich schließt).
@interface NSSavePanel (ClokeFundus)
- (void)clk_runWithHandler:(id)handler;
@end
@implementation NSSavePanel (ClokeFundus)
- (void)clk_runWithHandler:(id)handler {
	NSInteger r = [self runModal];
	void (^h)(NSInteger) = handler;
	if (h) h(r == NSOKButton ? 1 : 0);   // NSModalResponseOK = 1, NSModalResponseCancel = 0
	[handler release];
}
@end

static void clk_beginWithCompletionHandler(NSSavePanel *self, SEL _cmd, void (^handler)(NSInteger)) {
	[self performSelector:@selector(clk_runWithHandler:) withObject:[handler copy] afterDelay:0];
}
static void clk_beginSheet(NSSavePanel *self, SEL _cmd, NSWindow *window, void (^handler)(NSInteger)) {
	[self performSelector:@selector(clk_runWithHandler:) withObject:[handler copy] afterDelay:0];
}

// im nächsten Durchlauf des Haupt-Runloops (Darlings Runloop leert die Haupt-Dispatch-Queue nicht sicher)
static void clk_later(void (^b)(void)) {
	CFRunLoopPerformBlock(CFRunLoopGetMain(), kCFRunLoopCommonModes, b);
	CFRunLoopWakeUp(CFRunLoopGetMain());
}

// NSDocumentController (10.8): "Öffnen …" moderner Programme läuft hierüber (Logic Pro: CLgDocumentController); Darling hat es nicht.
@interface NSDocumentController (ClokeFundusPanels)
- (NSArray *)_allFileExtensions;
@end

static void clk_docBeginOpenPanel(id self, SEL _cmd, void (^handler)(NSArray *)) {
	void (^h)(NSArray *) = [handler copy];
	clk_later(^{
		NSOpenPanel *op = [NSOpenPanel openPanel];
		[op setAllowsMultipleSelection:YES];
		NSArray *types = [self respondsToSelector:@selector(_allFileExtensions)] ? [self _allFileExtensions] : nil;
		NSInteger r = [op runModalForDirectory:nil file:nil types:types];
		if (h) h(r == NSOKButton ? [op URLs] : nil);
		[h release];
	});
}

static void clk_docBeginOpenPanelForTypes(id self, SEL _cmd, NSOpenPanel *op, NSArray *types, void (^handler)(NSInteger)) {
	void (^h)(NSInteger) = [handler copy];
	[op retain];
	clk_later(^{
		NSInteger r = [op runModalForDirectory:nil file:nil types:types];
		if (h) h(r == NSOKButton ? 1 : 0);
		[h release];
		[op release];
	});
}

static BOOL installX11(void) {
	Class x11 = objc_getClass("X11Display");
	if (!x11) return NO;
	class_replaceMethod(x11, @selector(implementsCustomPanelForClass:), (IMP)clk_implementsCustomPanelForClass, "c@:#");
	class_replaceMethod(x11, @selector(savePanel:runModalForDirectory:file:), (IMP)clk_savePanel, "i@:@@@");
	class_replaceMethod(x11, @selector(openPanel:runModalForDirectory:file:types:), (IMP)clk_openPanel, "i@:@@@@");
	return YES;
}

__attribute__((constructor)) static void clk_fundus_bridge(void) {
	// Im AppKit-Bau steckt diese Datei im X11-Backend, X11Display gibt es dann schon. Als eigene Bibliothek (Test mit
	// DYLD_INSERT_LIBRARIES) lädt AppKit das Backend erst später: dann beim ersten Durchlauf des Haupt-Runloops.
	if (!installX11())
		CFRunLoopPerformBlock(CFRunLoopGetMain(), kCFRunLoopCommonModes, ^{ if (!installX11()) NSLog(@"fundusbridge: kein X11Display"); });
	Class dc = objc_getClass("NSDocumentController");
	class_addMethod(dc, @selector(beginOpenPanelWithCompletionHandler:), (IMP)clk_docBeginOpenPanel, "v@:@?");
	class_addMethod(dc, @selector(beginOpenPanel:forTypes:completionHandler:), (IMP)clk_docBeginOpenPanelForTypes, "v@:@@@?");
	Class sp = objc_getClass("NSSavePanel");
	class_replaceMethod(sp, @selector(beginWithCompletionHandler:), (IMP)clk_beginWithCompletionHandler, "v@:@?");
	class_replaceMethod(sp, @selector(beginSheetModalForWindow:completionHandler:), (IMP)clk_beginSheet, "v@:@@?");
}
