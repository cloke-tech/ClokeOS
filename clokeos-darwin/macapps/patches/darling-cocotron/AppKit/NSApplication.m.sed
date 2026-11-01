# ClokeOS: CLOKE_NO_UNTITLED=1 überspringt das Öffnen eines leeren Dokuments in -finishLaunching. Logic Pro hat beim Start keinen Delegaten
# (applicationShouldOpenUntitledFile: kann nicht NO sagen), sein Dokumenten-Fabrikmethode liefert nil, und AppKit zeigt eine Fehlertafel
# ohne Text, bevor didFinishLaunching kommt.
s/^( *)BOOL needsUntitled = YES;/\1BOOL needsUntitled = (getenv("CLOKE_NO_UNTITLED") == NULL);/
# ClokeOS: Apples neue .nib-Dateien (NIBArchive) lädt Cocotron nicht. Logic Pro setzt seinen Delegaten und die Oberfläche in -awakeFromNib des File's Owner
# (CLgApplication), das nur nach dem Laden der Haupt-Nib läuft. CLOKE_AWAKE_APP=1: nach dem fehlgeschlagenen Laden -awakeFromNib selbst senden.
s/^( *)NSLog\(@"Unable to load main nib file %@", nibFile\);/\1{ NSLog(@"Unable to load main nib file %@", nibFile); if (getenv("CLOKE_AWAKE_APP")) [NSApp awakeFromNib]; }/
