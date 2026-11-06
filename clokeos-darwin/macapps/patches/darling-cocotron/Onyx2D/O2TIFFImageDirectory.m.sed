# ClokeOS: Bildeigenschaften wie bei Apple auch unter "{TIFF}" (ImageDescription, Software, Orientation). Logic Pro liest seine
# GUI-Kataloge so: TIFF-Dateien, deren ImageDescription eine Plist mit den Bildteilen enthält.
s/^(    \[result setObject: \[NSNumber numberWithDouble: _yResolution\])$/    {\n        NSMutableDictionary *tiff = [NSMutableDictionary dictionary];\n        if (_imageDescription) [tiff setObject: _imageDescription forKey: @"ImageDescription"];\n        if (_software) [tiff setObject: _software forKey: @"Software"];\n        if (_orientation) [tiff setObject: [NSNumber numberWithUnsignedInt: _orientation] forKey: @"Orientation"];\n        [result setObject: tiff forKey: @"{TIFF}"];\n    }\n\1/
# ClokeOS: Deflate-komprimierte TIFF (Kompression 8 und 32946, z. B. aus Photoshop; alle GUI-Bilder von Logic Pro) über zlib,
# wie LZW streifenweise. Fehlerhafte Streifen werden zu Nullbytes in der erwarteten Länge.
s/^(#import <Onyx2D\/O2LZW.h>)$/\1\n#include <zlib.h>\nstatic NSData *clk_inflate(NSData *in, unsigned expected) {\n    NSMutableData *out = [NSMutableData dataWithLength: expected];\n    uLongf n = expected;\n    if (uncompress([out mutableBytes], \&n, [in bytes], [in length]) != Z_OK \&\& n == 0)\n        NSLog(@"TIFF deflate error");\n    return out;\n}/
s/^(        _compression != NSTIFFCompression_LZW)\) \{$/\1 \&\& _compression != 8 \&\& _compression != 32946) {/
s/^    if \(_compression == NSTIFFCompression_LZW\) \{$/    if (_compression == NSTIFFCompression_LZW || _compression == 8 || _compression == 32946) {/
s/^( *)data = LZWDecodeWithExpectedResultLength\(data, stripLength\);$/\1data = (_compression == NSTIFFCompression_LZW) ? LZWDecodeWithExpectedResultLength(data, stripLength) : clk_inflate(data, stripLength);/
