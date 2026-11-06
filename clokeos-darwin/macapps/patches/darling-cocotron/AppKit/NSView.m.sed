# ClokeOS: Ebenen (Core Animation) zeichnet Cocotron über OpenGL, das es hier nicht gibt; ebenengestützte Ansichten blieben schwarz
# (Logic Pro). Ohne Ebenen zeichnet jede Ansicht wie gewohnt mit drawRect: ins Fenster. CLOKE_VIEW_LAYERS=1 schaltet sie wieder ein.
s|^    //NSViewLayersEnabled = \[defaults boolForKey: @"NSViewLayersEnabled"\];$|    NSViewLayersEnabled = getenv("CLOKE_VIEW_LAYERS") != NULL;|
