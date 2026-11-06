# ClokeOS: zu-eins heißt wie bei Apple maxCount == 1 (Cocotron verlangte zusätzlich minCount == 1, im Code gebaute optionale
# zu-eins-Beziehungen galten so als zu-viele). optional für Beziehungen im eigenen Feld.
s/^    return \(_minCount == 1 && _maxCount == 1\) \? NO : YES;$/    return _maxCount == 1 ? NO : YES;/
s/^@end$/- (BOOL) isOptional {\n    return _optional;\n}\n\n- (void) setOptional: (BOOL) value {\n    _optional = value;\n}\n\n@end/
