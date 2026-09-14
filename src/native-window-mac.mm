#include "native-window.h"

#include <clap/clap.h>

#include <cstdio>
#include <functional>
#include <string>

#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>

namespace nch {
// Defined below; the application delegate needs it before it exists.
void requestQuit();
} // namespace nch

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
@end

namespace nch {
namespace {

std::function<void()> &quitHandler() {
	static std::function<void()> handler;
	return handler;
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

	NSMenuItem *windowItem = [[NSMenuItem alloc] init];
	[menubar addItem:windowItem];
	NSMenu *windowMenu = [[NSMenu alloc] initWithTitle:@"Window"];
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
	static NchAppDelegate *delegate = [[NchAppDelegate alloc] init];
	[NSApp setDelegate:delegate];
	installMainMenu();
	[NSApp activateIgnoringOtherApps:YES];
	[NSApp finishLaunching];
}

} // namespace

} // namespace nch

// Tracks the close button without needing a delegate object per window.
@interface NchWindowDelegate : NSObject <NSWindowDelegate>
@property(nonatomic) BOOL closed;
@end

@implementation NchWindowDelegate
- (BOOL)windowShouldClose:(NSWindow *)sender {
	(void)sender;
	self.closed = YES;
	return NO; // the host closes the plug-in's gui first, then the window
}
@end

namespace nch {
namespace {

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
		view_ = [[NSView alloc] initWithFrame:frame];
		[view_ setWantsLayer:YES];
		[window_ setContentView:view_];
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

	void setTitle(const std::string &title) override {
		[window_ setTitle:[NSString stringWithUTF8String:title.c_str()]];
	}

	void setSize(uint32_t width, uint32_t height) override {
		NSRect frame = [window_ frame];
		const NSRect content = [window_ contentRectForFrameRect:frame];
		// Keep the top-left corner still while the content box changes.
		frame.origin.y += content.size.height - height;
		[window_ setFrame:[window_ frameRectForContentRect:NSMakeRect(frame.origin.x, frame.origin.y, width, height)]
		          display:YES];
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
};

} // namespace

void setQuitHandler(std::function<void()> handler) {
	quitHandler() = std::move(handler);
}

void requestQuit() {
	if (quitHandler())
		quitHandler()();
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
		ensureApplication();
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
