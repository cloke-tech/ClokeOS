# ClokeOS: Versalhöhe und x-Höhe aus der OS/2-Tabelle der Schrift (sCapHeight, sxHeight) wie bei Apple; Cocotron setzte beide auf
# face->height (die ganze Zeilenhöhe, etwa 1,2 em). Fehlt die Tabelle oder ihr Wert (alte OS/2-Fassung < 2), 0,7 em bzw. 0,5 em.
# Logic Pro richtet die Ziffern seiner Zahlenfelder (Transportanzeige, Inspektor) an diesen Maßen aus.
s/^    _capHeight = face->height;$/    {\
        TT_OS2 *clk_os2 = (TT_OS2 *) FT_Get_Sfnt_Table(face, FT_SFNT_OS2);\
        _capHeight = (clk_os2 \&\& clk_os2->version >= 2 \&\& clk_os2->sCapHeight > 0) ? clk_os2->sCapHeight : face->units_per_EM * 0.7;\
        _xHeight = (clk_os2 \&\& clk_os2->version >= 2 \&\& clk_os2->sxHeight > 0) ? clk_os2->sxHeight : face->units_per_EM * 0.5;\
    }/
/^    _xHeight = face->height;$/d
s/^#import <Onyx2D\/O2Font_freetype.h>$/#import <Onyx2D\/O2Font_freetype.h>\
#include <ft2build.h>\
#include FT_TRUETYPE_TABLES_H/
