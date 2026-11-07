# ClokeOS: die Zelle einer Zeile kommt wie bei Apple zuerst vom Delegaten der Tabelle (tableView:dataCellForTableColumn:row:, bei einer
# NSOutlineView outlineView:dataCellForTableColumn:item:); nil heißt die Zelle der Spalte. Cocotron fragte nie, Logic Pros Inspektor zeigte so
# statt seiner Regler, Menüs und Ankreuzfelder nur Text ("Mute: 0", "<NSImage …>"). Gilt für alle Stellen (Zeichnen, Bearbeiten, Klicks).
# Ruft der Delegat selbst dataCellForRow: (für die Vorgabe), bekommt er ohne erneute Frage die Spaltenzelle.
/^- \(NSCell \*\) dataCellForRow: \(NSInteger\) row \{$/,/^\}$/c\
- (NSCell *) dataCellForRow: (NSInteger) row {\
    static BOOL asking;\
    id d = [_tableView delegate];\
    NSCell *cell = nil;\
    if (!asking && d != nil && row >= 0) {\
        asking = YES;\
        @try {\
            if ([_tableView isKindOfClass: [NSOutlineView class]] &&\
                [d respondsToSelector: @selector(outlineView:dataCellForTableColumn:item:)])\
                cell = [d outlineView: (NSOutlineView *) _tableView dataCellForTableColumn: self\
                                 item: [(NSOutlineView *) _tableView itemAtRow: row]];\
            else if ([d respondsToSelector: @selector(tableView:dataCellForTableColumn:row:)])\
                cell = [d tableView: _tableView dataCellForTableColumn: self row: row];\
        } @finally {\
            asking = NO;\
        }\
    }\
    return cell != nil ? cell : [self dataCell];\
}
