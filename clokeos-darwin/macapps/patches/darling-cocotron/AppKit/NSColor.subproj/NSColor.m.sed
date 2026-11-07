# ClokeOS: colorUsingColorSpaceName: an die Fassung mit device: weitergeben (wie bei Apple), statt "abstrakt" zu werfen;
# NSColor_CGColor setzt nur die zweite um. Logic Pro rief sie tausendfach je Start und lief damit in Ausnahmen.
/^- \(NSColor \*\) colorUsingColorSpaceName: \(NSColorSpaceName\) colorSpace \{$/{
n
s/NSInvalidAbstractInvocation\(\);/return [self colorUsingColorSpaceName: colorSpace device: nil];/
}
# ClokeOS: colorWithAlphaComponent: gab für Alpha < 1 nil zurück (Grundklasse); jetzt über eine RGB-Fassung der Farbe
/^- \(NSColor \*\) colorWithAlphaComponent: \(CGFloat\) alpha \{$/,/^\}$/s/^    return nil;$/    NSColor *rgb = [self colorUsingColorSpaceName: NSCalibratedRGBColorSpace device: nil];\n    return (rgb != nil \&\& rgb != self) ? [rgb colorWithAlphaComponent: alpha] : nil;/
