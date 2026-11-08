/* Ergänzungen für Darlings Foundation auf ClokeOS.
 *
 * 1. Zahlen-Literale: Apples clang übersetzt @42, @(3.5), @(2.5f) ab macOS 11 als Ziel in feste
 *    Objekte im Programm. Apple legt deren Klassen in Foundation; Darling hat sie nicht.
 *      NSConstantIntegerNumber { isa, const char *encoding, long long value }
 *      NSConstantDoubleNumber  { isa, double value }
 *      NSConstantFloatNumber   { isa, float value }
 *    (Layout aus clangs Ausgabe, Abschnitte __objc_intobj, __objc_doubleobj, __objc_floatobj.)
 * 2. -[NSThread initWithBlock:] und +detachNewThreadWithBlock: (seit macOS 10.12), fehlen bei Darling.
 * 3. Platzhalter für NSURLRequest, NSURLResponse, NSHTTPURLResponse und NSURLConnection. Auf macOS
 *    kommen sie aus CFNetwork, das Foundation re-exportiert. Für die Foundation-Probe gibt es kein
 *    CFNetwork; Laden über http(s) meldet deshalb einen Fehler, file:-URLs gehen an NSData vorbei.
 * 4. NSNumberFormatter: minimum, maximum, attributedStringForZero/Nil/NotANumber (gespeicherte Werte; nur eingetragen, wo Darling sie
 *    nicht hat). Logic Pros Transportanzeige (TransportTempoFormatter, TransportDisplayClockFormatter) fragt sie ab; ohne sie bekam sie
 *    nil und zeigte statt Position und Tempo nur Striche. Anders als bei Apple prüft getObjectValue: die Grenzen nicht. */
#import <Foundation/Foundation.h>
#import <Foundation/NSURLConnection.h>
#import <Foundation/NSURLRequest.h>
#import <Foundation/NSURLResponse.h>
#include <CoreFoundation/CFNumber.h>
#import <objc/runtime.h>

#define CLOKE_IMMORTAL                                        \
  -(id)retain { return self; }                                \
  -(oneway void)release {}                                    \
  -(id)autorelease { return self; }                           \
  -(NSUInteger)retainCount { return NSUIntegerMax; }          \
  -(id)copyWithZone:(NSZone *)zone { return self; }           \
  -(void)dealloc { if (0) [super dealloc]; }

/* gemeinsame Zugriffe über long long bzw. double */
#define CLOKE_NUMBER_ACCESSORS(LL, DBL)                                      \
  -(char)charValue { return (char)(LL); }                                    \
  -(unsigned char)unsignedCharValue { return (unsigned char)(LL); }          \
  -(short)shortValue { return (short)(LL); }                                 \
  -(unsigned short)unsignedShortValue { return (unsigned short)(LL); }       \
  -(int)intValue { return (int)(LL); }                                       \
  -(unsigned int)unsignedIntValue { return (unsigned int)(LL); }             \
  -(long)longValue { return (long)(LL); }                                    \
  -(unsigned long)unsignedLongValue { return (unsigned long)(LL); }          \
  -(long long)longLongValue { return (long long)(LL); }                      \
  -(unsigned long long)unsignedLongLongValue { return (unsigned long long)(LL); } \
  -(NSInteger)integerValue { return (NSInteger)(LL); }                       \
  -(NSUInteger)unsignedIntegerValue { return (NSUInteger)(LL); }             \
  -(float)floatValue { return (float)(DBL); }                                \
  -(double)doubleValue { return (double)(DBL); }                             \
  -(BOOL)boolValue { return (DBL) != 0; }

@interface NSConstantIntegerNumber : NSNumber {
  const char *_encoding;
  long long _value;
}
@end
@interface NSConstantDoubleNumber : NSNumber {
  double _value;
}
@end
@interface NSConstantFloatNumber : NSNumber {
  float _value;
}
@end

static BOOL isFloating(NSNumber *n) {
  const char *t = [n objCType];
  return t && (*t == 'd' || *t == 'f');
}
static NSComparisonResult compareNumbers(NSNumber *a, NSNumber *b) {
  if (isFloating(a) || isFloating(b)) {
    double x = [a doubleValue], y = [b doubleValue];
    return x < y ? NSOrderedAscending : x > y ? NSOrderedDescending : NSOrderedSame;
  }
  long long x = [a longLongValue], y = [b longLongValue];
  return x < y ? NSOrderedAscending : x > y ? NSOrderedDescending : NSOrderedSame;
}
#define CLOKE_NUMBER_COMMON                                                                   \
  -(NSComparisonResult)compare:(NSNumber *)other { return compareNumbers(self, other); }      \
  -(BOOL)isEqualToNumber:(NSNumber *)other { return other && compareNumbers(self, other) == NSOrderedSame; } \
  -(BOOL)isEqual:(id)other {                                                                  \
    return self == other || ([other isKindOfClass:[NSNumber class]] && [self isEqualToNumber:other]); } \
  -(NSString *)description { return [self stringValue]; }                                     \
  -(NSString *)descriptionWithLocale:(id)locale { return [self stringValue]; }                \
  -(NSComparisonResult)_reverseCompare:(NSNumber *)other { return compareNumbers(other, self); }

@implementation NSConstantIntegerNumber
CLOKE_IMMORTAL
CLOKE_NUMBER_ACCESSORS(_value, _value)
CLOKE_NUMBER_COMMON
- (const char *)objCType { return _encoding; }
- (void)getValue:(void *)buffer {
  switch (_encoding ? *_encoding : 'q') {
    case 'c': case 'C': case 'B': *(char *)buffer = (char)_value; break;
    case 's': case 'S': *(short *)buffer = (short)_value; break;
    case 'i': case 'I': *(int *)buffer = (int)_value; break;
    default: *(long long *)buffer = _value; break;
  }
}
- (CFNumberType)_cfNumberType {
  switch (_encoding ? *_encoding : 'q') {
    case 'c': case 'C': case 'B': return kCFNumberCharType;
    case 's': case 'S': return kCFNumberShortType;
    case 'i': case 'I': return kCFNumberIntType;
    case 'l': return kCFNumberLongType;
    default: return kCFNumberLongLongType;
  }
}
- (NSString *)stringValue {
  if (_encoding && (*_encoding == 'Q' || *_encoding == 'L'))
    return [NSString stringWithFormat:@"%llu", (unsigned long long)_value];
  return [NSString stringWithFormat:@"%lld", _value];
}
- (NSUInteger)hash { return (NSUInteger)_value; }
@end

@implementation NSConstantDoubleNumber
CLOKE_IMMORTAL
CLOKE_NUMBER_ACCESSORS(_value, _value)
CLOKE_NUMBER_COMMON
- (const char *)objCType { return "d"; }
- (void)getValue:(void *)buffer { *(double *)buffer = _value; }
- (CFNumberType)_cfNumberType { return kCFNumberDoubleType; }
- (NSString *)stringValue { return [NSString stringWithFormat:@"%.17g", _value]; }
- (NSUInteger)hash { return (NSUInteger)_value; }
@end

@implementation NSConstantFloatNumber
CLOKE_IMMORTAL
CLOKE_NUMBER_ACCESSORS(_value, _value)
CLOKE_NUMBER_COMMON
- (const char *)objCType { return "f"; }
- (void)getValue:(void *)buffer { *(float *)buffer = _value; }
- (CFNumberType)_cfNumberType { return kCFNumberFloatType; }
- (NSString *)stringValue { return [NSString stringWithFormat:@"%.9g", (double)_value]; }
- (NSUInteger)hash { return (NSUInteger)_value; }
@end

/* --- NSThread mit Block ------------------------------------------------------------------------ */

@interface ClokeThreadBlock : NSObject {
@public
  void (^_block)(void);
}
- (void)run:(id)unused;
@end
@implementation ClokeThreadBlock
- (void)run:(id)unused { _block(); }
- (void)dealloc { [_block release]; [super dealloc]; }
@end

@implementation NSThread (ClokeBlocks)
- (instancetype)initWithBlock:(void (^)(void))block {
  ClokeThreadBlock *b = [[ClokeThreadBlock new] autorelease];
  b->_block = [block copy];
  return [self initWithTarget:b selector:@selector(run:) object:nil];
}
+ (void)detachNewThreadWithBlock:(void (^)(void))block {
  [[[[NSThread alloc] initWithBlock:block] autorelease] start];
}
@end

/* --- URL-Laden: Platzhalter ohne CFNetwork ----------------------------------------------------- */

@implementation NSURLRequest
+ (id)requestWithURL:(NSURL *)URL { return [[[self alloc] initWithURL:URL] autorelease]; }
- (id)initWithURL:(NSURL *)URL {
  if ((self = [super init])) _internal = (id)[URL retain];
  return self;
}
- (void)dealloc { [(id)_internal release]; [super dealloc]; }
- (NSURL *)URL { return (NSURL *)_internal; }
- (id)copyWithZone:(NSZone *)zone { return [self retain]; }
@end

@implementation NSURLResponse
@end

@implementation NSHTTPURLResponse
+ (BOOL)isErrorStatusCode:(NSInteger)statusCode { return statusCode >= 400; }
+ (NSString *)localizedStringForStatusCode:(NSInteger)statusCode {
  return [NSString stringWithFormat:@"HTTP %ld", (long)statusCode];
}
- (NSInteger)statusCode { return 0; }
@end

@implementation NSURLConnection
+ (NSData *)sendSynchronousRequest:(NSURLRequest *)request returningResponse:(NSURLResponse **)response
                             error:(NSError **)error {
  if (response) *response = nil;
  if (error)
    *error = [NSError errorWithDomain:NSURLErrorDomain code:NSURLErrorUnsupportedURL
                             userInfo:@{NSLocalizedDescriptionKey : @"ClokeOS-Foundation-Probe: kein CFNetwork, URL-Laden nicht möglich"}];
  return nil;
}
@end

/* --- 4. NSNumberFormatter: Grenzen und Texte für 0, nil, NaN ----------------------------------------------------------------------- */
static char nf_min, nf_max, nf_zero, nf_nil, nf_nan;
static id nf_get(id self, SEL s) {
    const char *n = sel_getName(s);
    char *k = strcmp(n, "minimum") == 0 ? &nf_min : strcmp(n, "maximum") == 0 ? &nf_max : strcmp(n, "attributedStringForZero") == 0 ? &nf_zero
            : strcmp(n, "attributedStringForNil") == 0 ? &nf_nil : &nf_nan;
    return objc_getAssociatedObject(self, k);
}
static void nf_set(id self, SEL s, id v) {
    const char *n = sel_getName(s);
    char *k = strcmp(n, "setMinimum:") == 0 ? &nf_min : strcmp(n, "setMaximum:") == 0 ? &nf_max : strcmp(n, "setAttributedStringForZero:") == 0 ? &nf_zero
            : strcmp(n, "setAttributedStringForNil:") == 0 ? &nf_nil : &nf_nan;
    objc_setAssociatedObject(self, k, v, OBJC_ASSOCIATION_COPY_NONATOMIC);
}
__attribute__((constructor)) static void clk_numberformatter_init(void) {
    Class C = [NSNumberFormatter class];
    const char *names[] = { "minimum", "maximum", "attributedStringForZero", "attributedStringForNil", "attributedStringForNotANumber" };
    const char *setters[] = { "setMinimum:", "setMaximum:", "setAttributedStringForZero:", "setAttributedStringForNil:", "setAttributedStringForNotANumber:" };
    for (unsigned i = 0; i < 5; i++) {
        class_addMethod(C, sel_registerName(names[i]), (IMP) nf_get, "@@:");
        class_addMethod(C, sel_registerName(setters[i]), (IMP) nf_set, "v@:@");
    }
}
