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
