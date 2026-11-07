# ClokeOS: Werkzeugleiste in der Titelleiste (docs/titlebar.md): links Platz für die Ampelknöpfe, die CloakServer darüber zeichnet.
# Den Rand liefert das X11-Backend (macapps/appkit/titlebar.m) über diesen Zeiger; ohne Backend bleibt er 5 wie bei Cocotron.
s|^- \(void\) layoutViewsWithWidth: \(CGFloat\) desiredWidth$|CGFloat (*ClokeToolbarLeftMargin)(NSView *) = 0;\
\
- (void) layoutViewsWithWidth: (CGFloat) desiredWidth|
s|^    CGFloat leftMargin = 5, rightMargin = 5;$|    CGFloat leftMargin = ClokeToolbarLeftMargin ? ClokeToolbarLeftMargin(self) : 5, rightMargin = 5;|
