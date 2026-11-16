#include "platform/MacPlatform.h"

#include <QCoreApplication>
#include <QPointer>
#include <QWidget>
#include <QWindow>

#import <AppKit/AppKit.h>
#import <QuickLookThumbnailing/QuickLookThumbnailing.h>
#import <ServiceManagement/ServiceManagement.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#import <UserNotifications/UserNotifications.h>

#include <atomic>

// ------------------------------------------------------------------ helpers

namespace {

NSString *ns(const QString &s) { return s.toNSString(); }

NSView *nativeView(QWidget *w)
{
    if (!w)
        return nil;
    QWidget *top = w->window();
    return (__bridge NSView *)reinterpret_cast<void *>(top->winId());
}

QImage imageFromCGImage(CGImageRef cg)
{
    if (!cg)
        return {};
    const size_t w = CGImageGetWidth(cg), h = CGImageGetHeight(cg);
    QImage out(int(w), int(h), QImage::Format_ARGB32_Premultiplied);
    out.fill(Qt::transparent);
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef ctx = CGBitmapContextCreate(out.bits(), w, h, 8, size_t(out.bytesPerLine()), cs,
                                             kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Host);
    CGContextDrawImage(ctx, CGRectMake(0, 0, w, h), cg);
    CGContextRelease(ctx);
    CGColorSpaceRelease(cs);
    return out;
}

CGImageRef cgImageFromQImage(const QImage &src)
{
    const QImage img = src.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef ctx = CGBitmapContextCreate(const_cast<uchar *>(img.constBits()), size_t(img.width()), size_t(img.height()), 8,
                                             size_t(img.bytesPerLine()), cs,
                                             kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Host);
    CGImageRef cg = CGBitmapContextCreateImage(ctx);
    CGContextRelease(ctx);
    CGColorSpaceRelease(cs);
    return cg;
}

// Draws an NSImage into a QImage of pixel size w x h.
QImage renderNSImage(NSImage *image, int w, int h, const QColor *tint = nullptr)
{
    QImage out(w, h, QImage::Format_ARGB32_Premultiplied);
    out.fill(Qt::transparent);
    if (!image || w <= 0 || h <= 0)
        return out;
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef ctx = CGBitmapContextCreate(out.bits(), size_t(w), size_t(h), 8, size_t(out.bytesPerLine()), cs,
                                             kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Host);
    NSGraphicsContext *gc = [NSGraphicsContext graphicsContextWithCGContext:ctx flipped:NO];
    [NSGraphicsContext saveGraphicsState];
    [NSGraphicsContext setCurrentContext:gc];
    [image drawInRect:NSMakeRect(0, 0, w, h) fromRect:NSZeroRect operation:NSCompositingOperationSourceOver fraction:1.0];
    if (tint) {
        CGContextSetBlendMode(ctx, kCGBlendModeSourceIn);
        CGContextSetRGBFillColor(ctx, tint->redF(), tint->greenF(), tint->blueF(), tint->alphaF());
        CGContextFillRect(ctx, CGRectMake(0, 0, w, h));
    }
    [NSGraphicsContext restoreGraphicsState];
    CGContextRelease(ctx);
    CGColorSpaceRelease(cs);
    return out;
}

std::atomic<bool> g_internalDrag{false};
QByteArray g_internalDragData;

} // namespace

// ------------------------------------------------------------------ Obj-C helpers

@interface MDNotificationDelegate : NSObject <UNUserNotificationCenterDelegate>
@property (nonatomic) std::function<void()> onActivate;
@end

@implementation MDNotificationDelegate
- (void)userNotificationCenter:(UNUserNotificationCenter *)center
       willPresentNotification:(UNNotification *)notification
         withCompletionHandler:(void (^)(UNNotificationPresentationOptions))completionHandler
{
    completionHandler(UNNotificationPresentationOptionBanner | UNNotificationPresentationOptionList);
}
- (void)userNotificationCenter:(UNUserNotificationCenter *)center
didReceiveNotificationResponse:(UNNotificationResponse *)response
         withCompletionHandler:(void (^)(void))completionHandler
{
    auto cb = self.onActivate;
    dispatch_async(dispatch_get_main_queue(), ^{
        if (cb)
            cb();
    });
    completionHandler();
}
@end

// File promise provider that also carries our own item list, so drops back
// into MacDroid can be recognised as internal moves.
@interface MDFilePromiseProvider : NSFilePromiseProvider
@property (nonatomic, strong) NSData *internalData;
@end

@implementation MDFilePromiseProvider
- (NSArray<NSPasteboardType> *)writableTypesForPasteboard:(NSPasteboard *)pasteboard
{
    NSMutableArray *types = [[super writableTypesForPasteboard:pasteboard] mutableCopy];
    if (self.internalData)
        [types addObject:@(MacPlatform::kInternalPasteboardType)];
    return types;
}
- (id)pasteboardPropertyListForType:(NSPasteboardType)type
{
    if ([type isEqualToString:@(MacPlatform::kInternalPasteboardType)])
        return self.internalData;
    return [super pasteboardPropertyListForType:type];
}
- (NSPasteboardWritingOptions)writingOptionsForType:(NSPasteboardType)type pasteboard:(NSPasteboard *)pasteboard
{
    if ([type isEqualToString:@(MacPlatform::kInternalPasteboardType)])
        return 0;
    return [super writingOptionsForType:type pasteboard:pasteboard];
}
@end

@interface MDDragSource : NSObject <NSDraggingSource, NSFilePromiseProviderDelegate>
@property (nonatomic) MacPlatform::PromiseWriter writer;
@property (nonatomic) std::function<void()> finished;
@property (nonatomic, strong) NSArray<NSString *> *names;
@property (nonatomic, strong) NSOperationQueue *queue;
@property (nonatomic, weak) NSView *view;
@end

@implementation MDDragSource
- (instancetype)init
{
    if ((self = [super init])) {
        _queue = [[NSOperationQueue alloc] init];
        _queue.maxConcurrentOperationCount = 1;
        _queue.qualityOfService = NSQualityOfServiceUserInitiated;
    }
    return self;
}
- (NSDragOperation)draggingSession:(NSDraggingSession *)session
    sourceOperationMaskForDraggingContext:(NSDraggingContext)context
{
    return context == NSDraggingContextWithinApplication ? (NSDragOperationCopy | NSDragOperationMove)
                                                         : NSDragOperationCopy;
}
- (void)draggingSession:(NSDraggingSession *)session
           endedAtPoint:(NSPoint)screenPoint
              operation:(NSDragOperation)operation
{
    g_internalDrag = false;
    // The drag session swallowed the mouse-up: hand Qt a synthetic one so its
    // button state does not get stuck.
    NSView *view = self.view;
    if (view && view.window) {
        NSPoint wp = [view.window convertPointFromScreen:screenPoint];
        NSEvent *up = [NSEvent mouseEventWithType:NSEventTypeLeftMouseUp location:wp modifierFlags:0
                                        timestamp:NSProcessInfo.processInfo.systemUptime
                                     windowNumber:view.window.windowNumber context:nil
                                      eventNumber:0 clickCount:1 pressure:0];
        [view mouseUp:up];
    }
    if (self.finished)
        self.finished();
}
- (NSString *)filePromiseProvider:(NSFilePromiseProvider *)provider fileNameForType:(NSString *)fileType
{
    NSNumber *index = provider.userInfo;
    return self.names[index.unsignedIntegerValue];
}
- (void)filePromiseProvider:(NSFilePromiseProvider *)provider
          writePromiseToURL:(NSURL *)url
          completionHandler:(void (^)(NSError *))completionHandler
{
    NSNumber *index = provider.userInfo;
    const bool ok = self.writer && self.writer(int(index.integerValue), QString::fromNSString(url.path));
    completionHandler(ok ? nil : [NSError errorWithDomain:NSCocoaErrorDomain code:NSFileWriteUnknownError userInfo:nil]);
}
- (NSOperationQueue *)operationQueueForFilePromiseProvider:(NSFilePromiseProvider *)provider
{
    return self.queue;
}
@end

@interface MDPasteProvider : NSObject <NSPasteboardItemDataProvider>
@property (nonatomic) std::function<QString(int)> provide;
@end

@implementation MDPasteProvider
- (void)pasteboard:(NSPasteboard *)pasteboard item:(NSPasteboardItem *)item provideDataForType:(NSPasteboardType)type
{
    const int index = [[item stringForType:@"com.bliksund.macdroid.index"] intValue];
    const QString path = self.provide ? self.provide(index) : QString();
    if (!path.isEmpty())
        [item setString:[NSURL fileURLWithPath:ns(path)].absoluteString forType:NSPasteboardTypeFileURL];
}
- (void)pasteboardFinishedWithDataProvider:(NSPasteboard *)pasteboard
{
}
@end

namespace {
MDNotificationDelegate *g_notificationDelegate = nil;
MDDragSource *g_dragSource = nil;
MDPasteProvider *g_pasteProvider = nil;
}

// ------------------------------------------------------------------ API

namespace MacPlatform {

void setDockIconVisible(bool visible)
{
    [NSApp setActivationPolicy:visible ? NSApplicationActivationPolicyRegular : NSApplicationActivationPolicyAccessory];
    if (visible)
        [NSApp activate];
}

void activateApp()
{
    [NSApp activate];
}

void setAppearance(const QString &mode)
{
    if (mode == QLatin1String("light"))
        NSApp.appearance = [NSAppearance appearanceNamed:NSAppearanceNameAqua];
    else if (mode == QLatin1String("dark"))
        NSApp.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
    else
        NSApp.appearance = nil;
}

bool isDarkAppearance()
{
    NSAppearanceName name = [NSApp.effectiveAppearance bestMatchFromAppearancesWithNames:@[NSAppearanceNameAqua, NSAppearanceNameDarkAqua]];
    return [name isEqualToString:NSAppearanceNameDarkAqua];
}

void styleBrowserWindow(QWidget *window)
{
    NSView *view = nativeView(window);
    NSWindow *win = view.window;
    if (!win)
        return;
    // Qt's ExpandedClientAreaHint already extends content under the title bar;
    // hide the title text so the floating sidebar and toolbar own that space.
    win.titleVisibility = NSWindowTitleHidden;
    win.titlebarAppearsTransparent = YES;
    win.tabbingMode = NSWindowTabbingModeDisallowed;
}

bool startWindowDrag(QWidget *widget)
{
    NSView *view = nativeView(widget);
    NSEvent *event = NSApp.currentEvent;
    if (!view.window || !event || event.window != view.window)
        return false;
    // Leave the second click of a double-click to Qt (zoom/minimise).
    if (event.type == NSEventTypeLeftMouseDown && event.clickCount != 1)
        return false;
    if (event.type != NSEventTypeLeftMouseDown && event.type != NSEventTypeLeftMouseDragged)
        return false;
    // Let the window server move the window: smooth, multi-display aware and
    // supports macOS 26 edge tiling, unlike moving it from Qt.
    [view.window performWindowDragWithEvent:event];
    return true;
}

void titleBarDoubleClick(QWidget *widget)
{
    NSWindow *win = nativeView(widget).window;
    if (!win)
        return;
    NSString *action = [NSUserDefaults.standardUserDefaults stringForKey:@"AppleActionOnDoubleClick"];
    if ([action isEqualToString:@"Minimize"])
        [win performMiniaturize:nil];
    else if (![action isEqualToString:@"None"])
        [win performZoom:nil];
}

QImage fileTypeImage(const QString &extension, bool isFolder, int pixelSize)
{
    @autoreleasepool {
        UTType *type = isFolder ? UTTypeFolder
                                : (extension.isEmpty() ? UTTypeData : [UTType typeWithFilenameExtension:ns(extension)]);
        if (!type)
            type = UTTypeData;
        NSImage *icon = [[NSWorkspace sharedWorkspace] iconForContentType:type];
        return renderNSImage(icon, pixelSize, pixelSize);
    }
}

QString kindForExtension(const QString &extension, bool isFolder)
{
    @autoreleasepool {
        UTType *type = isFolder ? UTTypeFolder : [UTType typeWithFilenameExtension:ns(extension)];
        NSString *desc = type.localizedDescription;
        if (desc.length)
            return QString::fromNSString(desc);
        return extension.isEmpty() ? QString() : extension.toUpper();
    }
}

QImage symbolImage(const QString &name, qreal pointSize, const QColor &color, qreal dpr, int weight)
{
    @autoreleasepool {
        NSImage *img = [NSImage imageWithSystemSymbolName:ns(name) accessibilityDescription:nil];
        if (!img)
            return {};
        NSFontWeight w = weight > 0 ? NSFontWeightSemibold : (weight < 0 ? NSFontWeightLight : NSFontWeightRegular);
        NSImageSymbolConfiguration *cfg = [NSImageSymbolConfiguration configurationWithPointSize:pointSize weight:w];
        img = [img imageWithSymbolConfiguration:cfg];
        const NSSize sz = img.size;
        QImage out = renderNSImage(img, qRound(sz.width * dpr), qRound(sz.height * dpr), &color);
        out.setDevicePixelRatio(dpr);
        return out;
    }
}

QImage quickLookThumbnail(const QString &localPath, int pixelSize)
{
    @autoreleasepool {
        NSURL *url = [NSURL fileURLWithPath:ns(localPath)];
        QLThumbnailGenerationRequest *req =
            [[QLThumbnailGenerationRequest alloc] initWithFileAtURL:url
                                                               size:CGSizeMake(pixelSize, pixelSize)
                                                              scale:1.0
                                                representationTypes:QLThumbnailGenerationRequestRepresentationTypeThumbnail];
        dispatch_semaphore_t sem = dispatch_semaphore_create(0);
        __block CGImageRef result = nullptr;
        [[QLThumbnailGenerator sharedGenerator] generateBestRepresentationForRequest:req
            completionHandler:^(QLThumbnailRepresentation *rep, NSError *error) {
                if (rep && rep.CGImage)
                    result = CGImageRetain(rep.CGImage);
                dispatch_semaphore_signal(sem);
            }];
        if (dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, 20 * NSEC_PER_SEC)) != 0)
            return {};
        QImage img = imageFromCGImage(result);
        if (result)
            CGImageRelease(result);
        return img;
    }
}

void initNotifications(std::function<void()> onActivate)
{
    if (!NSBundle.mainBundle.bundleIdentifier)
        return; // not running from an .app bundle
    g_notificationDelegate = [[MDNotificationDelegate alloc] init];
    g_notificationDelegate.onActivate = std::move(onActivate);
    UNUserNotificationCenter *center = UNUserNotificationCenter.currentNotificationCenter;
    center.delegate = g_notificationDelegate;
    [center requestAuthorizationWithOptions:(UNAuthorizationOptionAlert | UNAuthorizationOptionSound)
                          completionHandler:^(BOOL, NSError *) {}];
}

void postNotification(const QString &title, const QString &body)
{
    if (!g_notificationDelegate)
        return;
    UNMutableNotificationContent *content = [[UNMutableNotificationContent alloc] init];
    content.title = ns(title);
    content.body = ns(body);
    UNNotificationRequest *req = [UNNotificationRequest requestWithIdentifier:NSUUID.UUID.UUIDString content:content trigger:nil];
    [UNUserNotificationCenter.currentNotificationCenter addNotificationRequest:req withCompletionHandler:nil];
}

bool launchAtLoginEnabled()
{
    return SMAppService.mainAppService.status == SMAppServiceStatusEnabled;
}

bool setLaunchAtLogin(bool enabled, QString *error)
{
    NSError *err = nil;
    const BOOL ok = enabled ? [SMAppService.mainAppService registerAndReturnError:&err]
                            : [SMAppService.mainAppService unregisterAndReturnError:&err];
    if (!ok && error && err)
        *error = QString::fromNSString(err.localizedDescription);
    return ok;
}

void registerDropTypes(QWidget *topLevel)
{
    NSView *view = nativeView(topLevel);
    if (!view)
        return;
    NSMutableSet *types = [NSMutableSet setWithArray:view.registeredDraggedTypes ?: @[]];
    [types addObject:@(kInternalPasteboardType)];
    [types addObject:NSPasteboardTypeFileURL];
    [types addObjectsFromArray:NSFilePromiseReceiver.readableDraggedTypes];
    [view registerForDraggedTypes:types.allObjects];
}

bool startFilePromiseDrag(QWidget *source, const QVector<PromisedItem> &items, const QByteArray &internalData,
                          const QImage &preview, PromiseWriter writer, std::function<void()> finished)
{
    NSView *view = nativeView(source);
    if (!view || !view.window || items.isEmpty())
        return false;
    // Qt may deliver the mouse move after AppKit finished with the NSEvent, so
    // currentEvent is not reliable: synthesise a drag event at the pointer.
    NSEvent *event = NSApp.currentEvent;
    if (!event || (event.type != NSEventTypeLeftMouseDragged && event.type != NSEventTypeLeftMouseDown)
        || event.window != view.window) {
        const NSPoint screen = NSEvent.mouseLocation;
        const NSPoint inWindow = [view.window convertPointFromScreen:screen];
        event = [NSEvent mouseEventWithType:NSEventTypeLeftMouseDragged location:inWindow
                              modifierFlags:NSEvent.modifierFlags timestamp:NSProcessInfo.processInfo.systemUptime
                               windowNumber:view.window.windowNumber context:nil eventNumber:0 clickCount:1 pressure:1.0];
    }

    registerDropTypes(source);
    if (!g_dragSource)
        g_dragSource = [[MDDragSource alloc] init];
    NSMutableArray<NSString *> *names = [NSMutableArray array];
    for (const PromisedItem &it : items)
        [names addObject:ns(it.name)];
    g_dragSource.names = names;
    g_dragSource.writer = std::move(writer);
    g_dragSource.finished = std::move(finished);
    g_dragSource.view = view;

    NSData *internal = [NSData dataWithBytes:internalData.constData() length:NSUInteger(internalData.size())];
    NSImage *image = nil;
    NSSize imageSize = NSMakeSize(64, 64);
    if (!preview.isNull()) {
        CGImageRef cg = cgImageFromQImage(preview);
        const qreal dpr = preview.devicePixelRatio() > 0 ? preview.devicePixelRatio() : 1.0;
        imageSize = NSMakeSize(preview.width() / dpr, preview.height() / dpr);
        image = [[NSImage alloc] initWithCGImage:cg size:imageSize];
        CGImageRelease(cg);
    }
    const NSPoint loc = [view convertPoint:event.locationInWindow fromView:nil];

    NSMutableArray<NSDraggingItem *> *dragItems = [NSMutableArray array];
    for (int i = 0; i < items.size(); ++i) {
        const PromisedItem &it = items.at(i);
        NSString *uti = UTTypeFolder.identifier;
        if (!it.isFolder) {
            const QString ext = it.name.contains(u'.') ? it.name.section(u'.', -1) : QString();
            UTType *t = ext.isEmpty() ? nil : [UTType typeWithFilenameExtension:ns(ext)];
            uti = (t ?: UTTypeData).identifier;
        }
        MDFilePromiseProvider *provider = [[MDFilePromiseProvider alloc] initWithFileType:uti delegate:g_dragSource];
        provider.userInfo = @(i);
        if (i == 0)
            provider.internalData = internal;
        NSDraggingItem *dragItem = [[NSDraggingItem alloc] initWithPasteboardWriter:provider];
        const NSRect frame = NSMakeRect(loc.x - imageSize.width / 2 + i * 3, loc.y - imageSize.height / 2 + i * 3,
                                        imageSize.width, imageSize.height);
        [dragItem setDraggingFrame:frame contents:(i == 0 ? image : nil)];
        [dragItems addObject:dragItem];
    }
    g_internalDragData = internalData;
    g_internalDrag = true;
    NSDraggingSession *session = [view beginDraggingSessionWithItems:dragItems event:event source:g_dragSource];
    session.animatesToStartingPositionsOnCancelOrFail = YES;
    session.draggingFormation = NSDraggingFormationPile;
    return true;
}

bool internalDragActive()
{
    return g_internalDrag.load();
}

QByteArray internalDragData()
{
    return g_internalDrag.load() ? g_internalDragData : QByteArray();
}

void writeItemsToPasteboard(const QVector<PromisedItem> &items, const QByteArray &internalData,
                            std::function<QString(int)> provideFile)
{
    if (!g_pasteProvider)
        g_pasteProvider = [[MDPasteProvider alloc] init];
    g_pasteProvider.provide = std::move(provideFile);

    NSPasteboard *pb = NSPasteboard.generalPasteboard;
    [pb clearContents];
    NSMutableArray<NSPasteboardItem *> *pbItems = [NSMutableArray array];
    for (int i = 0; i < items.size(); ++i) {
        NSPasteboardItem *item = [[NSPasteboardItem alloc] init];
        [item setString:[NSString stringWithFormat:@"%d", i] forType:@"com.bliksund.macdroid.index"];
        if (i == 0)
            [item setData:[NSData dataWithBytes:internalData.constData() length:NSUInteger(internalData.size())]
                  forType:@(kInternalPasteboardType)];
        [item setDataProvider:g_pasteProvider forTypes:@[NSPasteboardTypeFileURL]];
        [pbItems addObject:item];
    }
    [pb writeObjects:pbItems];
}

void clearPasteboard()
{
    [NSPasteboard.generalPasteboard clearContents];
}

bool pasteboardHasInternalItems()
{
    return [NSPasteboard.generalPasteboard availableTypeFromArray:@[@(kInternalPasteboardType)]] != nil;
}

QByteArray pasteboardInternalItems()
{
    NSData *data = [NSPasteboard.generalPasteboard dataForType:@(kInternalPasteboardType)];
    if (!data)
        return {};
    return QByteArray(static_cast<const char *>(data.bytes), qsizetype(data.length));
}

QStringList pasteboardFilePaths()
{
    if (pasteboardHasInternalItems())
        return {};
    QStringList out;
    NSArray<NSURL *> *urls = [NSPasteboard.generalPasteboard readObjectsForClasses:@[NSURL.class]
                                                                           options:@{NSPasteboardURLReadingFileURLsOnlyKey: @YES}];
    for (NSURL *url in urls)
        out << QString::fromNSString(url.path);
    return out;
}

} // namespace MacPlatform
