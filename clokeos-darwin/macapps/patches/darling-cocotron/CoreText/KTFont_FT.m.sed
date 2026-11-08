# ClokeOS: Umrisse von Glyphen (FreeType-Metriken, 26.6-Festkomma, y nach oben wie bei CoreGraphics) für CTFontGetBoundingRectsForGlyphs
/^- \(void\) getAdvancements: \(CGSize \*\) advancements$/i\
- (void) getBoundingRects: (CGRect *) rects forGlyphs: (const CGGlyph *) glyphs count: (NSUInteger) count {\
    O2Font_freetype *o2Font = (O2Font_freetype *) _font;\
    FT_Face face = [o2Font face];\
    FT_Set_Pixel_Sizes(face, _size, _size);\
    for (NSUInteger i = 0; i < count; i++) {\
        if (FT_Load_Glyph(face, glyphs[i], FT_LOAD_DEFAULT | FT_LOAD_NO_BITMAP) != 0) {\
            rects[i] = CGRectZero;\
            continue;\
        }\
        FT_Glyph_Metrics *m = \&face->glyph->metrics;\
        rects[i] = CGRectMake(m->horiBearingX / 64.0, (m->horiBearingY - m->height) / 64.0, m->width / 64.0, m->height / 64.0);\
    }\
}\

