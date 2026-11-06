# ClokeOS: setSelectedSegment:-1 (oder außerhalb) wählt wie bei Apple nichts aus, statt mit einem ungültigen Index in die Segmentliste zu greifen
# (NSRangeException in Logic Pros Transportleiste).
/^- \(void\) setSelectedSegment: \(NSInteger\) segment \{$/,/^\}$/s/^    \[\[_segments objectAtIndex: segment\] setSelected: YES\];$/    if (segment >= 0 \&\& segment < (NSInteger) [_segments count])\n        [[_segments objectAtIndex: segment] setSelected: YES];\n    else\n        _selectedSegment = -1;/
