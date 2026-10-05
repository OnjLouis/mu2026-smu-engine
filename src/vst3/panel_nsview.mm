// license:BSD-3-Clause
//
// The panel in an NSView. See panel_nsview.h for why this is its own file.
//
// There is no second copy of the panel in here: what is built is the *same*
// view the VST3 build shows -- smu2000::vst3::plug_view, which is
// src/vst3/view.cpp drawing through Dear ImGui + Metal, inside the NSView from
// src/vst3/view_mac.mm. Only the way a host asks for it differs, which is the
// whole reason the three formats can share one engine and one editor.

#import "panel_nsview.h"

#include "engine.h"
#include "plug_window.h"
#include "view.h"

#include <cstdio>

// The frame a host is handed. It is only that: the panel, the child view that
// paints it and the input handling all belong to the plug_view inside, which
// this holds for as long as it lives
@interface SMU2000PanelView : NSView
{
@public
	smu2000::vst3::plug_view *_plug;
}
// What owns the engine the panel draws; see make_panel_view's note in the
// header. Holding it here puts ARC's release of the owner after this view's
// dealloc has finished, so plug_view is always torn down while the engine it
// writes back through is still there
@property (nonatomic, strong) id owner;
@end

@implementation SMU2000PanelView

- (BOOL)isFlipped { return YES; }

// The panel handles the clicks, but a host that hands the click to this frame
// (a window that does not take key focus makes the first click a "first mouse"
// one) should get the same answer as the panel does -- view_mac.mm gives the
// same one for the VST3 side
- (BOOL)acceptsFirstMouse:(NSEvent *)event
{
	(void)event;
	return YES;
}

- (void)dealloc
{
	// plug_view first: the engine (owner, above) is still alive here.
	// plug_view counts its own references (it implements FUnknown's addRef /
	// release); make_panel_view took one, and this gives it back
	if (_plug)
		_plug->release();
}

// A host that lets the editor window be resized moves this view's frame. The
// panel and the child view inside have to follow it, which is plug_view::onSize
- (void)setFrameSize:(NSSize)size
{
	[super setFrameSize:size];
	if (!_plug)
		return;
	Steinberg::ViewRect r(0, 0, (Steinberg::int32)size.width, (Steinberg::int32)size.height);
	_plug->onSize(&r);
}

@end


namespace smu2000 {
namespace vst3 {

NSView *make_panel_view(engine &eng, NSSize preferred, id owner)
{
	eng.log_line("画面を作る");
	plug_view *plug = new plug_view(eng);

	int w = (int)preferred.width;
	int h = (int)preferred.height;
	if (w < kPanelMinW || h < kPanelMinH) {
		w = kPanelWidth;
		h = kPanelHeight;
	}
	// onSize clamps the way the VST3 host's size is clamped, and resizes the
	// panel to match, so the frame below is what the panel was laid out for
	Steinberg::ViewRect r(0, 0, w, h);
	plug->onSize(&r);
	w = plug->width();
	h = plug->height();

	SMU2000PanelView *view = [[SMU2000PanelView alloc] initWithFrame:NSMakeRect(0, 0, w, h)];
	// attached() answers a VST3 tresult, where kResultOk is 0 -- so this is a
	// comparison and not a truth test, or a view that attached perfectly would
	// be thrown away
	const Steinberg::tresult ar = plug->attached((__bridge void *)view, plug_window_type());
	if (ar != Steinberg::kResultOk) {
		char b[64];
		std::snprintf(b, sizeof(b), "画面を貼れない: %d", int(ar));
		eng.log_line(b);
		plug->release();
		return nil;
	}
	view->_plug = plug;
	// So the panel outlives the engine it draws (note on the @property above)
	view.owner = owner;
	eng.log_line("画面ができた");
	return view;
}

} // namespace vst3
} // namespace smu2000
