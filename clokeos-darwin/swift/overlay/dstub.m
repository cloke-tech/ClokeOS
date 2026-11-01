/* Klassen, die Swift-Dispatch-Code als ObjC-Klassen anspricht; PureDarwins libdispatch hat keine (nur Platzhalter) */
#define C(n) __attribute__((objc_root_class)) @interface n { Class isa; } @end @implementation n @end
C(OS_dispatch_data) C(OS_dispatch_group) C(OS_dispatch_queue) C(OS_dispatch_source)
