#include "native-window.h"

#include <clap/clap.h>

#include <cstdio>
#include <functional>
#include <string>

#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>
#import <WebKit/WebKit.h>

namespace nch {
// Defined below; the application delegate and the content view need them
// before either exists.
void requestQuit();
void openSettings();
void openPanel();
void loadPlugin(const std::string &path);
// The dragged .clap or .wav, or an empty string when the drag carries neither.
// Shared by the content view and the drop target, which both take a drop.
NSString *droppedPath(id<NSDraggingInfo> sender);
// A .clap loads; a .wav plays into it.
void openDropped(NSString *path);
} // namespace nch

// Takes the drop on behalf of a window whose content is a webview.
//
// A WKWebView swallows a drag before AppKit's search reaches the view beneath
// it, and unregistering its dragged types does not give it back -- the web
// content process handles the drag out of band. Registering the window itself
// does not help either, for the same reason. What does work is a view in front
// of the webview: AppKit finds it first, and returning nil from -hitTest: keeps
// every other event flowing through to the page underneath.
@interface NchDropOverlay : NSView
@end

@implementation NchDropOverlay {
	BOOL highlighted_;
}

- (instancetype)initWithFrame:(NSRect)frame {
	self = [super initWithFrame:frame];
	if (self != nil)
		[self registerForDraggedTypes:@[ NSPasteboardTypeFileURL ]];
	return self;
}

// Invisible to the mouse: clicks, scrolls and hovers belong to the page.
- (NSView *)hitTest:(NSPoint)point {
	(void)point;
	return nil;
}

- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)sender {
	return [self dragOperation:sender];
}

// Without this AppKit asks again on every mouse move and takes the answer, so
// the copy badge would go as soon as the pointer moved.
- (NSDragOperation)draggingUpdated:(id<NSDraggingInfo>)sender {
	return [self dragOperation:sender];
}

- (void)draggingExited:(id<NSDraggingInfo>)sender {
	(void)sender;
	[self setHighlighted:NO];
}

- (BOOL)performDragOperation:(id<NSDraggingInfo>)sender {
	[self setHighlighted:NO];
	NSString *path = nch::droppedPath(sender);
	if (path.length == 0)
		return NO;
	nch::openDropped(path);
	return YES;
}

- (NSDragOperation)dragOperation:(id<NSDraggingInfo>)sender {
	const BOOL wanted = nch::droppedPath(sender).length != 0;
	[self setHighlighted:wanted];
	return wanted ? NSDragOperationCopy : NSDragOperationNone;
}

- (void)setHighlighted:(BOOL)highlighted {
	if (highlighted_ == highlighted)
		return;
	highlighted_ = highlighted;
	[self setNeedsDisplay:YES];
}

// A border while a .clap or .wav is over the window, because the page underneath gives
// no sign on its own that the drop will land.
- (void)drawRect:(NSRect)dirty {
	(void)dirty;
	if (!highlighted_)
		return;
	[[NSColor selectedContentBackgroundColor] setStroke];
	NSBezierPath *border = [NSBezierPath bezierPathWithRect:NSInsetRect([self bounds], 2, 2)];
	[border setLineWidth:4];
	[border stroke];
}
@end

// A window that accepts a .clap dropped onto it. Dropping is the quickest way
// to try a plug-in, and it costs one view subclass.
@interface NchContentView : NSView
@end

@implementation NchContentView

- (instancetype)initWithFrame:(NSRect)frame {
	self = [super initWithFrame:frame];
	if (self != nil)
		[self registerForDraggedTypes:@[ NSPasteboardTypeFileURL ]];
	return self;
}

- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)sender {
	return nch::droppedPath(sender).length != 0 ? NSDragOperationCopy : NSDragOperationNone;
}

// Without this AppKit asks again on every mouse move and takes the answer, so
// the copy badge would go as soon as the pointer moved.
- (NSDragOperation)draggingUpdated:(id<NSDraggingInfo>)sender {
	return nch::droppedPath(sender).length != 0 ? NSDragOperationCopy : NSDragOperationNone;
}

- (BOOL)performDragOperation:(id<NSDraggingInfo>)sender {
	NSString *path = nch::droppedPath(sender);
	if (path.length == 0)
		return NO;
	nch::openDropped(path);
	return YES;
}
@end

// Cmd-Q would otherwise call -terminate: and kill the process where it stands,
// leaving the plug-in undestroyed. Cancelling the termination and asking the
// host to quit takes the ordinary shutdown path instead.
@interface NchAppDelegate : NSObject <NSApplicationDelegate>
@end

@implementation NchAppDelegate
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication *)sender {
	(void)sender;
	nch::requestQuit();
	return NSTerminateCancel;
}

- (void)showAudioMidiSettings:(id)sender {
	(void)sender;
	nch::openSettings();
}

- (void)showPluginPanel:(id)sender {
	(void)sender;
	nch::openPanel();
}

- (void)loadPlugin:(id)sender {
	(void)sender;
	NSOpenPanel *panel = [NSOpenPanel openPanel];
	[panel setAllowedFileTypes:@[ @"clap" ]];
	// A .clap is a bundle on macOS, which the panel treats as a directory
	// unless it is told to select it whole.
	[panel setCanChooseDirectories:YES];
	[panel setCanChooseFiles:YES];
	[panel setTreatsFilePackagesAsDirectories:NO];
	[panel setAllowsMultipleSelection:NO];
	[panel setMessage:@"Choose a CLAP plug-in"];
	if ([panel runModal] != NSModalResponseOK)
		return;
	NSURL *url = [[panel URLs] firstObject];
	if (url != nil)
		nch::loadPlugin(std::string([[url path] UTF8String]));
}
@end

namespace nch {
namespace {

std::function<void()> &quitHandler() {
	static std::function<void()> handler;
	return handler;
}

std::function<void()> &settingsHandler() {
	static std::function<void()> handler;
	return handler;
}

std::function<void()> &panelHandler() {
	static std::function<void()> handler;
	return handler;
}

std::function<void(const std::string &)> &loadPluginHandler() {
	static std::function<void(const std::string &)> handler;
	return handler;
}

std::function<void(const std::string &)> &playAudioFileHandler() {
	static std::function<void(const std::string &)> handler;
	return handler;
}

NchAppDelegate *applicationDelegate() {
	static NchAppDelegate *delegate = [[NchAppDelegate alloc] init];
	return delegate;
}

// An application built by hand gets no menu bar, and without one none of the
// standard shortcuts exist: no Cmd-Q, no Cmd-W, no Cmd-H. Every one of those
// is muscle memory, so the host installs the minimum that makes them work.
void installMainMenu() {
	NSMenu *menubar = [[NSMenu alloc] init];
	NSMenuItem *appItem = [[NSMenuItem alloc] init];
	[menubar addItem:appItem];

	NSString *name = @"clap-host";
	NSMenu *appMenu = [[NSMenu alloc] init];
	[appMenu addItemWithTitle:[@"About " stringByAppendingString:name]
	                   action:@selector(orderFrontStandardAboutPanel:)
	            keyEquivalent:@""];
	[appMenu addItem:[NSMenuItem separatorItem]];
	[appMenu addItemWithTitle:[@"Hide " stringByAppendingString:name]
	                   action:@selector(hide:)
	            keyEquivalent:@"h"];
	NSMenuItem *hideOthers = [appMenu addItemWithTitle:@"Hide Others"
	                                            action:@selector(hideOtherApplications:)
	                                     keyEquivalent:@"h"];
	[hideOthers setKeyEquivalentModifierMask:NSEventModifierFlagCommand | NSEventModifierFlagOption];
	[appMenu addItemWithTitle:@"Show All" action:@selector(unhideAllApplications:) keyEquivalent:@""];
	[appMenu addItem:[NSMenuItem separatorItem]];
	[appMenu addItemWithTitle:[@"Quit " stringByAppendingString:name]
	                   action:@selector(terminate:)
	            keyEquivalent:@"q"];
	[appItem setSubmenu:appMenu];

	NSMenuItem *fileItem = [[NSMenuItem alloc] init];
	[menubar addItem:fileItem];
	NSMenu *fileMenu = [[NSMenu alloc] initWithTitle:@"File"];
	NSMenuItem *loadItem = [fileMenu addItemWithTitle:@"Load Plug-in…"
	                                           action:@selector(loadPlugin:)
	                                    keyEquivalent:@"o"];
	[loadItem setTarget:applicationDelegate()];
	[fileItem setSubmenu:fileMenu];

	NSMenuItem *settingsItem = [[NSMenuItem alloc] init];
	[menubar addItem:settingsItem];
	NSMenu *settingsMenu = [[NSMenu alloc] initWithTitle:@"Settings"];
	NSMenuItem *audioMidi = [settingsMenu addItemWithTitle:@"Audio/MIDI Settings…"
	                                                action:@selector(showAudioMidiSettings:)
	                                         keyEquivalent:@","];
	// Targeted at the delegate rather than the responder chain, so the item
	// stays enabled whichever window happens to be focused.
	[audioMidi setTarget:applicationDelegate()];
	[settingsItem setSubmenu:settingsMenu];

	NSMenuItem *windowItem = [[NSMenuItem alloc] init];
	[menubar addItem:windowItem];
	NSMenu *windowMenu = [[NSMenu alloc] initWithTitle:@"Window"];
	NSMenuItem *panelItem = [windowMenu addItemWithTitle:@"Parameters & Presets"
	                                              action:@selector(showPluginPanel:)
	                                       keyEquivalent:@"p"];
	[panelItem setTarget:applicationDelegate()];
	[windowMenu addItem:[NSMenuItem separatorItem]];
	[windowMenu addItemWithTitle:@"Close" action:@selector(performClose:) keyEquivalent:@"w"];
	[windowMenu addItemWithTitle:@"Minimise" action:@selector(performMiniaturize:) keyEquivalent:@"m"];
	[windowMenu addItemWithTitle:@"Zoom" action:@selector(performZoom:) keyEquivalent:@""];
	[windowItem setSubmenu:windowMenu];
	[NSApp setWindowsMenu:windowMenu];

	[NSApp setMainMenu:menubar];
}

// A CLI process has no activation policy of its own, so the first window has
// to ask for one or it never appears on screen.
void ensureApplication() {
	static bool prepared = false;
	if (prepared)
		return;
	prepared = true;
	[NSApplication sharedApplication];
	[NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
	[NSApp setDelegate:applicationDelegate()];
	installMainMenu();
	[NSApp activateIgnoringOtherApps:YES];
	[NSApp finishLaunching];
}

} // namespace

} // namespace nch

// Tracks the close button, and carries whoever is entitled to a say in the
// window's size, without needing a delegate object per window.
@interface NchWindowDelegate : NSObject <NSWindowDelegate>
@property(nonatomic) BOOL closed;
- (void)takeResizer:(nch::NativeWindow::Resizer)resizer;
@end

@implementation NchWindowDelegate {
	nch::NativeWindow::Resizer resizer_;
}

- (BOOL)windowShouldClose:(NSWindow *)sender {
	(void)sender;
	self.closed = YES;
	return NO; // the host closes the plug-in's gui first, then the window
}

- (void)takeResizer:(nch::NativeWindow::Resizer)resizer {
	resizer_ = std::move(resizer);
}

// Answers the drag while it happens, so the window snaps to a size the plug-in
// will accept rather than being corrected once the user lets go.
- (NSSize)windowWillResize:(NSWindow *)sender toSize:(NSSize)size {
	if (!resizer_.adjust)
		return size;
	const NSRect content = [sender contentRectForFrameRect:NSMakeRect(0, 0, size.width, size.height)];
	auto width = static_cast<uint32_t>(content.size.width);
	auto height = static_cast<uint32_t>(content.size.height);
	resizer_.adjust(width, height);
	return [sender frameRectForContentRect:NSMakeRect(0, 0, width, height)].size;
}

- (void)windowDidResize:(NSNotification *)notification {
	if (!resizer_.commit)
		return;
	const NSSize size = [[[notification object] contentView] frame].size;
	resizer_.commit(static_cast<uint32_t>(size.width), static_cast<uint32_t>(size.height));
}
@end

// Keeps a window above the others while it is on. A plug-in's interface is
// usually being watched while something else has the keyboard, which is exactly
// when a window that hides itself is in the way.
@interface NchPinController : NSObject
@property(nonatomic, assign) NSWindow *window;
@end

@implementation NchPinController
- (void)togglePin:(NSButton *)sender {
	const BOOL pinned = [sender state] == NSControlStateValueOn;
	[[self window] setLevel:pinned ? NSFloatingWindowLevel : NSNormalWindowLevel];
	[sender setToolTip:pinned ? @"Stop keeping this window in front" : @"Keep this window in front"];
}
@end

namespace nch {

NSString *droppedPath(id<NSDraggingInfo> sender) {
	NSArray *urls = [[sender draggingPasteboard] readObjectsForClasses:@[ [NSURL class] ]
	                                                           options:@{NSPasteboardURLReadingFileURLsOnlyKey : @YES}];
	for (NSURL *url in urls)
		for (NSString *extension in @[ @"clap", @"wav", @"wave" ])
			if ([[url pathExtension] caseInsensitiveCompare:extension] == NSOrderedSame)
				return [url path];
	return @"";
}

void openDropped(NSString *path) {
	const std::string text([path UTF8String]);
	if ([[path pathExtension] caseInsensitiveCompare:@"clap"] == NSOrderedSame)
		loadPlugin(text);
	else if (playAudioFileHandler())
		playAudioFileHandler()(text);
}

namespace {

// The pin, drawn as a title-bar accessory so it sits beside the traffic lights
// rather than taking a row of the window's content.
NSTitlebarAccessoryViewController *makePinAccessory(NSWindow *window, NchPinController *controller) {
	[controller setWindow:window];
	NSButton *button = [NSButton buttonWithTitle:@"" target:controller action:@selector(togglePin:)];
	[button setButtonType:NSButtonTypePushOnPushOff];
	[button setBezelStyle:NSBezelStyleTexturedRounded];
	[button setBordered:NO];
	NSImage *pin = nil;
	if ([NSImage respondsToSelector:@selector(imageWithSystemSymbolName:accessibilityDescription:)])
		pin = [NSImage imageWithSystemSymbolName:@"pin" accessibilityDescription:@"Keep in front"];
	if (pin != nil) {
		[button setImage:pin];
		[button setImagePosition:NSImageOnly];
	} else {
		// Before SF Symbols; the character says the same thing.
		[button setTitle:@"\U0001F4CC"];
	}
	[button setToolTip:@"Keep this window in front"];
	[button setFrame:NSMakeRect(0, 0, 32, 22)];

	NSView *host = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 36, 22)];
	[host addSubview:button];
	NSTitlebarAccessoryViewController *accessory = [[NSTitlebarAccessoryViewController alloc] init];
	[accessory setView:host];
	[accessory setLayoutAttribute:NSLayoutAttributeRight];
	return accessory;
}

void describeView(NSView *view, int depth, std::string &out) {
	if (view == nil)
		return;
	const NSRect frame = [view frame];
	char line[256];
	std::snprintf(line, sizeof(line), "%*s%s  frame %.0f,%.0f %.0fx%.0f%s%s\n", depth * 2, "",
	              [NSStringFromClass([view class]) UTF8String], frame.origin.x, frame.origin.y, frame.size.width,
	              frame.size.height, [view isHidden] ? "  hidden" : "",
	              [view layer] != nil ? "  layer" : "");
	out += line;
	// A webview that is parented and sized can still be blank because its page
	// never loaded, and that difference is invisible from the view tree alone.
	if ([view respondsToSelector:@selector(estimatedProgress)]) {
		NSURL *url = [view valueForKey:@"URL"];
		std::snprintf(line, sizeof(line), "%*s  url %s  progress %s  loading %s  title %s\n", depth * 2, "",
		              url != nil ? [[url absoluteString] UTF8String] : "none",
		              [[[view valueForKey:@"estimatedProgress"] stringValue] UTF8String],
		              [[view valueForKey:@"loading"] boolValue] ? "yes" : "no",
		              [[view valueForKey:@"title"] UTF8String]);
		out += line;
	}
	for (NSView *child in [view subviews])
		describeView(child, depth + 1, out);
}

class CocoaWindow : public NativeWindow {
public:
	CocoaWindow(uint32_t width, uint32_t height, const std::string &title) {
		ensureApplication();
		const NSRect frame = NSMakeRect(0, 0, width, height);
		window_ = [[NSWindow alloc]
		    initWithContentRect:frame
		              styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskResizable
		                backing:NSBackingStoreBuffered
		                  defer:NO];
		delegate_ = [[NchWindowDelegate alloc] init];
		[window_ setDelegate:delegate_];
		[window_ setTitle:[NSString stringWithUTF8String:title.c_str()]];
		[window_ setReleasedWhenClosed:NO];
		view_ = [[NchContentView alloc] initWithFrame:frame];
		[view_ setWantsLayer:YES];
		[window_ setContentView:view_];
		pin_ = [[NchPinController alloc] init];
		[window_ addTitlebarAccessoryViewController:makePinAccessory(window_, pin_)];
		[window_ center];
	}

	~CocoaWindow() override {
		[window_ setDelegate:nil];
		[window_ orderOut:nil];
		[window_ close];
	}

	void *handle() override { return (__bridge void *)view_; }

	void attachChild(void *view) override {
		NSView *child = (__bridge NSView *)view;
		if (child == nil)
			return;
		[child setFrame:[view_ bounds]];
		[child setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
		[view_ addSubview:child];
	}

	void setResizer(Resizer resizer) override { [delegate_ takeResizer:std::move(resizer)]; }

	void setUserResizable(bool resizable) override {
		NSWindowStyleMask mask = [window_ styleMask];
		if (resizable)
			mask |= NSWindowStyleMaskResizable;
		else
			mask &= ~NSWindowStyleMaskResizable;
		[window_ setStyleMask:mask];
	}

	void acceptDropsAboveChild() override {
		if (overlay_ != nil)
			return;
		overlay_ = [[NchDropOverlay alloc] initWithFrame:[view_ bounds]];
		[overlay_ setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
		[view_ addSubview:overlay_ positioned:NSWindowAbove relativeTo:nil];
	}


	void setTitle(const std::string &title) override {
		[window_ setTitle:[NSString stringWithUTF8String:title.c_str()]];
	}

	void setSize(uint32_t width, uint32_t height) override {
		NSRect frame = [window_ frame];
		const NSRect content = [window_ contentRectForFrameRect:frame];
		// A window taller than the screen is worse than one that scrolls, and a
		// page that sizes itself has no idea how big the screen is.
		NSScreen *screen = [window_ screen] != nil ? [window_ screen] : [NSScreen mainScreen];
		if (screen != nil) {
			const NSRect visible = [screen visibleFrame];
			const NSRect room = [window_ contentRectForFrameRect:visible];
			height = std::min<uint32_t>(height, static_cast<uint32_t>(room.size.height));
			width = std::min<uint32_t>(width, static_cast<uint32_t>(room.size.width));
		}
		// Keep the top-left corner still while the content box changes.
		frame.origin.y += content.size.height - height;
		NSRect grown =
		    [window_ frameRectForContentRect:NSMakeRect(frame.origin.x, frame.origin.y, width, height)];
		// Growing downwards off the bottom of the screen hides what it grew to
		// show, so slide it back up.
		if (screen != nil) {
			const NSRect visible = [screen visibleFrame];
			if (NSMinY(grown) < NSMinY(visible))
				grown.origin.y = NSMinY(visible);
			if (NSMaxY(grown) > NSMaxY(visible))
				grown.origin.y = NSMaxY(visible) - grown.size.height;
		}
		[window_ setFrame:grown display:YES];
		[view_ setFrame:NSMakeRect(0, 0, width, height)];
	}

	void show() override {
		[window_ makeKeyAndOrderFront:nil];
		[NSApp activateIgnoringOtherApps:YES];
	}

	void hide() override { [window_ orderOut:nil]; }

	bool wantsClose() const override { return delegate_.closed == YES; }

	std::string describeContents() const override {
		std::string out;
		describeView(view_, 0, out);
		char line[512];
		const NSRect frame = [window_ frame];
		std::snprintf(line, sizeof(line),
		              "window frame %.0f,%.0f %.0fx%.0f\nvisible %s  key %s  onActiveSpace %s  occluded %s\n"
		              "screen %s\napp active %s  policy %ld\n",
		              frame.origin.x, frame.origin.y, frame.size.width, frame.size.height,
		              [window_ isVisible] ? "yes" : "no", [window_ isKeyWindow] ? "yes" : "no",
		              [window_ isOnActiveSpace] ? "yes" : "no",
		              ([window_ occlusionState] & NSWindowOcclusionStateVisible) == 0 ? "yes" : "no",
		              [window_ screen] != nil ? [[[window_ screen] localizedName] UTF8String] : "none",
		              [NSApp isActive] ? "yes" : "no", (long)[NSApp activationPolicy]);
		out += line;
		return out;
	}

	bool writeSnapshot(const std::string &path, std::string &error) override {
		@autoreleasepool {
			// CGWindowListCreateImage needs screen-recording rights; asking the
			// window for its own contents does not.
			const CGSize size = [window_ frame].size;
			const NSRect content = [window_ contentRectForFrameRect:[window_ frame]];
			(void)size;
			NSBitmapImageRep *bitmap = [view_ bitmapImageRepForCachingDisplayInRect:[view_ bounds]];
			if (bitmap == nil) {
				error = "the window has no drawable contents";
				return false;
			}
			[view_ cacheDisplayInRect:[view_ bounds] toBitmapImageRep:bitmap];
			NSData *png = [bitmap representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
			if (png == nil) {
				error = "could not encode the window contents";
				return false;
			}
			(void)content;
			NSString *file = [NSString stringWithUTF8String:path.c_str()];
			if (![png writeToFile:file atomically:YES]) {
				error = "could not write " + path;
				return false;
			}
			return true;
		}
	}

private:
	NSWindow *window_ = nil;
	NSView *view_ = nil;
	NchWindowDelegate *delegate_ = nil;
	NchPinController *pin_ = nil;
	NchDropOverlay *overlay_ = nil;
};

} // namespace

void setQuitHandler(std::function<void()> handler) {
	quitHandler() = std::move(handler);
}

void requestQuit() {
	if (quitHandler())
		quitHandler()();
}

void setSettingsHandler(std::function<void()> handler) {
	settingsHandler() = std::move(handler);
}

void setPanelHandler(std::function<void()> handler) {
	panelHandler() = std::move(handler);
}

void setLoadPluginHandler(std::function<void(const std::string &)> handler) {
	loadPluginHandler() = std::move(handler);
}

void setPlayAudioFileHandler(std::function<void(const std::string &)> handler) {
	playAudioFileHandler() = std::move(handler);
}

std::string chooseFile(const std::string &message, const std::vector<std::string> &extensions) {
	prepareApplication();
	NSOpenPanel *panel = [NSOpenPanel openPanel];
	NSMutableArray *types = [NSMutableArray array];
	for (const auto &extension : extensions)
		[types addObject:[NSString stringWithUTF8String:extension.c_str()]];
	[panel setAllowedFileTypes:types];
	[panel setCanChooseFiles:YES];
	[panel setCanChooseDirectories:NO];
	[panel setAllowsMultipleSelection:NO];
	[panel setMessage:[NSString stringWithUTF8String:message.c_str()]];
	if ([panel runModal] != NSModalResponseOK)
		return {};
	NSURL *url = [[panel URLs] firstObject];
	return url != nil ? std::string([[url path] UTF8String]) : std::string();
}

void loadPlugin(const std::string &path) {
	if (loadPluginHandler())
		loadPluginHandler()(path);
}

void openSettings() {
	if (settingsHandler())
		settingsHandler()();
}

void openPanel() {
	if (panelHandler())
		panelHandler()();
}

void prepareApplication() {
	@autoreleasepool {
		ensureApplication();
	}
}

std::unique_ptr<NativeWindow> createNativeWindow(uint32_t width, uint32_t height, const std::string &title,
                                                 std::string &error) {
	@autoreleasepool {
		try {
			return std::make_unique<CocoaWindow>(width, height, title);
		} catch (const std::exception &failure) {
			error = failure.what();
			return nullptr;
		}
	}
}

const char *nativeWindowApi() {
	return CLAP_WINDOW_API_COCOA;
}

void runApplicationLoop(const std::function<bool()> &tick, int intervalMs) {
	@autoreleasepool {
		// A run that never opens a window should not become an application:
		// NSApplicationActivationPolicyRegular takes a Dock icon and an
		// activation handshake with the window server, which a headless render
		// has no use for and which does not always complete when several such
		// processes start and exit in quick succession. So the loop stays a
		// plain run loop until something actually asks for a window.
		while (NSApp == nil) {
			if (!tick())
				return;
			CFRunLoopRunInMode(kCFRunLoopDefaultMode, intervalMs / 1000.0, false);
		}

		__block bool running = true;
		// Common modes so the host keeps ticking through window resizes and
		// menu tracking, which otherwise starve it.
		NSTimer *timer = [NSTimer timerWithTimeInterval:intervalMs / 1000.0
		                                        repeats:YES
		                                          block:^(NSTimer *firing) {
			                                          if (!running)
				                                          return;
			                                          if (tick())
				                                          return;
			                                          running = false;
			                                          [firing invalidate];
			                                          [NSApp stop:nil];
			                                          // -stop: only takes effect once the loop
			                                          // handles another event, so give it one.
			                                          [NSApp postEvent:[NSEvent otherEventWithType:NSEventTypeApplicationDefined
			                                                                              location:NSZeroPoint
			                                                                         modifierFlags:0
			                                                                             timestamp:0
			                                                                          windowNumber:0
			                                                                               context:nil
			                                                                               subtype:0
			                                                                                 data1:0
			                                                                                 data2:0]
			                                                   atStart:YES];
		                                          }];
		[[NSRunLoop mainRunLoop] addTimer:timer forMode:NSRunLoopCommonModes];
		[NSApp run];
		[timer invalidate];
	}
}

} // namespace nch
