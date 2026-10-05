// license:BSD-3-Clause
//
// AUv3 を入れておくための器のアプリ。
//
// macOS はアプリの中に入っている .appex しか AUv3 として認めないので、
// プラグインだけを配ることができない。このアプリ自体は音を出さない。
// 一度起動するとシステムが中の .appex を見つけ、DAW の一覧に出るようになる。
//
// **ROM はバンドルに入れない。** 画像が Yamaha のものなので、渡す物に載せる
// ことはできない。だからこの窓は「ROM をどこから取るか」を一度だけ訊いて、
// 拡張のアプリケーションサポートに置いてやる:
//
//   ~/Library/Containers/<拡張の id>/Data/Library/Application Support/S-MU2000
//
// This is the one place a sandboxed extension can read outside its own bundle.
// $HOME inside the .appex is its container, so this is exactly what
// config_dir() answers in the plug-in (src/vst3/engine.cpp), and the same
// directory the engine already writes log.txt and boot snapshots into. Once
// the files are here the engine finds them by itself: neither the bundle nor
// the search order is involved. A ROM directory is an ordinary copy of the
// user's own files, so it can be handed around or deleted like one.
//
// A sandboxed process may only write inside its own container, and the files
// have to land in the *extension's*. So this app is signed without
// com.apple.security.app-sandbox, while the .appex keeps its own (an app
// extension outside a sandbox never registers). packaging/auv3-app.entitlements.
//
// 窓なしでやる道もある:
//
//   open -a S-MU2000.app --args --install-roms /path/to/roms

#import <Cocoa/Cocoa.h>
#import <Security/Security.h>

#include "roms_dir.h"

#include <cstdio>
#include <string>
#include <vector>

namespace {

// Width of the window, in points. Fixed rather than resizable: every path in
// the status is a filesystem path, and a window too narrow for them turns the
// answer into a puzzle
constexpr CGFloat BOX_WIDTH = 620;

// The .appex we carry, or nil. It is the only AUv3 macOS will register, and
// its bundle id names both the container and the extension a DAW gets
NSBundle *find_appex(void)
{
	NSString *plugIns = [[NSBundle mainBundle] builtInPlugInsPath];
	for (NSString *name in [[NSFileManager defaultManager] contentsOfDirectoryAtPath:plugIns
	                                                                           error:nil]) {
		if ([name.pathExtension isEqualToString:@"appex"])
			return [NSBundle bundleWithPath:[plugIns stringByAppendingPathComponent:name]];
	}
	return nil;
}

// Where this app's files of its own belong: ~/Library/Application Support.
//
// Asked of Foundation rather than spelled out. NSFileManager already knows the
// per-user location on every macOS version, and it is the same answer the
// search in src/vst3/engine.cpp is built on (config_dir() in
// src/compat/paths.h), so the window and the plug-in cannot disagree about
// where "the user's own" is.
NSURL *application_support_dir(void)
{
	return [[NSFileManager defaultManager] URLForDirectory:NSApplicationSupportDirectory
	                                               inDomain:NSUserDomainMask
	                                      appropriateForURL:nil
	                                                create:NO
	                                                 error:NULL];
}

// The extension's own Application Support directory: its container's, since a
// sandboxed process can read nothing outside its own.
//
//   ~/Library/Containers/<appex id>/Data/Library/Application Support/S-MU2000
//
// Foundation has no call for "another bundle's container" -- containerURLFor-
// SecurityApplicationGroupIdentifier: only answers for app groups this app
// belongs to, and asking for one we do not belong to would put a group
// entitlement in the signature. The layout above is the documented one, and it
// is spelled with NSURL so no separator is written by hand.
//
// NSHomeDirectoryForUser() rather than NSHomeDirectory(): if this app is ever
// run sandboxed after all, the latter points at *our* container and the path
// built from it would be wrong in a way that looks right. Inside the .appex the
// directory reached here is exactly what config_dir() returns, because macOS
// points $HOME at that container's Data.
NSURL *extension_support_dir(NSBundle *appex)
{
	NSString *ident = appex.bundleIdentifier;
	if (ident.length == 0)
		return nil;
	NSURL *url = [NSURL fileURLWithPath:NSHomeDirectoryForUser(nil) isDirectory:YES];
	for (NSString *part in @[ @"Library", @"Containers", ident, @"Data" ])
		url = [url URLByAppendingPathComponent:part isDirectory:YES];
	return [url URLByAppendingPathComponent:@"Library/Application Support/S-MU2000"
	                             isDirectory:YES];
}

// Where the desktop app keeps its ROMs. The same name under the user's own
// Application Support (not the extension's container), so someone already
// running S-MU2000 on this machine has a set there to copy
NSURL *shared_roms_dir(void)
{
	return [application_support_dir() URLByAppendingPathComponent:@"S-MU2000/roms"
	                                                   isDirectory:YES];
}

// Whether this process carries the App Sandbox entitlement, asked of the
// security framework rather than guessed from the path shape.
//
// This app must not be sandboxed: a sandboxed process can only write inside its
// own container, and the files have to land in the *extension's*. Checking the
// entitlement says exactly that. The fallback is for the (unlikely) case of the
// security framework refusing, where being wrong the other way only costs an
// error message from the copy itself.
BOOL app_is_sandboxed(void)
{
	SecTaskRef task = SecTaskCreateFromSelf(NULL);
	if (!task)
		return NO;
	CFTypeRef value = SecTaskCopyValueForEntitlement(task, CFSTR("com.apple.security.app-sandbox"), NULL);
	const bool on = value && CFGetTypeID(value) == CFBooleanGetTypeID() &&
	                CFBooleanGetValue((CFBooleanRef)value);
	if (value)
		CFRelease(value);
	CFRelease(task);
	return on;
}

// Text the window shows. Only for display: a path on screen is text, so a
// missing file becomes a string here, at the last moment, and nowhere else.
std::string to_std(NSString *s)
{
	return s.length ? std::string(s.UTF8String) : std::string();
}

NSString *to_ns(const std::string &s)
{
	return s.empty() ? @"" : [NSString stringWithUTF8String:s.c_str()];
}

// A path as the installer wants it. NSURL is already a path object, so this
// converts once, at the boundary, and everything past it stays a path: no
// string ever holds a separator on the way to install_roms or roms_missing.
smu2000::fs::path to_path(NSURL *u)
{
	return u ? smu2000::fs::path(to_std(u.path)) : smu2000::fs::path();
}

// A label that takes as many lines as its text needs. The status is a
// paragraph (paths, lists of files), so single-line mode is off; the height
// then follows from the text and the layout constraints give it a width
NSTextField *make_paragraph(BOOL monospaced)
{
	NSTextField *f = [[NSTextField alloc] initWithFrame:NSZeroRect];
	f.editable = NO;
	f.selectable = YES;              // a path is worth copying out of the window
	f.bezeled = NO;
	f.bordered = NO;
	f.drawsBackground = NO;
	f.alignment = NSTextAlignmentLeft;
	f.font = monospaced ? [NSFont monospacedSystemFontOfSize:11
	                                                  weight:NSFontWeightRegular]
	                    : [NSFont systemFontOfSize:12];
	f.lineBreakMode = NSLineBreakByWordWrapping;
	f.usesSingleLineMode = NO;
	f.maximumNumberOfLines = 0;
	f.cell.wraps = YES;
	return f;
}

} // namespace


@interface AppDelegate : NSObject <NSApplicationDelegate>
@end

@implementation AppDelegate {
	NSWindow       *_window;
	NSTextField    *_status;
	NSButton       *_choose;
	NSButton       *_shared;
	NSButton       *_reveal;
	NSURL         *_support;   // the extension's Application Support directory
	NSURL         *_dest;       // .../S-MU2000/roms, where the files go
}

- (void)applicationDidFinishLaunching:(NSNotification *)note
{
	(void)note;

	_support = extension_support_dir(find_appex());
	_dest = _support ? [_support URLByAppendingPathComponent:@"roms" isDirectory:YES] : nil;

	// Make the directory even before anything is put in it, so the path shown
	// below is one Finder can open
	if (_dest) {
		std::error_code ec;
		smu2000::fs::create_directories(to_path(_dest), ec);
	}

	const NSRect frame = NSMakeRect(0, 0, BOX_WIDTH, 470);
	_window = [[NSWindow alloc]
	    initWithContentRect:frame
	              styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
	                         NSWindowStyleMaskMiniaturizable)
	                backing:NSBackingStoreBuffered
	                  defer:NO];
	_window.title = @"S-MU2000 AUv3";
	[_window center];

	NSTextField *label = make_paragraph(NO);
	label.stringValue = @"This app installs the AUv3 plug-in. It makes no sound "
	                     "of its own.\nOpen it once and the AUv3 appears in your "
	                     "DAW's instrument list.";

	_status = make_paragraph(YES);
	_choose = [NSButton buttonWithTitle:@"Install ROMs\u2026"
	                             target:self
	                             action:@selector(choose:)];
	_shared = [NSButton buttonWithTitle:@"Install from Application Support"
	                             target:self
	                             action:@selector(shared:)];
	_reveal  = [NSButton buttonWithTitle:@"Reveal Plug-in Folder"
	                             target:self
	                             action:@selector(reveal:)];

	NSTextView *text = [[NSTextView alloc] initWithFrame:NSZeroRect];
	text.editable = NO;
	text.drawsBackground = NO;
	text.font = [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular];
	text.string = [NSString stringWithFormat:
	    @"  Output     MAIN OUT L/R\n"
	     "  Input      A/D INPUT (AD1 left, AD2 right)\n"
	     "  MIDI in    cable 0 = IN A (parts 1-16)\n"
	     "            cable 1 = IN B (parts 17-32)\n"
	     "            cable 2 = IN C (parts 33-48)\n"
	     "            cable 3 = IN D (parts 49-64)\n"
	     "  MIDI out   MIDI OUT (the firmware's replies)\n\n"
	     "A separate plug-in from the AUv2 (S-MU2000), so both can sit in a\n"
	     "session at once without either claiming the other's state."];

	NSScrollView *scroll = [[NSScrollView alloc] initWithFrame:NSZeroRect];
	scroll.documentView = text;
	scroll.hasVerticalScroller = YES;
	scroll.borderType = NSBezelBorder;
	// Text in a scroll view has no width of its own to wrap at. Without this
	// it asks for the width of its longest line and drags the window wider
	[text.widthAnchor constraintEqualToAnchor:scroll.contentView.widthAnchor].active = YES;

	// Plain constraints rather than stack views: the paragraphs have no height
	// of their own (it follows from the text), and a stack view spends a lot
	// of effort guessing one. Stated out, it just lays out.
	NSView *box = _window.contentView;
	for (NSView *v in @[ label, _status, _choose, _shared, _reveal, scroll ]) {
		[v setTranslatesAutoresizingMaskIntoConstraints:NO];
		[box addSubview:v];     // a view with no superview draws nothing
	}

	[NSLayoutConstraint activateConstraints:@[
		// The width is pinned here, not left to the window. A wrapping label has
		// no width of its own to measure, so the chain below (label's two edges,
		// then the scroll view's) leaves the box free to be any width at all --
		// and AppKit answers that freedom by shrinking the window to nothing.
		// The height is left flexible: it follows from the paragraphs, and the
		// window then grows with whatever the status has to say.
		[box.widthAnchor constraintEqualToConstant:BOX_WIDTH],

		[label.leadingAnchor  constraintEqualToAnchor:box.leadingAnchor constant:16],
		[label.trailingAnchor constraintEqualToAnchor:box.trailingAnchor constant:-16],
		[label.topAnchor      constraintEqualToAnchor:box.topAnchor constant:16],

		[_status.leadingAnchor  constraintEqualToAnchor:label.leadingAnchor],
		[_status.trailingAnchor constraintEqualToAnchor:label.trailingAnchor],
		[_status.topAnchor      constraintEqualToAnchor:label.bottomAnchor constant:14],

		[_choose.leadingAnchor constraintEqualToAnchor:box.leadingAnchor constant:16],
		[_choose.topAnchor     constraintEqualToAnchor:_status.bottomAnchor constant:14],
		[_shared.leadingAnchor constraintGreaterThanOrEqualToAnchor:_choose.trailingAnchor constant:8],
		[_shared.centerYAnchor constraintEqualToAnchor:_choose.centerYAnchor],
		[_reveal.leadingAnchor constraintGreaterThanOrEqualToAnchor:_shared.trailingAnchor constant:8],
		[_reveal.centerYAnchor constraintEqualToAnchor:_choose.centerYAnchor],
		[_reveal.trailingAnchor constraintLessThanOrEqualToAnchor:box.trailingAnchor constant:-16],

		[scroll.topAnchor      constraintEqualToAnchor:_choose.bottomAnchor constant:14],
		[scroll.leadingAnchor  constraintEqualToAnchor:label.leadingAnchor],
		[scroll.trailingAnchor constraintEqualToAnchor:label.trailingAnchor],
		[scroll.bottomAnchor   constraintEqualToAnchor:box.bottomAnchor constant:-16],
		// The paragraphs may take as many lines as they need, but the plug-in
		// description below always keeps some room
		[scroll.heightAnchor constraintGreaterThanOrEqualToConstant:160],
	]];

	[_window makeKeyAndOrderFront:nil];
	[NSApp activateIgnoringOtherApps:YES];

	[self refresh];
	// Layout only settles once the window is on screen, and this window's
	// content is nothing but constraints. Without a layout pass here the first
	// frame is drawn from unsatisfiable guesses
	[_window.contentView layoutSubtreeIfNeeded];
}

// What the plug-in will find, and where from. Everything here is read-only:
// this app's only write into the plug-in's world is the install.
- (void)refresh
{
	NSFileManager *fm = [NSFileManager defaultManager];
	NSURL *found = nil;       // where the plug-in will read them from
	NSString *note = nil;     // something worth saying about that place

	const std::vector<smu2000::fs::path> missing =
	    _dest ? smu2000::roms_missing(to_path(_dest))
	          : std::vector<smu2000::fs::path>{ smu2000::fs::path("mu2000_flash.bin") };
	if (missing.empty()) {
		found = _dest;
	} else {
		// A bundle built with AUV3_ROMS carries its own. Fine to use, but it is
		// exactly what stops the app from being handed out, so it is worth
		// naming: the plug-in would work on this machine and nowhere else
		NSURL *baked = [find_appex() URLForResource:@"mu2000_flash.bin"
		                                 withExtension:nil];
		if (baked) {
			found = [baked URLByDeletingLastPathComponent];
			note = @"These ROMs are inside the app bundle, so this build cannot be\n"
			       "handed out to anyone else. Installing your own set drops that\n"
			       "limitation.";
		}
	}

	NSMutableString *text = [NSMutableString string];
	if (found) {
		[text appendFormat:@"ROMs found. The plug-in will load them from:\n  %@\n\n", found.path];
		if (note)
			[text appendFormat:@"%@\n\n", note];
	} else if (!_support) {
		[text appendString:@"The plug-in inside this app could not be located, so there is\n"
		                   "nowhere to put ROMs. Reinstall the app and try again.\n\n"];
	} else if (app_is_sandboxed()) {
		[text appendString:@"This app is sandboxed and cannot write into the plug-in's own\n"
		                   "container, so it cannot install ROMs. Reinstall an app\n"
		                   "built with the sandbox off its own.\n\n"];
	} else {
		[text appendFormat:@"No ROMs yet. The AUv3 needs a dump of the MU2000's own program\n"
		                   "and wave ROMs, which cannot be shipped with the app.\n"
		                   "They are needed in:\n  %@\n\n", _dest.path];
		[text appendString:@"Missing right now:\n"];
		for (const smu2000::fs::path &rel : missing)
			[text appendFormat:@"  %s\n", to_ns(rel.string()).UTF8String];
		[text appendString:@"\nOptional (the plug-in has stand-ins): standin/sin-table.bin,\n"
		                   "hd44780u_b04.bin\n"];
	}

	// The desktop app keeps its ROMs in the same-named directory of the user's
	// own Application Support. Someone already running S-MU2000 on this machine
	// has a set there to copy, so nothing to hunt for
	NSURL *shared = shared_roms_dir();
	[_shared setEnabled:smu2000::has_roms(to_path(shared)) && _dest != nil];
	if ([_shared isEnabled])
		[text appendFormat:@"\nA set is also sitting in your Application Support folder:\n  %@\n",
		                  shared.path];

	_status.stringValue = text;
	[_choose setEnabled:_dest != nil];
	[_reveal setEnabled:_dest != nil && [fm fileExistsAtPath:_dest.path]];
}

- (void)choose:(id)sender
{
	(void)sender;
	NSOpenPanel *panel = [NSOpenPanel openPanel];
	panel.canChooseFiles = NO;
	panel.canChooseDirectories = YES;
	panel.allowsMultipleSelection = NO;
	panel.canCreateDirectories = NO;
	panel.prompt = @"Install";
	panel.message = @"Choose the folder holding the MU2000 ROM dump "
	                 "(mu2000_flash.bin and the dump/ folder with the four 8MB images).";
	// Start where a previous set would be, so re-picking it is one click
	NSURL *shared = shared_roms_dir();
	if ([[NSFileManager defaultManager] fileExistsAtPath:shared.path])
		panel.directoryURL = shared;

	if ([panel runModal] != NSModalResponseOK)
		return;
	NSURL *url = panel.URLs.firstObject;
	if (url)
		[self installFrom:url];
}

- (void)shared:(id)sender
{
	(void)sender;
	if (_dest)
		[self installFrom:shared_roms_dir()];
}

- (void)reveal:(id)sender
{
	(void)sender;
	if (_dest)
		[[NSWorkspace sharedWorkspace] activateFileViewerSelectingURLs:@[ _dest ]];
}

// 36MB of images, so off the main thread. The window says which folder it is
// working from rather than showing a sheet, and the copy keeps the app
// responsive either way
- (void)installFrom:(NSURL *)from
{
	if (!_dest)
		return;
	const smu2000::fs::path first_missing = [&] {
		const std::vector<smu2000::fs::path> m = smu2000::roms_missing(to_path(from));
		return m.empty() ? smu2000::fs::path() : m.front();
	}();
	if (!first_missing.empty()) {
		NSAlert *alert = [[NSAlert alloc] init];
		alert.messageText = @"That folder does not hold a complete set of ROMs.";
		alert.informativeText = [NSString stringWithFormat:
		    @"First thing missing: %@\n\n"
		     "A ROM folder holds mu2000_flash.bin (4MB) and dump/ with the four\n"
		     "8MB wave ROMs: xv364a0.ic49, xv365a0.ic50, xw848a0.ic53, xw849a0.ic54.",
		     to_ns(first_missing.string())];
		[alert addButtonWithTitle:@"OK"];
		[alert runModal];
		return;
	}

	// The buttons go quiet for the copy. The status line says what is happening
	// and where from, which is the whole of the feedback this needs -- 36MB off
	// a local disk is quick enough that a progress bar would only flicker
	[_choose setEnabled:NO];
	[_shared setEnabled:NO];
	[_reveal setEnabled:NO];
	_status.stringValue = [NSString stringWithFormat:@"Installing from:\n  %@\n\n36MB to copy.",
	                 from.path];

	const smu2000::fs::path src = to_path(from);
	const smu2000::fs::path dst = to_path(_dest);
	dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
		std::string why;
		const bool ok = smu2000::install_roms(src, dst, why);
		dispatch_async(dispatch_get_main_queue(), ^{
			[_choose setEnabled:YES];
			if (!ok) {
				_status.stringValue = [NSString stringWithFormat:@"Install failed: %s\n", why.c_str()];
				[_shared setEnabled:NO];
				[_reveal setEnabled:NO];
				[self refresh];
				return;
			}
			[self refresh];
			// The plug-in is already registered by now, and a DAW that has it
			// open holds the failure it saw, so say what has to happen next
			NSAlert *alert = [[NSAlert alloc] init];
			alert.messageText = @"ROMs installed.";
			alert.informativeText = @"Restart your DAW (or reopen the session). "
			                        "The AUv3 finds them from now on without asking.";
			[alert addButtonWithTitle:@"OK"];
			[alert runModal];
		});
	});
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)app
{
	(void)app;
	return YES;
}

@end

// Install without the window, for packaging scripts and for testing:
//
//   open -a S-MU2000.app --args --install-roms /path/to/roms
//
// Prints what it did and exits. Anything else opens the window.
static int install_from_argv(const char *roms_dir)
{
	NSURL *support = extension_support_dir(find_appex());
	if (!support) {
		std::fprintf(stderr, "no .appex inside this app\n");
		return 1;
	}
	if (app_is_sandboxed()) {
		std::fprintf(stderr, "this app is sandboxed: it cannot write into the "
		                     "plug-in's container\n");
		return 1;
	}
	NSURL *dest = [support URLByAppendingPathComponent:@"roms" isDirectory:YES];
	const smu2000::fs::path from =
	    smu2000::fs::path(to_std([NSString stringWithUTF8String:roms_dir]));
	std::string why;
	if (!smu2000::install_roms(from, to_path(dest), why)) {
		std::fprintf(stderr, "%s\n", why.c_str());
		return 1;
	}
	std::printf("ROMs installed: %s\n", to_path(dest).string().c_str());
	return 0;
}

int main(int argc, const char *argv[])
{
	if (argc == 3 && std::string(argv[1]) == "--install-roms")
		return install_from_argv(argv[2]);

	@autoreleasepool {
		NSApplication *app = [NSApplication sharedApplication];
		AppDelegate *del = [[AppDelegate alloc] init];
		app.delegate = del;
		[app setActivationPolicy:NSApplicationActivationPolicyRegular];
		[app run];
	}
	return 0;
}
