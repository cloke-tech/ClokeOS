// provides: MTLCreateSystemDefaultDevice MTLCopyAllDevices MTLCopyAllDevicesWithObserver MTLRemoveDeviceObserver
// Keine GPU: es gibt kein Metal-Gerät. Programme, die das prüfen, nehmen ihren Software-Weg.
#include <objc/message.h>

// leeres NSArray, über die ObjC-Laufzeit gesucht: das Stub braucht so kein Foundation beim Laden
static void *clk_empty_array(void) {
  Class c = objc_getClass("NSArray");
  return c ? ((void *(*)(Class, SEL))objc_msgSend)(c, sel_registerName("array")) : 0;
}
CLK_EXPORT void *MTLCreateSystemDefaultDevice(void) {
  clk_log("MTLCreateSystemDefaultDevice -> nil (keine GPU)");
  return 0;
}
CLK_EXPORT void *MTLCopyAllDevices(void) {
  clk_log("MTLCopyAllDevices -> leeres Array (keine GPU)");
  return clk_empty_array();
}
// Rückgabe wie bei Apple: Geräteliste (leer), *observer = nil
CLK_EXPORT void *MTLCopyAllDevicesWithObserver(void **observer, void *handler) {
  clk_log("MTLCopyAllDevicesWithObserver -> leeres Array (keine GPU)");
  if (observer) *observer = 0;
  return clk_empty_array();
}
CLK_EXPORT void MTLRemoveDeviceObserver(void *observer) {}
