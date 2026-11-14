# ClokeOS: Wird das Schlüsselfenster ausgeblendet (orderOut, Panels nach runModal), wird wie bei macOS das Hauptfenster oder sonst das
# nächste sichtbare Fenster, das es kann, zum Schlüsselfenster. Bisher blieb das unsichtbare Panel key und schluckte alle Tasten
# (Logic Pro: nach einem Export gingen Pfeiltasten und Cmd+E ans geschlossene NSOpenPanel).
/^    case NSWindowOut:$/,/^        break;$/{
/^        \[\[self platformWindow\] hideWindow\];$/a\
        if ([self isKeyWindow]) {\
            NSWindow *next = [NSApp mainWindow];\
            if (next == self || ![next isVisible] || ![next canBecomeKeyWindow]) {\
                next = nil;\
                for (NSWindow *w in [NSApp windows])\
                    if (w != self && [w isVisible] && [w canBecomeKeyWindow]) { next = w; break; }\
            }\
            [self resignKeyWindow];\
            /* NSApp-Schlüsselfenster sofort umsetzen: makeKeyWindow setzt nur den X-Fokus, und das FocusIn kommt bei einem\
               ausgeblendeten Fenster nicht zuverlässig */\
            [NSApp _setKeyWindow: next];\
            if (next != nil) {\
                [next makeKeyWindow];\
                [next becomeKeyWindow];\
            }\
        }
}
# ClokeOS: -nextEventMatchingMask:untilDate:inMode:dequeue: wiederholte die Abfrage, solange kein passendes Ereignis kam; nach
# Ablauf von untilDate liefert NSApp nil, das galt als "passt nicht": die Zeitgrenze wirkte nie. Verfolgungsschleifen, die bei
# stillstehender Maus weiterarbeiten (Rand-Scrollen beim Ziehen, Fundus), blieben stehen. Apple gibt dann nil zurück.
s/\} while \(!\(mask & NSEventMaskFromType\(\[event type\]\)\)\);/} while (event != nil \&\& !(mask \& NSEventMaskFromType([event type])));/
