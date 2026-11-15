/*
 * clokeos_fb.h: Schnittstelle von /dev/clokeos_fb (kext/ClokeIntelGfx, ClokeIntelFB) für den Xorg-Treiber
 * clokeintel (xdriver/clokeintel). Gemeinsam für Kernel und Userland.
 *
 * Der Treiber hat zwei Bildspeicher hintereinander im VRAM-Bereich des IOFramebuffers (Speicher-Typ
 * kIOFBVRAMMemory, den Xorg über IOConnectMapMemory abbildet): Puffer i beginnt bei buf_offset[i].
 * Xorg zeichnet in den gerade nicht angezeigten Puffer und schaltet dann um; die Anzeige übernimmt die
 * neue Adresse erst im nächsten Vblank (PLANE_SURF ist doppelt gepuffert), dadurch reißt das Bild nicht.
 */
#ifndef CLOKEOS_FB_H
#define CLOKEOS_FB_H

#include <stdint.h>
#include <sys/ioccom.h>

#define CLOKEOS_FB_VERSION 1

struct clokeos_fb_info {
  uint32_t version;
  uint32_t width, height, stride; /* Bildpunkte, Bytes je Zeile */
  uint32_t nbuf;                  /* 1 oder 2 */
  uint32_t shown;                 /* Puffer, der gerade angezeigt wird (oder nach dem nächsten Vblank) */
  uint64_t buf_offset[2];         /* im VRAM-Bereich */
  uint64_t vram_size;
  uint32_t refresh_mhz;           /* Bildfrequenz in mHz */
  uint32_t reserved[5];
};

#define CLOKEOS_FB_GET_INFO _IOR('C', 1, struct clokeos_fb_info)
/* Puffer zum Anzeigen vormerken (wird im nächsten Vblank übernommen) */
#define CLOKEOS_FB_FLIP _IOW('C', 2, uint32_t)
/* Warten, bis der vorgemerkte Puffer wirklich angezeigt wird (höchstens 50 ms); 0 = angezeigt */
#define CLOKEOS_FB_WAIT _IOWR('C', 3, uint32_t)

#endif
