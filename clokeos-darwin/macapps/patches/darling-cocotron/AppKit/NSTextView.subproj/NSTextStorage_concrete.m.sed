# ClokeOS: -initWithString:attributes: und -initWithAttributedString: fehlten (unbekannter Selektor). Logic Pros Zahlenfelder bauen damit
# ein kleines Layout, aus dem sie den Versatz ihrer Ziffern lesen (+[MAKitSegmentedScrubberCell getXOffset:yOffset:…]); ohne sie war der
# Textspeicher nil, alle Versätze 0 und die Ziffern der Transportanzeige und der Inspektorwerte lagen außerhalb ihres Felds.
/^- init \{$/i\
- initWithString: (NSString *) string attributes: (NSDictionary *) attributes {\
    [self initWithString: string ? string : @""];\
    if (attributes != nil \&\& [_string length] > 0)\
        [self setAttributes: attributes range: NSMakeRange(0, [_string length])];\
    return self;\
}\
\
- initWithAttributedString: (NSAttributedString *) other {\
    [self initWithString: @""];\
    if (other != nil)\
        [self replaceCharactersInRange: NSMakeRange(0, 0) withAttributedString: other];\
    return self;\
}\

