# ClokeOS: diese vier waren STUBs ohne Wirkung; macapps/appkit/cggradient.m setzt sie um (zusammen mit CGGradient)
/^void CGContextDrawLinearGradient\(CGContextRef c,$/,/^\}$/d
/^void CGContextDrawRadialGradient\(CGContextRef c,$/,/^\}$/d
/^void CGContextDrawTiledImage\(CGContextRef c, CGRect rect, CGImageRef image\)$/,/^\}$/d
/^void CGContextShowGlyphsAtPositions\(CGContextRef c,$/,/^\}$/d
