# ClokeOS: +localizedStringWithFormat: war ein Platzhalter, der nil lieferte (Logic Pro legt das Ergebnis in eine NSAttributedString:
# Absturz beim Öffnen eines Projekts). Wie bei Apple: Format mit dem aktuellen Gebietsschema.
/^\+ \(id\)localizedStringWithFormat:\(NSString \*\)format, \.\.\.$/,/^\}$/{
s/^#warning TODO https:\/\/code.google.com\/p\/apportable\/issues\/detail\?id=269$/    va_list args;\n    va_start(args, format);\n    NSString *result = [[[self alloc] initWithFormat: format locale: [NSLocale currentLocale] arguments: args] autorelease];\n    va_end(args);/
s/^    return nil;$/    return result;/
}
