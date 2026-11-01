# ClokeOS: CLOKE_NO_UNTITLED=1 überspringt das Öffnen eines leeren Dokuments in -finishLaunching. Logic Pro hat beim Start keinen Delegaten
# (applicationShouldOpenUntitledFile: kann nicht NO sagen), sein Dokumenten-Fabrikmethode liefert nil, und AppKit zeigt eine Fehlertafel
# ohne Text, bevor didFinishLaunching kommt.
s/^( *)BOOL needsUntitled = YES;/\1BOOL needsUntitled = (getenv("CLOKE_NO_UNTITLED") == NULL);/
