/* Klassen, die Swift-Dispatch-Code als ObjC-Klassen anspricht; PureDarwins libdispatch hat keine (nur Platzhalter) */
#import <Foundation/Foundation.h>   /* NSObject als Oberklasse; die Klasse selbst liefert zur Laufzeit Darlings Foundation */
#define C(n) @interface n : NSObject @end @implementation n @end
C(OS_dispatch_data) C(OS_dispatch_group) C(OS_dispatch_queue) C(OS_dispatch_source)

/* Helfer für overlay/Bridging.swift: NSURL-Aufrufe mit NSURLResourceKey-Feldern, ohne die Schlüsseltypen in Swift benennen zu müssen */
#import <Foundation/Foundation.h>
NSDictionary *clk_url_resource_values(NSURL *url, NSArray *keys, NSError **err) { return [url resourceValuesForKeys:keys error:err]; }
BOOL clk_url_set_resource_values(NSURL *url, NSDictionary *values, NSError **err) { return [url setResourceValues:values error:err]; }
NSData *clk_url_bookmark(NSURL *url, NSUInteger options, NSArray *keys, NSURL *relative, NSError **err) {
  return [url bookmarkDataWithOptions:options includingResourceValuesForKeys:keys relativeToURL:relative error:err];
}
NSDirectoryEnumerator *clk_fm_enumerator(NSFileManager *fm, NSURL *url, NSArray *keys, NSUInteger options, BOOL (^handler)(NSURL *, NSError *)) {
  return [fm enumeratorAtURL:url includingPropertiesForKeys:keys options:options errorHandler:handler];
}
