# ClokeOS: ansichtsbasierte Gliederungen (macapps/appkit/tableviews.m): keine Datenzellen zeichnen, das Dreieck schon
s/^@implementation NSOutlineView$/BOOL ClkTableIsViewBased(NSTableView *t);\n\n&/
/^- \(void\) drawRow: \(NSInteger\) row clipRect: \(NSRect\) rect \{$/,/^\}$/s/^        \} else \{$/        } else if (!ClkTableIsViewBased(self)) {/
