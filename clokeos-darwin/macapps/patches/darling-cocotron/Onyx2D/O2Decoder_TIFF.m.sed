# ClokeOS: ASCII-Felder als UTF-8 lesen (so schreibt sie macOS), Latin-1 nur, wenn das kein gültiges UTF-8 ist
s/^(    return \[\[\[NSString alloc\] initWithBytes: ascii)$/    NSString *utf8 = [[[NSString alloc] initWithBytes: ascii length: count - 1 encoding: NSUTF8StringEncoding] autorelease];\n    if (utf8 != nil)\n        return utf8;\n\1/
