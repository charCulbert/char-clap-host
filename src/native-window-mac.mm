#include "native-window.h"

#include <clap/clap.h>

#import <Cocoa/Cocoa.h>

namespace nch {
namespace {

// A CLI process has no activation policy of its own, so the first window has
// to ask for one or it never appears on screen.
void ensureApplication() {
	static bool prepared = false;
	if (prepared)
		return;
	prepared = true;
	[NSApplication sharedApplication];
	[NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
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

private:
	NSWindow *window_ = nil;
	NSView *view_ = nil;
	NchWindowDelegate *delegate_ = nil;
};

} // namespace

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

void pumpApplicationEvents() {
	@autoreleasepool {
		if (NSApp == nil)
			return;
		// Drain whatever is queued without blocking; the main loop decides how
		// often to come back.
		while (true) {
			NSEvent *event = [NSApp nextEventMatchingMask:NSEventMaskAny
			                                    untilDate:[NSDate distantPast]
			                                       inMode:NSDefaultRunLoopMode
			                                      dequeue:YES];
			if (event == nil)
				break;
			[NSApp sendEvent:event];
		}
	}
}

} // namespace nch
