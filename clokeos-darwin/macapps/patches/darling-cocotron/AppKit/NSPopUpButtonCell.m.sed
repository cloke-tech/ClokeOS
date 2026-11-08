# ClokeOS: Cocotrons eigene Aufrufe von synchronizeTitleAndSelectedItem laufen über _clkSynchronize: die (überschreibbare) Methode einmal,
# verschachtelte Aufrufe direkt die Fassung von NSPopUpButtonCell. Logic Pros MAPopUpButtonCell ruft darin selectItem: auf, das wieder
# synchronisierte: endlose Rekursion bis zum Stapelüberlauf beim Öffnen eines Projekts.
s/^( *)\[self synchronizeTitleAndSelectedItem\];$/\1[self _clkSynchronize];/
s/^@end$/static int clk_sync_depth;\n- (void) _clkSynchronize {\n    if (clk_sync_depth > 0) {\n        ((void (*)(id, SEL)) [NSPopUpButtonCell instanceMethodForSelector: @selector(synchronizeTitleAndSelectedItem)])(self, @selector(synchronizeTitleAndSelectedItem));\n        return;\n    }\n    clk_sync_depth++;\n    [self synchronizeTitleAndSelectedItem];\n    clk_sync_depth--;\n}\n\n@end/
# ClokeOS: eigene Wert-Setzer wie bei Apple: setObjectValue: wählt den Eintrag [Wert intValue]; setIntValue:/setIntegerValue:/setFloatValue:/
# setDoubleValue: wählen den Eintrag direkt, ohne über setObjectValue: zu gehen (auf dem Mac nachgeprüft), setStringValue: tut nichts.
# Sonst liefen NSCells Setzer über setObjectValue:, und Logic Pros MyPopUpButtonCell (setObjectValue: ruft setDoubleValue:) rekursierte
# bis zum Stapelüberlauf.
s/static int clk_sync_depth;/- (void) setObjectValue: (id) value {\n    if ([value respondsToSelector: @selector(intValue)])\n        [self selectItemAtIndex: [value intValue]];\n}\n- (void) setIntValue: (int) value {\n    [self selectItemAtIndex: value];\n}\n- (void) setIntegerValue: (NSInteger) value {\n    [self selectItemAtIndex: value];\n}\n- (void) setFloatValue: (float) value {\n    [self selectItemAtIndex: (NSInteger) value];\n}\n- (void) setDoubleValue: (double) value {\n    [self selectItemAtIndex: (NSInteger) value];\n}\n- (void) setStringValue: (NSString *) value {\n}\nstatic int clk_sync_depth;/
# … ein Index außerhalb der Einträge wählt nichts aus (statt einer NSRangeException aus itemAtIndex:)
s/^    NSMenuItem \*item = \(index < 0\) \? nil : \[self itemAtIndex: index\];$/    NSMenuItem *item = (index < 0 || index >= [self numberOfItems]) ? nil : [self itemAtIndex: index];/
# ClokeOS: CLOKE_BUTTON_TRACE meldet setTitle: von Aufklapp-Zellen (Titel, abwärts klappend, Einträge vorher/nachher, gewählter Titel)
/^- \(void\) setTitle: \(NSString \*\) title \{$/,/^\}$/s/^\}$/    {\
        static int clk_t = -1, clk_n;\
        if (clk_t < 0) clk_t = getenv("CLOKE_BUTTON_TRACE") != NULL;\
        if (clk_t \&\& clk_n++ < 400)\
            NSLog(@"[clokeos-popup] %s setTitle:%@ pullsDown=%d items=%ld sel=%ld title=%@ idx=%ld,%ld menu=%p,%p erstes=%@", object_getClassName(self), title, (int) _pullsDown, (long) [_menu numberOfItems], (long) _selectedIndex, [self title], (long) [_menu indexOfItemWithTitle: title], (long) [self indexOfItemWithTitle: title], _menu, [self menu], [_menu numberOfItems] ? [[_menu itemAtIndex: 0] title] : nil);\
    }\
}/
# ClokeOS: altersStateOfSelectedItem (Standard YES) wie bei Apple: selectItem: schaltet den bisher gewählten Eintrag aus und den neuen
# ein. Logic Pros MAPopUpButtonCell sucht in synchronizeTitleAndSelectedItem den eingeschalteten Eintrag und wählte ohne ihn keinen
# (die Aufklappmenüs im Inspektor blieben leer).
/^- \(void\) selectItem: \(NSMenuItem \*\) item \{$/,/^\}$/s/^    if \(selectedIndex != _selectedIndex\) \{$/    if ([self altersStateOfSelectedItem]) {\
        NSArray *clk_items = [_menu itemArray];\
        if (_selectedIndex >= 0 \&\& _selectedIndex < (NSInteger) [clk_items count] \&\& _selectedIndex != selectedIndex)\
            [[clk_items objectAtIndex: _selectedIndex] setState: NSOffState];\
        if (item != nil \&\& selectedIndex >= 0)\
            [item setState: NSOnState];\
    }\
    if (selectedIndex != _selectedIndex) {/
s/static int clk_sync_depth;/static char clk_alters_key;\
- (BOOL) altersStateOfSelectedItem {\
    NSNumber *v = objc_getAssociatedObject(self, \&clk_alters_key);\
    return v ? [v boolValue] : YES;\
}\
- (void) setAltersStateOfSelectedItem: (BOOL) flag {\
    objc_setAssociatedObject(self, \&clk_alters_key, [NSNumber numberWithBool: flag], OBJC_ASSOCIATION_RETAIN_NONATOMIC);\
}\
static int clk_sync_depth;/
s/^#import <AppKit\/NSPopUpButtonCell.h>$/#import <AppKit\/NSPopUpButtonCell.h>\
#import <objc\/runtime.h>/
