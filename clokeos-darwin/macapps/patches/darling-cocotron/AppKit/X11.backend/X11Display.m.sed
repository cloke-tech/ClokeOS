# ClokeOS: Carbon-Tastencodes (NSEvent keyCode) nach dem Keysym der X-Taste statt nach der festen evdev-Nummer. Der X-Server von
# ClokeOS liefert XFree86-Tastencodes: Buchstaben und Ziffern sind gleich, der Pfeil-/Navigationsblock nicht (Pfeil runter = 104 statt
# 116 → bisher "Keypad Enter"). Programme, die nach keyCode gehen (Logic Pro: Spurauswahl mit Pfeilen), bekamen falsche Tasten.
/^#import <stddef.h>$/a\
#import <X11/keysym.h>\
#import <X11/XKBlib.h>\
static int clkCarbonFromKeySym(KeySym ks) {\
    switch (ks) {\
    case XK_Up: return 126; case XK_Down: return 125; case XK_Left: return 123; case XK_Right: return 124;\
    case XK_Home: return 115; case XK_End: return 119; case XK_Prior: return 116; case XK_Next: return 121;\
    case XK_Delete: return 117; case XK_Insert: case XK_Help: return 114; case XK_Return: return 36; case XK_KP_Enter: return 76;\
    case XK_BackSpace: return 51; case XK_Tab: case XK_ISO_Left_Tab: return 48; case XK_Escape: return 53; case XK_space: return 49;\
    case XK_F1: return 122; case XK_F2: return 120; case XK_F3: return 99; case XK_F4: return 118; case XK_F5: return 96;\
    case XK_F6: return 97; case XK_F7: return 98; case XK_F8: return 100; case XK_F9: return 101; case XK_F10: return 109;\
    case XK_F11: return 103; case XK_F12: return 111; case XK_F13: return 105; case XK_F14: return 107; case XK_F15: return 113;\
    case XK_Shift_L: return 56; case XK_Shift_R: return 60; case XK_Control_L: return 59; case XK_Control_R: return 62;\
    case XK_Alt_L: return 58; case XK_Alt_R: return 61; case XK_Super_L: case XK_Meta_L: return 55; case XK_Super_R: case XK_Meta_R: return 54;\
    case XK_Caps_Lock: return 57;\
    default: return -1;\
    }\
}\
static int clkCarbonKeyCode(Display *dpy, unsigned keycode, const uint8_t *table) {\
    static int cache[256]; static BOOL filled;\
    if (!filled) {\
        for (int k = 0; k < 256; k++) {\
            int c = k >= 8 ? clkCarbonFromKeySym(XkbKeycodeToKeysym(dpy, (KeyCode) k, 0, 0)) : -1;\
            cache[k] = c >= 0 ? c : table[k];\
        }\
        filled = YES;\
    }\
    return cache[keycode & 255];\
}
s/^        const int carbonKeyCode = x11ToCarbon\[ev->xkey\.keycode\];$/        const int carbonKeyCode = clkCarbonKeyCode(_display, ev->xkey.keycode, x11ToCarbon);/
