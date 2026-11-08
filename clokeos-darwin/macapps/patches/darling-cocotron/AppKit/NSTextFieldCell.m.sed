# ClokeOS: auf hervorgehobenem Grund (backgroundStyle Emphasized, z. B. gewählte Zeile einer ansichtsbasierten Tabelle) zeichnet die Zelle
# Systemfarben für Schrift (controlTextColor, labelColor, textColor …) wie AppKit in alternateSelectedControlTextColor; eigene Farben
# bleiben (macapps/appkit/tableviews.m, ClkCellTextColor).
s/^@implementation NSTextFieldCell$/NSColor *ClkCellTextColor(NSCell *cell, NSColor *color);\n\n&/
/^- \(NSAttributedString \*\) attributedStringValue \{$/,/^\}$/s/^            if \(_textColor != nil\)$/            if (ClkCellTextColor(self, _textColor) != nil)/
/^- \(NSAttributedString \*\) attributedStringValue \{$/,/^\}$/s/^                \[attributes setObject: _textColor$/                [attributes setObject: ClkCellTextColor(self, _textColor)/
