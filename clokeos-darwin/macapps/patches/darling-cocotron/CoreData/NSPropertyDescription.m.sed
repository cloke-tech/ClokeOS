# ClokeOS: Modelle im Code bauen (Logic Pros Audio-Unit-Zwischenspeicher, MAAudioUnitSupport): init, Name, optional, transient und
# userInfo setzen. Cocotron las Modelle nur aus .mom-Dateien; init war abstrakt, die Setter nicht umgesetzt.
# optional/transient/userInfo hängen als zugeordnete Objekte am Objekt (die Klasse hat dafür keine Felder).
s/^(#import <Foundation\/NSRaise.h>)$/\1\n#import <objc\/runtime.h>\nstatic char clk_optional, clk_transient, clk_userinfo;/
/^- init \{$/{
n
s/^    NSInvalidAbstractInvocation\(\);$/    self = [super init];\n    objc_setAssociatedObject(self, \&clk_optional, [NSNumber numberWithBool: YES], OBJC_ASSOCIATION_RETAIN);\n    return self;/
n
s/^    return nil;$//
}
/^- \(BOOL\) isOptional \{$/,/^\}$/s/^    NSUnimplementedMethod\(\);$/    return objc_getAssociatedObject(self, \&clk_optional) != nil;/
/^- \(BOOL\) isTransient \{$/,/^\}$/s/^    NSUnimplementedMethod\(\);$/    return objc_getAssociatedObject(self, \&clk_transient) != nil;/
/^- \(NSDictionary \*\) userInfo \{$/,/^\}$/s/^    NSUnimplementedMethod\(\);$/    return objc_getAssociatedObject(self, \&clk_userinfo);/
/^- \(NSArray \*\) validationPredicates \{$/,/^\}$/s/^    NSUnimplementedMethod\(\);$/    return [NSArray array];/
/^- \(NSArray \*\) validationWarnings \{$/,/^\}$/s/^    NSUnimplementedMethod\(\);$/    return [NSArray array];/
/^- \(void\) setName: \(NSString \*\) value \{$/,/^\}$/s/^    NSUnimplementedMethod\(\);$/    value = [value copy];\n    [_propertyName release];\n    _propertyName = value;/
/^- \(void\) setOptional: \(BOOL\) value \{$/,/^\}$/s/^    NSUnimplementedMethod\(\);$/    objc_setAssociatedObject(self, \&clk_optional, value ? [NSNumber numberWithBool: YES] : nil, OBJC_ASSOCIATION_RETAIN);/
/^- \(void\) setTransient: \(BOOL\) value \{$/,/^\}$/s/^    NSUnimplementedMethod\(\);$/    objc_setAssociatedObject(self, \&clk_transient, value ? [NSNumber numberWithBool: YES] : nil, OBJC_ASSOCIATION_RETAIN);/
/^- \(void\) setUserInfo: \(NSDictionary \*\) value \{$/,/^\}$/s/^    NSUnimplementedMethod\(\);$/    objc_setAssociatedObject(self, \&clk_userinfo, value, OBJC_ASSOCIATION_COPY);/
/^          withValidationWarnings: \(NSArray \*\) warnings$/,/^\}$/s/^    NSUnimplementedMethod\(\);$/    \/* Prüfregeln werden nicht ausgewertet *\//
s/^@end$/\/* ClokeOS: Rückverweis auf die Entität, gesetzt von -[NSEntityDescription setProperties:] *\/\n- (void) _clkSetEntity: (NSEntityDescription *) entity {\n    _entity = entity;\n}\n\n@end/
