#include "gop.h"
#ifndef GOP_HOST_TEST
#include "app.h"
#include "console.h"
#endif

/* ---- Kommandozeile ---------------------------------------------------------------------------- */

UINTN clokeos_gop_parse(CONST CHAR8 *cmdline, UINT32 *w, UINT32 *h) {
  static const CHAR8 key[] = "cloke_gop=";
  if (!cmdline)
    return 0;
  for (CONST CHAR8 *p = cmdline; *p; p++) {
    if (p != cmdline && p[-1] != ' ')
      continue;
    UINTN k = 0;
    while (key[k] && p[k] == key[k])
      k++;
    if (key[k])
      continue;
    CONST CHAR8 *v = p + k;
    if (v[0] == 'm' && v[1] == 'a' && v[2] == 'x' && (v[3] == 0 || v[3] == ' '))
      return 1;
    UINT32 a = 0, b = 0;
    while (*v >= '0' && *v <= '9')
      a = a * 10 + (UINT32)(*v++ - '0');
    if (*v != 'x' && *v != 'X')
      return 0;
    v++;
    while (*v >= '0' && *v <= '9')
      b = b * 10 + (UINT32)(*v++ - '0');
    if (!a || !b || (*v && *v != ' '))
      return 0;
    *w = a;
    *h = b;
    return 2;
  }
  return 0;
}

#ifndef GOP_HOST_TEST

static BOOLEAN linear(EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *mi) {
  return mi->PixelFormat == PixelRedGreenBlueReserved8BitPerColor ||
         mi->PixelFormat == PixelBlueGreenRedReserved8BitPerColor;
}

static EFI_GRAPHICS_OUTPUT_PROTOCOL *find_gop(AppContext *ctx) {
  EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = NULL;
  EFI_STATUS st = uefi_call_wrapper(ctx->bs->LocateProtocol, 3, &gEfiGraphicsOutputProtocolGuid, NULL, (VOID **)&gop);
  if (EFI_ERROR(st) || !gop || !gop->Mode || !gop->Mode->Info)
    return NULL;
  return gop;
}

BOOLEAN clokeos_gop_select(AppContext *ctx, CONST CHAR8 *cmdline) {
  UINT32 want_w = 0, want_h = 0;
  UINTN how = clokeos_gop_parse(cmdline, &want_w, &want_h);
  if (!how)
    return FALSE;
  EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = find_gop(ctx);
  if (!gop)
    return FALSE;
  UINT32 best = gop->Mode->MaxMode;
  UINT64 best_px = 0;
  for (UINT32 m = 0; m < gop->Mode->MaxMode; m++) {
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *mi = NULL;
    UINTN sz = 0;
    if (EFI_ERROR(uefi_call_wrapper(gop->QueryMode, 4, gop, m, &sz, &mi)) || !mi || !linear(mi))
      continue;
    UINT64 px = (UINT64)mi->HorizontalResolution * mi->VerticalResolution;
    if (how == 2 && (mi->HorizontalResolution != want_w || mi->VerticalResolution != want_h))
      continue;
    if (px > best_px) {
      best_px = px;
      best = m;
    }
  }
  if (best == gop->Mode->MaxMode) {
    log_info(L"cloke_gop: no linear mode %ux%u, keeping mode %u\r\n", want_w, want_h, gop->Mode->Mode);
    return FALSE;
  }
  if (best == gop->Mode->Mode) {
    log_info(L"cloke_gop: mode %u is already active\r\n", best);
    return FALSE;
  }
  EFI_STATUS st = uefi_call_wrapper(gop->SetMode, 2, gop, best);
  log_info(L"cloke_gop: SetMode(%u) -> %ux%u: %r\r\n", best, gop->Mode->Info->HorizontalResolution,
           gop->Mode->Info->VerticalResolution, st);
  return !EFI_ERROR(st);
}

/* ---- Bericht auf dem Stick -------------------------------------------------------------------- */

typedef struct {
  CHAR8 buf[8192];
  UINTN len;
} Text;

static VOID put(Text *t, CONST CHAR8 *s) {
  while (*s && t->len < sizeof(t->buf) - 1)
    t->buf[t->len++] = *s++;
}

static VOID put_num(Text *t, UINT64 v, UINTN base) {
  CHAR8 d[24];
  UINTN n = 0;
  do {
    UINTN r = (UINTN)(v % base);
    d[n++] = (CHAR8)(r < 10 ? '0' + r : 'a' + r - 10);
    v /= base;
  } while (v && n < sizeof(d));
  if (base == 16)
    put(t, (CONST CHAR8 *)"0x");
  while (n && t->len < sizeof(t->buf) - 1)
    t->buf[t->len++] = d[--n];
}

static CONST CHAR8 *fmt_name(EFI_GRAPHICS_PIXEL_FORMAT f) {
  switch (f) {
  case PixelRedGreenBlueReserved8BitPerColor: return (CONST CHAR8 *)"RGBX";
  case PixelBlueGreenRedReserved8BitPerColor: return (CONST CHAR8 *)"BGRX";
  case PixelBitMask: return (CONST CHAR8 *)"bitmask";
  case PixelBltOnly: return (CONST CHAR8 *)"blt-only";
  default: return (CONST CHAR8 *)"?";
  }
}

static VOID write_file(EFI_FILE_PROTOCOL *dir, CONST CHAR16 *name, VOID *data, UINTN len) {
  EFI_FILE_PROTOCOL *f = NULL;
  UINT64 mode = EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE;
  if (EFI_ERROR(uefi_call_wrapper(dir->Open, 5, dir, &f, (CHAR16 *)name, mode, 0)))
    return;
  uefi_call_wrapper(f->Delete, 1, f); /* ältere, längere Datei nicht stehen lassen */
  if (EFI_ERROR(uefi_call_wrapper(dir->Open, 5, dir, &f, (CHAR16 *)name, mode, 0)))
    return;
  uefi_call_wrapper(f->Write, 3, f, &len, data);
  uefi_call_wrapper(f->Close, 1, f);
}

VOID clokeos_gop_dump(AppContext *ctx) {
  static Text t;
  t.len = 0;
  put(&t, (CONST CHAR8 *)"ClokeOS loader: GOP\n");
  EFI_HANDLE *handles = NULL;
  UINTN n = 0;
  EFI_STATUS st = uefi_call_wrapper(ctx->bs->LocateHandleBuffer, 5, ByProtocol, &gEfiGraphicsOutputProtocolGuid,
                                    NULL, &n, &handles);
  if (EFI_ERROR(st))
    n = 0;
  UINT8 *edid = NULL;
  UINT32 edid_len = 0;
  for (UINTN i = 0; i < n; i++) {
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = NULL;
    if (EFI_ERROR(uefi_call_wrapper(ctx->bs->HandleProtocol, 3, handles[i], &gEfiGraphicsOutputProtocolGuid,
                                    (VOID **)&gop)) || !gop || !gop->Mode || !gop->Mode->Info)
      continue;
    put(&t, (CONST CHAR8 *)"gop "), put_num(&t, i, 10);
    put(&t, (CONST CHAR8 *)": mode "), put_num(&t, gop->Mode->Mode, 10);
    put(&t, (CONST CHAR8 *)" of "), put_num(&t, gop->Mode->MaxMode, 10);
    put(&t, (CONST CHAR8 *)", "), put_num(&t, gop->Mode->Info->HorizontalResolution, 10);
    put(&t, (CONST CHAR8 *)"x"), put_num(&t, gop->Mode->Info->VerticalResolution, 10);
    put(&t, (CONST CHAR8 *)" stride "), put_num(&t, gop->Mode->Info->PixelsPerScanLine, 10);
    put(&t, (CONST CHAR8 *)" "), put(&t, fmt_name(gop->Mode->Info->PixelFormat));
    put(&t, (CONST CHAR8 *)", framebuffer "), put_num(&t, gop->Mode->FrameBufferBase, 16);
    put(&t, (CONST CHAR8 *)" size "), put_num(&t, gop->Mode->FrameBufferSize, 16);
    put(&t, (CONST CHAR8 *)"\n");
    for (UINT32 m = 0; m < gop->Mode->MaxMode; m++) {
      EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *mi = NULL;
      UINTN sz = 0;
      if (EFI_ERROR(uefi_call_wrapper(gop->QueryMode, 4, gop, m, &sz, &mi)) || !mi)
        continue;
      put(&t, (CONST CHAR8 *)"  mode "), put_num(&t, m, 10);
      put(&t, (CONST CHAR8 *)": "), put_num(&t, mi->HorizontalResolution, 10);
      put(&t, (CONST CHAR8 *)"x"), put_num(&t, mi->VerticalResolution, 10);
      put(&t, (CONST CHAR8 *)" "), put(&t, fmt_name(mi->PixelFormat));
      put(&t, (CONST CHAR8 *)"\n");
    }
    /* EDID: aktiv (was gerade angezeigt wird) vor erkannt */
    EFI_EDID_ACTIVE_PROTOCOL *ea = NULL;
    EFI_EDID_DISCOVERED_PROTOCOL *ed = NULL;
    if (!edid && !EFI_ERROR(uefi_call_wrapper(ctx->bs->HandleProtocol, 3, handles[i], &gEfiEdidActiveProtocolGuid,
                                              (VOID **)&ea)) && ea && ea->SizeOfEdid) {
      edid = ea->Edid, edid_len = ea->SizeOfEdid;
      put(&t, (CONST CHAR8 *)"  EDID (active): "), put_num(&t, edid_len, 10), put(&t, (CONST CHAR8 *)" bytes\n");
    }
    if (!edid && !EFI_ERROR(uefi_call_wrapper(ctx->bs->HandleProtocol, 3, handles[i], &gEfiEdidDiscoveredProtocolGuid,
                                              (VOID **)&ed)) && ed && ed->SizeOfEdid) {
      edid = ed->Edid, edid_len = ed->SizeOfEdid;
      put(&t, (CONST CHAR8 *)"  EDID (discovered): "), put_num(&t, edid_len, 10), put(&t, (CONST CHAR8 *)" bytes\n");
    }
    /* Native Auflösung aus dem ersten Detailed Timing Descriptor (Byte 54) */
    if (edid && edid_len >= 128) {
      UINT8 *d = edid + 54;
      UINT32 clk = (UINT32)(d[0] | d[1] << 8) * 10; /* kHz */
      UINT32 ha = d[2] | (UINT32)(d[4] & 0xf0) << 4, va = d[5] | (UINT32)(d[7] & 0xf0) << 4;
      put(&t, (CONST CHAR8 *)"  EDID native: "), put_num(&t, ha, 10), put(&t, (CONST CHAR8 *)"x"), put_num(&t, va, 10);
      put(&t, (CONST CHAR8 *)", pixel clock "), put_num(&t, clk, 10), put(&t, (CONST CHAR8 *)" kHz\n");
    }
  }
  if (!n)
    put(&t, (CONST CHAR8 *)"no GOP\n");
  if (handles)
    uefi_call_wrapper(ctx->bs->FreePool, 1, handles);

  EFI_FILE_PROTOCOL *root = NULL, *dir = NULL;
  if (EFI_ERROR(app_open_self_volume(ctx, &root)))
    return;
  if (!EFI_ERROR(uefi_call_wrapper(root->Open, 5, root, &dir, L"\\ClokeOS",
                                   EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, EFI_FILE_DIRECTORY))) {
    write_file(dir, L"gop.txt", t.buf, t.len);
    if (edid)
      write_file(dir, L"edid.bin", edid, edid_len);
    uefi_call_wrapper(dir->Close, 1, dir);
    log_info(L"GOP: report written to \\ClokeOS\\gop.txt\r\n");
  }
  uefi_call_wrapper(root->Close, 1, root);
}

#endif
