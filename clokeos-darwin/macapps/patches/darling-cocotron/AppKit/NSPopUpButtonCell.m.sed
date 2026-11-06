# ClokeOS: Cocotrons eigene Aufrufe von synchronizeTitleAndSelectedItem laufen über _clkSynchronize: die (überschreibbare) Methode einmal,
# verschachtelte Aufrufe direkt die Fassung von NSPopUpButtonCell. Logic Pros MAPopUpButtonCell ruft darin selectItem: auf, das wieder
# synchronisierte: endlose Rekursion bis zum Stapelüberlauf beim Öffnen eines Projekts.
s/^( *)\[self synchronizeTitleAndSelectedItem\];$/\1[self _clkSynchronize];/
s/^@end$/static int clk_sync_depth;\n- (void) _clkSynchronize {\n    if (clk_sync_depth > 0) {\n        ((void (*)(id, SEL)) [NSPopUpButtonCell instanceMethodForSelector: @selector(synchronizeTitleAndSelectedItem)])(self, @selector(synchronizeTitleAndSelectedItem));\n        return;\n    }\n    clk_sync_depth++;\n    [self synchronizeTitleAndSelectedItem];\n    clk_sync_depth--;\n}\n\n@end/
