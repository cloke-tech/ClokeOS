/* NSURLComponents für Darlings Foundation (dort nur eine leere Klasse mit "TODO").
 * Mac-Programme (Logic Pro) rufen -initWithURL:resolvingAgainstBaseURL: und die Teile beim Start auf.
 *
 * Zerlegung nach RFC 3986: scheme ":" [ "//" [ user [ ":" password ] "@" ] host [ ":" port ] ] path [ "?" query ] [ "#" fragment ].
 * Die Teile werden prozentkodiert gehalten (percentEncoded…); die Zugriffe ohne "percentEncoded" liefern dekodierte
 * Zeichenketten und kodieren beim Setzen wie bei Apple (path, query, user … mit den jeweils erlaubten Zeichen).
 * macapps/patches/darling-foundation/src/NSURLComponents.m.sed nimmt Darlings leere Implementierung heraus. */
#include <ctype.h>
#import <Foundation/NSURLComponents.h>
#import <Foundation/Foundation.h>

@interface NSURLComponents () {
    NSString *_scheme, *_user, *_password, *_host, *_path, *_query, *_fragment;   /* prozentkodiert (außer scheme, host) */
    NSNumber *_port;
}
@end

static NSString *CKEnc(NSString *s, NSCharacterSet *allowed) {
    return s ? [s stringByAddingPercentEncodingWithAllowedCharacters:allowed] : nil;
}
static NSString *CKDec(NSString *s) { return s ? [s stringByRemovingPercentEncoding] : nil; }

static NSCharacterSet *CKSet(NSString *extra) {
    NSMutableCharacterSet *m = [NSMutableCharacterSet alphanumericCharacterSet];
    [m addCharactersInString:@"-._~!$&'()*+,;=" ];
    [m addCharactersInString:extra];
    return m;
}

/* Darlings Foundation hat -stringByAddingPercentEncodingWithAllowedCharacters: und die URL…AllowedCharacterSet nicht (macOS 10.9): hier nachgereicht.
 * Mengen nach RFC 3986 wie bei Apple: unreserved = Buchstaben, Ziffern, "-._~"; sub-delims = "!$&'()*+,;=". */
static NSCharacterSet *CKURLSet(NSString *extra) {
    NSMutableCharacterSet *m = [NSMutableCharacterSet alphanumericCharacterSet];
    [m addCharactersInString:@"-._~!$&'()*+,;="];
    [m addCharactersInString:extra];
    return m;
}

@implementation NSCharacterSet (ClokeURLSets)
+ (NSCharacterSet *)URLUserAllowedCharacterSet { return CKURLSet(@""); }
+ (NSCharacterSet *)URLPasswordAllowedCharacterSet { return CKURLSet(@""); }
+ (NSCharacterSet *)URLHostAllowedCharacterSet { return CKURLSet(@":[]"); }
+ (NSCharacterSet *)URLPathAllowedCharacterSet { return CKURLSet(@":@/"); }
+ (NSCharacterSet *)URLQueryAllowedCharacterSet { return CKURLSet(@":@/?"); }
+ (NSCharacterSet *)URLFragmentAllowedCharacterSet { return CKURLSet(@":@/?"); }
@end

@implementation NSString (ClokeURLEncoding)
- (NSString *)stringByAddingPercentEncodingWithAllowedCharacters:(NSCharacterSet *)allowed {
    NSData *d = [self dataUsingEncoding:NSUTF8StringEncoding];
    if (!d) return nil;
    const unsigned char *b = [d bytes];
    NSUInteger n = [d length];
    NSMutableString *out = [NSMutableString stringWithCapacity:n];
    for (NSUInteger i = 0; i < n; i++) {
        if (b[i] < 0x80 && [allowed characterIsMember:(unichar)b[i]])
            [out appendFormat:@"%c", b[i]];
        else
            [out appendFormat:@"%%%02X", b[i]];
    }
    return out;
}
@end

@implementation NSURLComponents

+ (instancetype)componentsWithURL:(NSURL *)url resolvingAgainstBaseURL:(BOOL)resolve {
    return [[[self alloc] initWithURL:url resolvingAgainstBaseURL:resolve] autorelease];
}
+ (instancetype)componentsWithString:(NSString *)string {
    return [[[self alloc] initWithString:string] autorelease];
}

- (instancetype)init { return [super init]; }

- (instancetype)initWithURL:(NSURL *)url resolvingAgainstBaseURL:(BOOL)resolve {
    if (!url) { [self release]; return nil; }
    NSString *s = resolve ? [[url absoluteURL] absoluteString] : [url relativeString];
    return [self initWithString:s];
}

- (instancetype)initWithString:(NSString *)string {
    if (!string) { [self release]; return nil; }
    self = [super init];
    if (!self) return nil;
    NSString *rest = string;
    NSRange r = [rest rangeOfString:@"#"];
    if (r.location != NSNotFound) { _fragment = [[rest substringFromIndex:r.location + 1] copy]; rest = [rest substringToIndex:r.location]; }
    r = [rest rangeOfString:@"?"];
    if (r.location != NSNotFound) { _query = [[rest substringFromIndex:r.location + 1] copy]; rest = [rest substringToIndex:r.location]; }
    /* scheme: Buchstabe, dann Buchstaben/Ziffern/+-. bis ":" */
    r = [rest rangeOfString:@":"];
    if (r.location != NSNotFound && r.location > 0) {
        NSString *sc = [rest substringToIndex:r.location];
        NSCharacterSet *bad = [[NSCharacterSet characterSetWithCharactersInString:
            @"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789+-."] invertedSet];
        if ([sc rangeOfCharacterFromSet:bad].location == NSNotFound && isalpha([sc characterAtIndex:0])) {
            _scheme = [sc copy];
            rest = [rest substringFromIndex:r.location + 1];
        }
    }
    if ([rest hasPrefix:@"//"]) {
        rest = [rest substringFromIndex:2];
        NSRange sl = [rest rangeOfString:@"/"];
        NSString *auth = sl.location == NSNotFound ? rest : [rest substringToIndex:sl.location];
        rest = sl.location == NSNotFound ? @"" : [rest substringFromIndex:sl.location];
        NSRange at = [auth rangeOfString:@"@" options:NSBackwardsSearch];
        if (at.location != NSNotFound) {
            NSString *ui = [auth substringToIndex:at.location];
            auth = [auth substringFromIndex:at.location + 1];
            NSRange c = [ui rangeOfString:@":"];
            if (c.location != NSNotFound) {
                _user = [[ui substringToIndex:c.location] copy];
                _password = [[ui substringFromIndex:c.location + 1] copy];
            } else _user = [ui copy];
        }
        NSRange c = [auth rangeOfString:@":" options:NSBackwardsSearch];
        if (c.location != NSNotFound && ![auth hasSuffix:@"]"]) {
            NSString *p = [auth substringFromIndex:c.location + 1];
            if ([p length]) _port = [[NSNumber numberWithInteger:[p integerValue]] retain];
            auth = [auth substringToIndex:c.location];
        }
        if ([auth hasPrefix:@"["] && [auth hasSuffix:@"]"]) auth = [auth substringWithRange:NSMakeRange(1, [auth length] - 2)];
        _host = [auth copy];
    }
    _path = [rest copy];
    return self;
}

- (void)dealloc {
    [_scheme release]; [_user release]; [_password release]; [_host release]; [_path release];
    [_query release]; [_fragment release]; [_port release];
    [super dealloc];
}

- (id)copyWithZone:(NSZone *)zone {
    NSURLComponents *c = [[NSURLComponents allocWithZone:zone] initWithString:[self string] ?: @""];
    return c;
}

- (NSString *)string {
    NSMutableString *s = [NSMutableString string];
    if (_scheme) [s appendFormat:@"%@:", _scheme];
    if (_host || _user || _password || _port) {
        [s appendString:@"//"];
        if (_user) { [s appendString:_user]; if (_password) [s appendFormat:@":%@", _password]; [s appendString:@"@"]; }
        if (_host) [s appendString:[_host rangeOfString:@":"].location != NSNotFound ? [NSString stringWithFormat:@"[%@]", _host] : _host];
        if (_port) [s appendFormat:@":%@", _port];
    }
    if (_path) [s appendString:_path];
    if (_query) [s appendFormat:@"?%@", _query];
    if (_fragment) [s appendFormat:@"#%@", _fragment];
    return s;
}

- (NSURL *)URL { return [NSURL URLWithString:[self string]]; }
- (NSURL *)URLRelativeToURL:(NSURL *)base { return [NSURL URLWithString:[self string] relativeToURL:base]; }

/* ---- Zugriffe: dekodiert -------------------------------------------------------------------- */
- (NSString *)scheme { return _scheme; }
- (void)setScheme:(NSString *)v { [_scheme autorelease]; _scheme = [v copy]; }
- (NSString *)host { return CKDec(_host); }
- (void)setHost:(NSString *)v { [_host autorelease]; _host = [CKEnc(v, CKSet(@"")) copy]; }
- (NSString *)user { return CKDec(_user); }
- (void)setUser:(NSString *)v { [_user autorelease]; _user = [CKEnc(v, CKSet(@"")) copy]; }
- (NSString *)password { return CKDec(_password); }
- (void)setPassword:(NSString *)v { [_password autorelease]; _password = [CKEnc(v, CKSet(@"")) copy]; }
- (NSNumber *)port { return _port; }
- (void)setPort:(NSNumber *)v { [_port autorelease]; _port = [v retain]; }
- (NSString *)path { return CKDec(_path); }
- (void)setPath:(NSString *)v { [_path autorelease]; _path = [CKEnc(v, CKSet(@"/:@")) copy]; }
- (NSString *)query { return CKDec(_query); }
- (void)setQuery:(NSString *)v { [_query autorelease]; _query = [CKEnc(v, CKSet(@"/?:@")) copy]; }
- (NSString *)fragment { return CKDec(_fragment); }
- (void)setFragment:(NSString *)v { [_fragment autorelease]; _fragment = [CKEnc(v, CKSet(@"/?:@")) copy]; }

/* ---- Zugriffe: prozentkodiert --------------------------------------------------------------- */
- (NSString *)percentEncodedHost { return _host; }
- (void)setPercentEncodedHost:(NSString *)v { [_host autorelease]; _host = [v copy]; }
- (NSString *)percentEncodedUser { return _user; }
- (void)setPercentEncodedUser:(NSString *)v { [_user autorelease]; _user = [v copy]; }
- (NSString *)percentEncodedPassword { return _password; }
- (void)setPercentEncodedPassword:(NSString *)v { [_password autorelease]; _password = [v copy]; }
- (NSString *)percentEncodedPath { return _path; }
- (void)setPercentEncodedPath:(NSString *)v { [_path autorelease]; _path = [v copy]; }
- (NSString *)percentEncodedQuery { return _query; }
- (void)setPercentEncodedQuery:(NSString *)v { [_query autorelease]; _query = [v copy]; }
- (NSString *)percentEncodedFragment { return _fragment; }
- (void)setPercentEncodedFragment:(NSString *)v { [_fragment autorelease]; _fragment = [v copy]; }

/* ---- queryItems ------------------------------------------------------------------------------ */
static NSString *CKQEnc(NSString *s) { return CKEnc(s, CKSet(@"/?:@")); }

- (NSArray *)percentEncodedQueryItems {
    if (!_query) return nil;
    NSMutableArray *a = [NSMutableArray array];
    for (NSString *pair in [_query componentsSeparatedByString:@"&"]) {
        NSRange e = [pair rangeOfString:@"="];
        NSString *n = e.location == NSNotFound ? pair : [pair substringToIndex:e.location];
        NSString *v = e.location == NSNotFound ? nil : [pair substringFromIndex:e.location + 1];
        [a addObject:[NSURLQueryItem queryItemWithName:n value:v]];
    }
    return a;
}
- (NSArray *)queryItems {
    NSMutableArray *a = [NSMutableArray array];
    for (NSURLQueryItem *i in [self percentEncodedQueryItems])
        [a addObject:[NSURLQueryItem queryItemWithName:CKDec([i name]) value:CKDec([i value])]];
    return _query ? a : nil;
}
- (void)setPercentEncodedQueryItems:(NSArray *)items {
    [_query autorelease]; _query = nil;
    if (!items) return;
    NSMutableArray *parts = [NSMutableArray array];
    for (NSURLQueryItem *i in items)
        [parts addObject:[i value] ? [NSString stringWithFormat:@"%@=%@", [i name], [i value]] : [i name]];
    _query = [[parts componentsJoinedByString:@"&"] copy];
}
- (void)setQueryItems:(NSArray *)items {
    NSMutableArray *enc = nil;
    if (items) {
        enc = [NSMutableArray array];
        for (NSURLQueryItem *i in items)
            [enc addObject:[NSURLQueryItem queryItemWithName:CKQEnc([i name]) value:CKQEnc([i value])]];
    }
    [self setPercentEncodedQueryItems:enc];
}

@end
