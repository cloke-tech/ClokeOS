# ClokeOS: Verbindungsarten, die Cocotron nicht kennt (z. B. NSNibAXAttributeConnector aus neueren Nibs), tun nichts und werden einmal je
# Klasse gemeldet (CLOKE_NIB_LOG=1). Vorher warf die Basisklasse eine Ausnahme, und alle folgenden Verbindungen der Nib fehlten.
/^- \(void\) establishConnection \{$/{
n
s/NSInvalidAbstractInvocation\(\);/static NSMutableSet *seen; if (!seen) seen = [NSMutableSet new];\n    NSString *cn = NSStringFromClass([self class]);\n    if (![seen containsObject: cn]) { [seen addObject: cn]; if (getenv("CLOKE_NIB_LOG")) NSLog(@"[clokeos-nib] Verbindung %@ nicht umgesetzt", cn); }/
}
s/^(#import <AppKit\/NSRaise.h>)$/\1\n#include <stdlib.h>/
