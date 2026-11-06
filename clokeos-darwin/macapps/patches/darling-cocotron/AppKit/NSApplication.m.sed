# ClokeOS: CLOKE_NO_UNTITLED=1 überspringt das Öffnen eines leeren Dokuments in -finishLaunching. Logic Pro hat beim Start keinen Delegaten
# (applicationShouldOpenUntitledFile: kann nicht NO sagen), sein Dokumenten-Fabrikmethode liefert nil, und AppKit zeigt eine Fehlertafel
# ohne Text, bevor didFinishLaunching kommt.
s/^( *)BOOL needsUntitled = YES;/\1BOOL needsUntitled = (getenv("CLOKE_NO_UNTITLED") == NULL);/
# ClokeOS: Apples neue .nib-Dateien (NIBArchive) lädt Cocotron nicht. Logic Pro setzt seinen Delegaten und die Oberfläche in -awakeFromNib des File's Owner
# (CLgApplication), das nur nach dem Laden der Haupt-Nib läuft. CLOKE_AWAKE_APP=1: nach dem fehlgeschlagenen Laden -awakeFromNib selbst senden.
s/^( *)NSLog\(@"Unable to load main nib file %@", nibFile\);/\1{ NSLog(@"Unable to load main nib file %@", nibFile); if (getenv("CLOKE_AWAKE_APP")) [NSApp awakeFromNib]; }/
# ClokeOS: stopModal/stopModalWithCode:/abortModal beenden wie bei Apple die gerade laufende Modalschleife (runModalForWindow:/runModalSession:),
# nicht die zuletzt begonnene Sitzung. Logic Pro beginnt während seines Hinweisdialogs eine weitere Sitzung (Fortschrittsfenster); "Continue"
# beendete dann diese, und der Dialog lief unsichtbar weiter.
s/^(- \(NSModalResponse\) runModalSession: \(NSModalSession\) session \{)$/static NSMutableArray *clk_running_sessions;\n\1\n    if (clk_running_sessions == nil)\n        clk_running_sessions = [NSMutableArray new];\n    [clk_running_sessions addObject: session];\n    NSModalResponse clk_result = [self _clkRunModalSession: session];\n    [clk_running_sessions removeObjectIdenticalTo: session];\n    return clk_result;\n}\n\n- (NSModalResponse) _clkRunModalSession: (NSModalSession) session {/
/^- \(void\) stopModalWithCode: \(NSModalResponse\) code \{$/,/^\}$/s/^    \[\[_modalStack lastObject\] stopModalWithCode: code\];$/    id running = [clk_running_sessions lastObject];\n    [(running ? running : [_modalStack lastObject]) stopModalWithCode: code];/
