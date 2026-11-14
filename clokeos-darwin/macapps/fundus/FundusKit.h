// FundusKit: die Finder-Ansicht von ClokeOS (docs/fundus.md). Benutzt von Fundus.app und von den Öffnen/Sichern-
// Panels aller Mac-Programme (AppKit lädt FundusKit und ruft +[FKPanel fk_runPanelWithOptions:]).
#import <Cocoa/Cocoa.h>

typedef NS_ENUM(NSInteger, FKViewMode) { FKViewModeIcons = 0, FKViewModeList = 1 };
typedef NS_ENUM(NSInteger, FKPurpose) { FKPurposeWindow = 0, FKPurposeOpen = 1, FKPurposeSave = 2 };

@class FKBrowserView;

@protocol FKBrowserDelegate <NSObject>
@optional
- (void)browserDidChangeDirectory:(FKBrowserView *)browser;
- (void)browserSelectionDidChange:(FKBrowserView *)browser;
// Doppelklick auf eine Datei (nicht Ordner): YES, wenn der Delegat sie übernimmt (Panel: auswählen und schließen)
- (BOOL)browser:(FKBrowserView *)browser openFile:(NSString *)path;
@end

@interface FKBrowserView : NSView
@property (weak) id<FKBrowserDelegate> delegate;
@property (nonatomic) FKPurpose purpose;
@property (nonatomic, copy, readonly) NSString *directory;
@property (nonatomic) FKViewMode viewMode;
@property (nonatomic) BOOL showHidden;
@property (nonatomic, copy) NSArray *allowedExtensions;   // Panels: andere Dateien grau (nil = alle)
@property (nonatomic) BOOL canChooseFiles, canChooseDirectories, allowsMultipleSelection;
- (instancetype)initWithFrame:(NSRect)frame purpose:(FKPurpose)purpose;
- (void)goTo:(NSString *)path;
- (void)goBack;
- (void)goForward;
- (void)goUp;
- (BOOL)canGoBack;
- (BOOL)canGoForward;
- (void)reload;
- (NSArray *)selectedPaths;
- (void)selectPath:(NSString *)path;
- (void)newFolder;
- (void)renameSelection;
- (void)trashSelection;
- (void)duplicateSelection;
- (void)copySelection;
- (void)cutSelection;
- (void)paste;
- (void)openSelection;
- (void)selectAll;
- (void)focusSearch;
- (void)openInTerminal;
- (void)emptyTrash;
- (NSString *)title;   // Name des aktuellen Ordners wie im Finder-Titel
@end

@interface FKPanel : NSObject
// Optionen: purpose ("open"/"save"), directory, name, types (Endungen), multiple, files, dirs, title, prompt,
// message, accessory (NSView), createDirs. Ergebnis: Array von Pfaden oder nil (Abbrechen).
+ (NSArray *)fk_runPanelWithOptions:(NSDictionary *)options;
@end

NSString *FKLocalized(NSString *key);
void FKHideTitlebar(NSWindow *window);   // Inhalt bis oben, Ampel darüber (keine eigene Fensterleiste)
NSString *FKLanguage(void);
BOOL fkExists(NSString *path, BOOL *isDir);
