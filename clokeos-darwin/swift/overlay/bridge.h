// Minimale Deklarationen für die Brücke zwischen Swift und Objective-C-Foundation (Darlings Klassen, zur Laufzeit gebunden)
@import ObjectiveC;
@interface NSString : NSObject
@end
NSString *clk_nsstring_utf8(const char *bytes, long length);
