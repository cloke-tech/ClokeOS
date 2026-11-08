# ClokeOS: Attributschlüssel mit Apples Werten ("NSFont", "NSColor", "NSParagraphStyle" …). Cocotron nahm "NSFontAttributeName" usw.;
# Programme mit Apples Zeichenketten (Logic Pro, Nib-Archive, CoreText kCTFontAttributeName = "NSFont") verloren dadurch Schrift,
# Farbe und Absatzstil (Logics LCD-Beschriftungen in 12 pt Helvetica statt 8 pt, nicht zentriert).
s/^(NSAttributedStringKey NSFontAttributeName = @)"NSFontAttributeName";/\1"NSFont";/
s/^(NSAttributedStringKey NSParagraphStyleAttributeName = @)"NSParagraphStyleAttributeName";/\1"NSParagraphStyle";/
s/^(NSAttributedStringKey NSForegroundColorAttributeName = @)"NSForegroundColorAttributeName";/\1"NSColor";/
s/^(NSAttributedStringKey NSBackgroundColorAttributeName = @)"NSBackgroundColorAttributeName";/\1"NSBackgroundColor";/
s/^(NSAttributedStringKey NSUnderlineStyleAttributeName = @)"NSUnderlineStyleAttributeName";/\1"NSUnderline";/
s/^(NSAttributedStringKey NSUnderlineColorAttributeName = @)"NSUnderlineColorAttributeName";/\1"NSUnderlineColor";/
s/^(NSAttributedStringKey NSAttachmentAttributeName = @)"NSAttachmentAttributeName";/\1"NSAttachment";/
s/^(NSAttributedStringKey NSKernAttributeName = @)"NSKernAttributeName";/\1"NSKern";/
s/^(NSAttributedStringKey NSLigatureAttributeName = @)"NSLigatureAttributeName";/\1"NSLigature";/
s/^(NSAttributedStringKey NSStrikethroughStyleAttributeName = @)"NSStrikethroughStyleAttributeName";/\1"NSStrikethrough";/
s/^(NSAttributedStringKey NSStrikethroughColorAttributeName = @)"NSStrikethroughColorAttributeName";/\1"NSStrikethroughColor";/
s/^(NSAttributedStringKey NSObliquenessAttributeName = @)"NSObliquenessAttributeName";/\1"NSObliqueness";/
s/^(NSAttributedStringKey NSStrokeWidthAttributeName = @)"NSStrokeWidthAttributeName";/\1"NSStrokeWidth";/
s/^(NSAttributedStringKey NSStrokeColorAttributeName = @)"NSStrokeColorAttributeName";/\1"NSStrokeColor";/
s/^(NSAttributedStringKey NSBaselineOffsetAttributeName = @)"NSBaselineOffsetAttributeName";/\1"NSBaselineOffset";/
s/^(NSAttributedStringKey NSSuperscriptAttributeName = @)"NSSuperscriptAttributeName";/\1"NSSuperScript";/
s/^(NSAttributedStringKey NSLinkAttributeName = @)"NSLinkAttributeName";/\1"NSLink";/
s/^(NSAttributedStringKey NSShadowAttributeName = @)"NSShadowAttributeName";/\1"NSShadow";/
s/^(NSAttributedStringKey NSExpansionAttributeName = @)"NSExpansionAttributeName";/\1"NSExpansion";/
s/^(NSAttributedStringKey NSCursorAttributeName = @)"NSCursorAttributeName";/\1"NSCursor";/
s/^(NSAttributedStringKey NSToolTipAttributeName = @)"NSToolTipAttributeName";/\1"NSToolTip";/
s/^(NSAttributedStringKey NSSpellingStateAttributeName = @)"NSSpellingStateAttributeName";/\1"NSSpellingState";/
