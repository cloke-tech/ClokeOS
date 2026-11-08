# ClokeOS: NSCustomView legt die eigentliche Ansicht mit -initWithFrame: an und las nur Rahmen, Maske, Tag und Unteransichten.
# Auto Layout (Bedingungen, translatesAutoresizingMaskIntoConstraints, Prioritäten) kam so nie an; Bedingungen, die auf die
# Ansicht selbst zeigen, hätten den Platzhalter getroffen. CLKCustomViewDecoded (appkit/autolayout.m) ersetzt den Platzhalter im
# Unarchiver durch die neue Ansicht und liest dann die Auto-Layout-Schlüssel wie -[NSView initWithCoder:].
s/^(@implementation NSCustomView)$/void CLKCustomViewDecoded(NSCoder *coder, id placeholder, NSView *view);\n\n\1/
s/^( *)\[self release\];$/\1CLKCustomViewDecoded(coder, self, newView);\n&/
