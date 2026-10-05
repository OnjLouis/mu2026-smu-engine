// license:BSD-3-Clause
//
// The macOS half of the PC editor windows (src/ui/pc_window.cpp is the Windows
// half: Win32 + Direct3D 11; this is AppKit + Metal).
//
// The class is the same shape as the Windows one, method for method, so a new
// feature added there lands here in the same place:
//
//   show()      create the window, the Metal device and the ImGui context
//   frame()     called from gui's timer (30 frames a second), like WM_TIMER
//   visible()   whether the window is on the screen right now
//   shutdown()  gui is ending; tell the view (unmute the overview, and so on)
//
// Three windows share this class (the editor, the overview and the insertion
// editor) and **each owns an ImGui context**, which is why imgui_impl_osx is
// not used here: that backend installs one event monitor for the whole process
// and feeds whatever context happens to be current, so with three contexts the
// input would arrive in the wrong window. Instead the view hands the events to
// its own window's context, which is what ImGui_ImplWin32_WndProcHandler does
// on the other side.
//
// Drawing goes to a CAMetalLayer, one frame per frame() call -- the Windows
// side presents the swap chain with no wait, and so does this: the gui timer
// decides the pace.

#include "ui/pc_window.h"

#include "ui/lang.h"
#include "ui/font_file.h"
#include "ui/texts.h"

#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>

#include "imgui.h"
#include "backends/imgui_impl_metal.h"

#include <cfloat>
#include <cstring>

@class SMUEditorView;
@class SMUEditorWinDelegate;

namespace ui {

void pc_window_drop_file(const std::string &path)
{
	if (pc_window::s_drop)
		pc_window::s_drop(path);
}

namespace lang_detail {

// The OS default locale for ui::locale_default_lang (ui/lang.h): reporting
// the tag is all this backend does (CoreFoundation comes with Cocoa above).
// Finder-launched apps have no LANG, so Japanese Macs would otherwise come
// up English.
namespace {
std::string query_os_locale()
{
	CFLocaleRef loc = CFLocaleCopyCurrent();
	if (!loc)
		return {};
	const CFTypeRef v = CFLocaleGetValue(loc, kCFLocaleLanguageCode);
	char tag[16] = {};
	const bool known = v && CFGetTypeID(v) == CFStringGetTypeID() &&
	    CFStringGetCString((CFStringRef)v, tag, sizeof(tag),
	                       kCFStringEncodingUTF8);
	CFRelease(loc);
	return known ? tag : "";
}
const os_locale_registrar os_locale_reg(query_os_locale);
} // namespace

} // namespace lang_detail

} // namespace ui

// The window, the view and the Metal plumbing, in one place. pc_window (C++)
// holds a pointer to this
struct host {
	NSWindow              *win   = nil;
	SMUEditorView         *view  = nil;
	CAMetalLayer          *layer = nil;
	id<MTLDevice>          dev   = nil;
	id<MTLCommandQueue>    queue = nil;
	// Kept here because NSWindow holds its delegate **weakly**: without a
	// strong reference this dies at the end of create() and closing the window
	// later is a message to freed memory
	SMUEditorWinDelegate  *delegate = nil;
};

// ---------------------------------------------------------------------------
// The view. Feeds the events to **its** window's ImGui context

@interface SMUEditorView : NSView
{
@public
	host            *_h;        // the window, the layer and the Metal device
	ImGuiContext    *_ctx;      // this window's ImGui context
}
// Retained so it can be cleared in -updateTrackingAreas when bounds change
@property (nonatomic) NSTrackingArea *hover_area;
@end

@implementation SMUEditorView

- (instancetype)initWithFrame:(NSRect)frame host:(host *)h
{
	self = [super initWithFrame:frame];
	if (self) {
		_h = h;
		_ctx = nullptr;
		// The Metal layer is made here and **hosted**, not just asked for with
		// +layerClass: AppKit's own layer backing is NSViewBackingLayer and it
		// does not always honour a layerClass override, which ended in an
		// "unrecognized selector sent to NSViewBackingLayer" the first time a
		// window was opened. Hosting the layer means this exact object is used
		CAMetalLayer *layer = [CAMetalLayer layer];
		layer.device = h->dev;
		layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
		layer.framebufferOnly = YES;
		[self setLayer:layer];
		[self setWantsLayer:YES];
		h->layer = layer;
		// A MIDI file dropped on an editor window plays, the same as on the
		// panel window and the same as WM_DROPFILES on Windows
		[self registerForDraggedTypes:@[ NSPasteboardTypeFileURL ]];
		// mouseMoved: is opted in per view with a tracking area, in
		// -updateTrackingAreas below
	}
	return self;
}

- (BOOL)acceptsFirstResponder
{
	return YES;
}

- (BOOL)acceptsFirstMouse:(NSEvent *)event
{
	(void)event;
	return YES;
}

- (BOOL)isFlipped
{
	// ImGui's y runs down, like GDI's. Flipping the view means no conversion
	// at all in the draw and none in the mouse events either
	return YES;
}

// ---- mouse

- (void)feedModifiers:(NSEvent *)event
{
	ImGuiIO &io = ImGui::GetIO();
	const NSEventModifierFlags f = [event modifierFlags];
	io.AddKeyEvent(ImGuiMod_Shift, (f & NSEventModifierFlagShift) != 0);
	io.AddKeyEvent(ImGuiMod_Ctrl,  (f & NSEventModifierFlagControl) != 0);
	io.AddKeyEvent(ImGuiMod_Alt,   (f & NSEventModifierFlagOption) != 0);
	io.AddKeyEvent(ImGuiMod_Super, (f & NSEventModifierFlagCommand) != 0);
}

- (void)feedMousePos:(NSEvent *)event
{
	ImGuiIO &io = ImGui::GetIO();
	const NSPoint p = [self convertPoint:[event locationInWindow] fromView:nil];
	io.AddMousePosEvent((float)p.x, (float)p.y);
}

// The whole view is one tracking area: AppKit keeps -mouseMoved: coming on
// plain moves (the drag and down handlers need no opt-in)
- (void)updateTrackingAreas
{
	[super updateTrackingAreas];
	if (self.hover_area) {
		[self removeTrackingArea:self.hover_area];
		self.hover_area = nil;
	}
	if (NSIsEmptyRect(self.bounds))
		return;
	// NSTrackingMouseEnteredAndExited updates _hover, NSTrackingActiveAlways
	// keeps it alive when the window is not key (a palette-like window)
	NSTrackingAreaOptions options = NSTrackingMouseEnteredAndExited | NSTrackingMouseMoved |
	                                NSTrackingActiveAlways | NSTrackingInVisibleRect;
	self.hover_area = [[NSTrackingArea alloc] initWithRect:self.bounds options:options owner:self userInfo:nil];
	[self addTrackingArea:self.hover_area];
}

- (void)mouseDown:(NSEvent *)event
{
	ImGui::SetCurrentContext(_ctx);
	ImGuiIO &io = ImGui::GetIO();
	[self feedMousePos:event];
	io.AddMouseButtonEvent((int)[event buttonNumber], true);
}

- (void)rightMouseDown:(NSEvent *)event
{
	// buttonNumber is 1 for the secondary button; the plain handler covers it
	[self mouseDown:event];
}

- (void)mouseUp:(NSEvent *)event
{
	ImGui::SetCurrentContext(_ctx);
	ImGuiIO &io = ImGui::GetIO();
	[self feedMousePos:event];
	io.AddMouseButtonEvent((int)[event buttonNumber], false);
}

- (void)rightMouseUp:(NSEvent *)event
{
	[self mouseUp:event];
}

- (void)mouseMoved:(NSEvent *)event        { [self mouseDragged:event]; }
- (void)rightMouseDragged:(NSEvent *)event { [self mouseDragged:event]; }
- (void)otherMouseDragged:(NSEvent *)event { [self mouseDragged:event]; }

- (void)mouseDragged:(NSEvent *)event
{
	ImGui::SetCurrentContext(_ctx);
	[self feedMousePos:event];
}

- (void)mouseEntered:(NSEvent *)event
{
	// entered/exited can arrive while another window's context is the current
	// one; every handler sets its own context first, this one was missing it
	ImGui::SetCurrentContext(_ctx);
	[self feedMousePos:event];
}

- (void)mouseExited:(NSEvent *)event
{
	(void)event;
	// While ImGui is dragging (WantCaptureMouse) the dragged: events keep
	// coming even outside the view and carry the position, so leave it;
	// otherwise the cursor is really gone and ImGui should stop hovering
	ImGui::SetCurrentContext(_ctx);
	if (!ImGui::GetIO().WantCaptureMouse)
		ImGui::GetIO().AddMousePosEvent(-FLT_MAX, -FLT_MAX);
}

- (void)scrollWheel:(NSEvent *)event
{
	ImGui::SetCurrentContext(_ctx);
	ImGuiIO &io = ImGui::GetIO();
	// A tap to stop the scrolling arrives as a cancelled event carrying large
	// deltas; it is not a scroll (imgui_impl_osx ignores it for the same reason)
	if ([event phase] == NSEventPhaseCancelled)
		return;
	double dx = [event scrollingDeltaX];
	double dy = [event scrollingDeltaY];
	// A trackpad reports points, a wheel reports notches; ImGui wants wheels
	if ([event hasPreciseScrollingDeltas]) {
		dx *= 0.01;
		dy *= 0.01;
	} else {
		dx *= 0.1;
		dy *= 0.1;
	}
	if (dx != 0.0 || dy != 0.0)
		io.AddMouseWheelEvent((float)dx, (float)dy);
}

// ---- keys

// The virtual key codes ImGui cares about. The same table imgui_impl_osx
// keeps, written out so this file does not have to pull in <Carbon/Carbon.h>
// (window_mac.mm avoids it the same way). Everything else falls through to
// text input
static ImGuiKey key_from_code(unsigned short code)
{
	switch (code) {
	// ---- modifiers and editing keys
	case 0x24: return ImGuiKey_Enter;          // Return
	case 0x30: return ImGuiKey_Tab;
	case 0x31: return ImGuiKey_Space;
	case 0x33: return ImGuiKey_Backspace;      // Delete, the backwards one
	case 0x35: return ImGuiKey_Escape;
	case 0x4C: return ImGuiKey_KeypadEnter;
	case 0x72: return ImGuiKey_Insert;         // Help, which is where Insert went
	case 0x73: return ImGuiKey_Home;
	case 0x74: return ImGuiKey_PageUp;
	case 0x75: return ImGuiKey_Delete;         // Forward delete
	case 0x77: return ImGuiKey_End;
	case 0x79: return ImGuiKey_PageDown;
	// ---- function keys, scattered on purpose on the real keyboards
	case 0x7A: return ImGuiKey_F1;
	case 0x78: return ImGuiKey_F2;
	case 0x63: return ImGuiKey_F3;
	case 0x76: return ImGuiKey_F4;
	case 0x60: return ImGuiKey_F5;
	case 0x61: return ImGuiKey_F6;
	case 0x62: return ImGuiKey_F7;
	case 0x64: return ImGuiKey_F8;
	case 0x65: return ImGuiKey_F9;
	case 0x6D: return ImGuiKey_F10;
	case 0x67: return ImGuiKey_F11;
	case 0x6F: return ImGuiKey_F12;
	// ---- punctuation, in the hardware's order
	case 0x18: return ImGuiKey_Equal;
	case 0x1B: return ImGuiKey_Minus;
	case 0x1E: return ImGuiKey_RightBracket;
	case 0x21: return ImGuiKey_LeftBracket;
	case 0x27: return ImGuiKey_Apostrophe;
	case 0x29: return ImGuiKey_Semicolon;
	case 0x2A: return ImGuiKey_Backslash;
	case 0x2B: return ImGuiKey_Comma;
	case 0x2C: return ImGuiKey_Slash;
	case 0x2F: return ImGuiKey_Period;
	case 0x32: return ImGuiKey_GraveAccent;
	// ---- the digits, the top row of the keyboard
	case 0x1D: return ImGuiKey_0;
	case 0x12: return ImGuiKey_1;
	case 0x13: return ImGuiKey_2;
	case 0x14: return ImGuiKey_3;
	case 0x15: return ImGuiKey_4;
	case 0x17: return ImGuiKey_5;
	case 0x16: return ImGuiKey_6;
	case 0x1A: return ImGuiKey_7;
	case 0x1C: return ImGuiKey_8;
	case 0x19: return ImGuiKey_9;
	// ---- the keypad
	case 0x41: return ImGuiKey_KeypadDecimal;
	case 0x43: return ImGuiKey_KeypadMultiply;
	case 0x45: return ImGuiKey_KeypadAdd;
	case 0x4B: return ImGuiKey_KeypadDivide;
	case 0x4E: return ImGuiKey_KeypadSubtract;
	case 0x4F: return ImGuiKey_KeypadEqual;
	case 0x52: return ImGuiKey_Keypad0;
	case 0x53: return ImGuiKey_Keypad1;
	case 0x54: return ImGuiKey_Keypad2;
	case 0x55: return ImGuiKey_Keypad3;
	case 0x56: return ImGuiKey_Keypad4;
	case 0x57: return ImGuiKey_Keypad5;
	case 0x58: return ImGuiKey_Keypad6;
	case 0x59: return ImGuiKey_Keypad7;
	case 0x5A: return ImGuiKey_Keypad8;
	case 0x5B: return ImGuiKey_Keypad9;
	// ---- the letters. A S D F H G Z X C V (ISO) B Q W E R: the left hand,
	// scattered, in the order the hardware reports them
	case 0x00: return ImGuiKey_A;
	case 0x0B: return ImGuiKey_B;
	case 0x08: return ImGuiKey_C;
	case 0x02: return ImGuiKey_D;
	case 0x0E: return ImGuiKey_E;
	case 0x03: return ImGuiKey_F;
	case 0x05: return ImGuiKey_G;
	case 0x04: return ImGuiKey_H;
	case 0x22: return ImGuiKey_I;
	case 0x26: return ImGuiKey_J;
	case 0x28: return ImGuiKey_K;
	case 0x25: return ImGuiKey_L;
	case 0x2E: return ImGuiKey_M;
	case 0x2D: return ImGuiKey_N;
	case 0x1F: return ImGuiKey_O;
	case 0x23: return ImGuiKey_P;
	case 0x0C: return ImGuiKey_Q;
	case 0x0F: return ImGuiKey_R;
	case 0x01: return ImGuiKey_S;
	case 0x11: return ImGuiKey_T;
	case 0x20: return ImGuiKey_U;
	case 0x09: return ImGuiKey_V;
	case 0x0D: return ImGuiKey_W;
	case 0x07: return ImGuiKey_X;
	case 0x10: return ImGuiKey_Y;
	case 0x06: return ImGuiKey_Z;
	default: break;
	}
	return ImGuiKey_None;
}

- (void)keyDown:(NSEvent *)event
{
	ImGui::SetCurrentContext(_ctx);
	ImGuiIO &io = ImGui::GetIO();
	[self feedModifiers:event];
	const ImGuiKey key = key_from_code([event keyCode]);
	if (key != ImGuiKey_None)
		io.AddKeyEvent(key, true);
	// Text, where there is any. The views' text boxes are the only readers, so only while one is
	// active: a held key (playing notes from the keyboard) repeats text, and ImGui trickles text
	// against mouse moves, so a drag would lag further and further behind
	if (!io.WantTextInput)
		return;
	NSString *chars = [event characters];
	for (NSUInteger i = 0; i < [chars length]; i++)
		io.AddInputCharacterUTF16([chars characterAtIndex:i]);
}

- (void)keyUp:(NSEvent *)event
{
	ImGui::SetCurrentContext(_ctx);
	ImGuiIO &io = ImGui::GetIO();
	[self feedModifiers:event];
	const ImGuiKey key = key_from_code([event keyCode]);
	if (key != ImGuiKey_None)
		io.AddKeyEvent(key, false);
}

- (void)flagsChanged:(NSEvent *)event
{
	// Only the modifiers themselves changed. Holding a modifier must not read
	// as a key press, and letting one up must not read as a release of a key
	// the views never saw down
	ImGui::SetCurrentContext(_ctx);
	[self feedModifiers:event];
}

// ---- the Metal layer follows the window

- (void)layout
{
	[super layout];
	if (_h && _h->layer) {
		const CGFloat scale = [self window].backingScaleFactor;
		_h->layer.contentsScale = scale;
		const NSSize b = [self bounds].size;
		_h->layer.drawableSize = NSMakeSize(b.width * scale, b.height * scale);
	}
}
- (void)viewDidChangeBackingProperties
{
	[super viewDidChangeBackingProperties];
	[self layout];
}

// ---- dropping a file

- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)sender
{
	NSPasteboard *pb = [sender draggingPasteboard];
	return [[pb types] containsObject:NSPasteboardTypeFileURL] ? NSDragOperationCopy
	                                                          : NSDragOperationNone;
}

- (BOOL)prepareForDragOperation:(id<NSDraggingInfo>)sender
{
	(void)sender;
	return YES;
}

- (BOOL)performDragOperation:(id<NSDraggingInfo>)sender
{
	NSPasteboard *pb = [sender draggingPasteboard];
	NSArray<NSURL *> *urls = [pb readObjectsForClasses:@[ [NSURL class] ]
	                                          options:@{ NSPasteboardURLReadingFileURLsOnlyKey : @YES }];
	NSURL *url = [urls firstObject];
	if (!url)
		return NO;
	const char *path = [[url path] UTF8String];
	if (!path)
		return NO;
	ui::pc_window_drop_file(std::string(path));
	return YES;
}

@end

// Closing an editor window hides it, the way WM_CLOSE calls ShowWindow(SW_HIDE)
// instead of DestroyWindow: opening it again brings back the same state

@interface SMUEditorWinDelegate : NSObject <NSWindowDelegate>
{
@public
	host *_h;
}
@end

@implementation SMUEditorWinDelegate

- (BOOL)windowShouldClose:(NSWindow *)sender
{
	(void)sender;
	[_h->win orderOut:nil];
	return NO;
}

@end

// ---------------------------------------------------------------------------
// Helpers

namespace {

// The title comes from imgui_view as UTF-32 (wchar_t is 32 bits here, unlike
// Windows). The sources are UTF-8, so the other way is a plain conversion
NSString *title_of(const ui::imgui_view &view)
{
	const wchar_t *w = view.title();
	std::string utf8;
	for (; w && *w; w++) {
		const unsigned int c = unsigned(*w);
		if (c < 0x80) {
			utf8 += char(c);
		} else if (c < 0x800) {
			utf8 += char(0xc0 | (c >> 6));
			utf8 += char(0x80 | (c & 0x3f));
		} else if (c < 0x10000) {
			utf8 += char(0xe0 | (c >> 12));
			utf8 += char(0x80 | ((c >> 6) & 0x3f));
			utf8 += char(0x80 | (c & 0x3f));
		} else {
			utf8 += char(0xf0 | (c >> 18));
			utf8 += char(0x80 | ((c >> 12) & 0x3f));
			utf8 += char(0x80 | ((c >> 6) & 0x3f));
			utf8 += char(0x80 | (c & 0x3f));
		}
	}
	return [NSString stringWithUTF8String:utf8.c_str()];
}

} // namespace

// ---------------------------------------------------------------------------

namespace ui {

pc_window::~pc_window()
{
	destroy();
}

bool pc_window::visible() const
{
	const host *h = (const host *)m_ns;
	return h && h->win && [h->win isVisible];
}

void pc_window::hide()
{
	const host *h = (const host *)m_ns;
	if (h && h->win)
		[h->win orderOut:nil];
}

void pc_window::shutdown(bridge &br)
{
	if (!m_imgui)
		return;
	ImGui::SetCurrentContext(m_imgui);
	m_view->hidden(br);
}

bool pc_window::show(std::string &err)
{
	if (!m_ns && !create(err))
		return false;
	host *h = (host *)m_ns;
	[h->win orderFront:nil];
	[h->win makeKeyAndOrderFront:nil];
	// SetForegroundWindow's counterpart -- but only once the application is
	// already running. Coming up, the activation is window_mac.mm's business
	if ([NSApp isRunning]) {
		if (@available(macOS 14.0, *))
			[NSApp activate];
		else
			[NSApp activateIgnoringOtherApps:YES];
	}
	return true;
}

bool pc_window::create(std::string &err)
{
	[NSApplication sharedApplication];    // idempotent, and wanted before NSWindow

	host *h = new host;
	m_ns = h;

	h->dev = MTLCreateSystemDefaultDevice();
	if (!h->dev) {
		delete h;
		m_ns = nullptr;
		err = UI_TEXT(dlg_metal_fail, "Cannot use Metal");   // user-facing, matches the rest of gui's messages
		return false;
	}
	h->queue = [h->dev newCommandQueue];

	NSRect frame = NSMakeRect(0, 0, m_view->default_width(), m_view->default_height());
	const NSWindowStyleMask style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
	                                NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable;
	h->win = [[NSWindow alloc] initWithContentRect:frame
	                                      styleMask:style
	                                        backing:NSBackingStoreBuffered
	                                          defer:NO];
	[h->win setTitle:title_of(*m_view)];
	[h->win center];

	SMUEditorWinDelegate *delegate = [[SMUEditorWinDelegate alloc] init];
	delegate->_h = h;
	[h->win setDelegate:delegate];
	h->delegate = delegate;

	h->view = [[SMUEditorView alloc] initWithFrame:frame host:h];
	[h->win setContentView:h->view];
	[h->win makeFirstResponder:h->view];
	// Tracking areas deliver mouseMoved: to the view on their own; the window
	// flag is the older, wider gate. Set it too so hover works either way
	[h->win setAcceptsMouseMovedEvents:YES];

	IMGUI_CHECKVERSION();
	// a context per window (ImGui attaches to the current one) -- the same as pc_window.cpp
	m_imgui = ImGui::CreateContext();
	ImGui::SetCurrentContext(m_imgui);
	ImGuiIO &io = ImGui::GetIO();
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
	io.IniFilename = nullptr;       // no imgui.ini littered into the working folder

	ImGui::StyleColorsDark();
	ImGuiStyle &st = ImGui::GetStyle();
	st.FrameRounding = 3;

	// The one shared font setup: ui/font_file.h asks CoreText for a face by
	// family name (never a hard-coded path into /System), checks it can draw
	// what the panel writes, and reads it once.
	add_cjk_ui_font(io.Fonts);

	ImGui_ImplMetal_Init(h->dev);
	h->view->_ctx = m_imgui;
	return true;
}

void pc_window::destroy()
{
	if (m_imgui) {
		ImGui::SetCurrentContext(m_imgui);
		ImGui_ImplMetal_Shutdown();
		ImGui::DestroyContext(m_imgui);
		m_imgui = nullptr;
	}
	if (host *h = (host *)m_ns) {
		[h->win orderOut:nil];
		[h->win setDelegate:nil];
		delete h;
		m_ns = nullptr;
	}
}

void pc_window::frame(xg::model &m, const xg_snapshot &ram, bridge &br)
{
	host *h = (host *)m_ns;
	const bool shown = m_imgui && h && h->win && [h->win isVisible] && ![h->win isMiniaturized];
	if (m_was_visible && !shown && m_imgui) {
		ImGui::SetCurrentContext(m_imgui);
		m_view->hidden(br);                  // closed or miniaturized: release the held-down buttons
	}
	m_was_visible = shown;
	if (!shown)
		return;
	ImGui::SetCurrentContext(m_imgui);
	ImGuiIO &io = ImGui::GetIO();

	const NSRect b = [h->view bounds];
	const CGFloat scale = [h->win backingScaleFactor];
	io.DisplaySize = ImVec2(float(b.size.width), float(b.size.height));
	io.DisplayFramebufferScale = ImVec2(float(scale), float(scale));

	// The drawable decides the render pass, and the pass is wanted before
	// NewFrame (the same order the Metal example uses)
	id<CAMetalDrawable> drawable = [h->layer nextDrawable];
	if (!drawable)
		return;
	MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
	pass.colorAttachments[0].texture = drawable.texture;
	pass.colorAttachments[0].loadAction = MTLLoadActionClear;
	pass.colorAttachments[0].clearColor = MTLClearColorMake(0.10, 0.10, 0.11, 1.0);
	pass.colorAttachments[0].storeAction = MTLStoreActionStore;

	ImGui_ImplMetal_NewFrame(pass);
	ImGui::NewFrame();
	m_view->draw(m, ram, br);
	xgui::drag_flush(br);          // マウスで動かしている値の、間引いた送信
	ImGui::Render();

	id<MTLCommandBuffer> buf = [h->queue commandBuffer];
	id<MTLRenderCommandEncoder> enc = [buf renderCommandEncoderWithDescriptor:pass];
	ImGui_ImplMetal_RenderDrawData(ImGui::GetDrawData(), buf, enc);
	[enc endEncoding];
	[buf presentDrawable:drawable];
	[buf commit];
	// no wait: gui's timer (30 frames a second) decides the pace
}

} // namespace ui
