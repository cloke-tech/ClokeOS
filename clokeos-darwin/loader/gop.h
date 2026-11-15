#ifndef CLOKEOS_GOP_H
#define CLOKEOS_GOP_H

/* ClokeOS: Bildschirmmodus über GOP wählen und für die Fehlersuche festhalten.
 *
 * Boot-Arg cloke_gop=max      größter linearer Modus (meist die native Auflösung des Panels)
 *         cloke_gop=1920x1080  genau dieser Modus, falls die Firmware ihn anbietet
 * Ohne Boot-Arg bleibt der Modus der Firmware (normalerweise nativ).
 *
 * clokeos_gop_dump schreibt \ClokeOS\gop.txt (alle Modi, aktueller Modus, Adresse des
 * Framebuffers) und \ClokeOS\edid.bin (EDID des Panels, falls die Firmware es kennt) auf
 * das Startmedium, wie die ACPI-Tabellen. */

#include "common.h"

/* Gibt TRUE zurück, wenn der Modus gewechselt wurde. */
BOOLEAN clokeos_gop_select(AppContext *ctx, CONST CHAR8 *cmdline);

VOID clokeos_gop_dump(AppContext *ctx);

/* Hilfsfunktion, auch für Tests auf dem Mac: liest cloke_gop= aus der Kommandozeile.
 * Ergebnis: 0 = nicht gesetzt, 1 = max, 2 = feste Größe in *w x *h. */
UINTN clokeos_gop_parse(CONST CHAR8 *cmdline, UINT32 *w, UINT32 *h);

#endif
