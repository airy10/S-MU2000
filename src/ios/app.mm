// license:BSD-3-Clause
//
// ios/app.mm - the iOS standalone front end, step 1: the panel, drawn.
//
// This replaces the self-test harness (ios/smoke.mm, kept for the moment because
// it still answers "does the extension register?", which the standalone does not
// need). It is the iOS counterpart of src/gui.cpp, and it is short for the same
// reason gui.cpp is: the boot sequence, the panel and the MIDI routing live in
// ui/engine and ui/app, shared with the desktop front ends, so this file only has
// to make the app, load the ROMs and hand the panel a view.
//
// Step 1 deliberately stops before audio and input. What it answers is whether
// the ImGui panel renders on iOS at all - the same question for the app and for
// the AUv3, since both paint through ui/panel.cpp and neither knows what it is
// drawn on. Panel.cpp carries no hardcoded size: it takes the width paint_main
// passes and lays itself out, so the phone's dimensions are a matter of the host
// handing over a different number, not of changing the panel.
//
// No touch handling yet, so the panel shows but cannot be operated.

#import <UIKit/UIKit.h>

#include "compat/paths.h"
#include "mu2000.h"
#include "rom_search.h"
#include "ui/app_ios.h"
#include "ui/engine.h"
#include "ui/file_ask_ios.h"
#include "ui/rom_import_ios.h"
#include "ui/layout.h"
#include "ui/options.h"
#include "ui/tool_args.h"
#include "ui/window_ios.h"

#import <TargetConditionals.h>

#include <cstdio>
#include <cstring>
#include <memory>

namespace {

// The ROM directory, through the shared search (src/rom_search.h) rather than
// one hardcoded path: an installed set in the container (the "Install ROM
// files..." menu entry, src/ui/rom_import_ios.mm) has to win over a bundle copy,
// and that ordering is exactly what the desktop already does. The bundle's
// roms/ stays the last candidate, so `make ios-standalone IOS_ROMS=roms` still
// works for local builds.
//
// Deliberately NOT ../Resources/roms among the image_dir candidates' worth of
// assumptions: on the flat iOS bundle that resolves to a sibling of the .app,
// and any subdirectory under the app's Resources/ breaks ad-hoc codesign anyway
// (see the Makefile's IOS_PANEL_DIR comment for the bisection that proved it).
std::string rom_dir()
{
	std::string tried;
	const std::string here = smu2000::module_dir(reinterpret_cast<const void *>(&rom_dir));
	const std::string dir = smu2000::find_roms(here, false, tried);
	if (!tried.empty())
		std::fprintf(stderr, "[ios] ROM search:\n%s", tried.c_str());
	return dir;
}

} // namespace

// Whether the machine is up, kept next to the code that decides it rather than
// in the scene method, because boot_machine() also runs from the install
// callback.
static bool s_machine_up = false;

// The screen sleeps only while we are foreground *and* silent. UIKit clears
// the flag for us when the app leaves the foreground, which is why that half is
// re-asked on the way back rather than remembered.
static void keep_screen_awake()
{
	UIApplication.sharedApplication.idleTimerDisabled = s_machine_up;
}

static bool boot_machine_once(ui::gui_app &gui, ui::engine &eng, ui::tool_args &a);

// boot_machine_once() plus the one thing that follows from its answer: a
// running machine keeps the screen awake.
static bool boot_machine(ui::gui_app &gui, ui::engine &eng, ui::tool_args &a)
{
	const bool up = boot_machine_once(gui, eng, a);
	s_machine_up = up;
	keep_screen_awake();
	return up;
}

// Load the ROMs, boot the firmware, open the audio device. In one place
// because it runs twice when the app starts with no ROMs: once now (nothing to
// load), and once more the moment the user finishes installing them, so the
// machine comes up without a relaunch. Returns whether the machine is running.
static bool boot_machine_once(ui::gui_app &gui, ui::engine &eng, ui::tool_args &a)
{
	if (!gui.load_machine(eng, a)) {
		std::fprintf(stderr, "[ios] no ROMs: the panel comes up empty\n");
		return false;
	}
	if (!eng.boot()) {
		std::fprintf(stderr, "[ios] boot failed: %s\n", eng.message.c_str());
		return false;
	}
	ui::engine_options startup;
	startup.native_fx = gui.persisted_native_fx;
	startup.native_engine = gui.persisted_native_engine;
	gui.wire_engine(eng, startup);
	if (startup.native_fx) eng.mu.set_native_fx(startup.native_fx);
	gui.apply_native_engine(eng, startup);
	eng.state.store(1);
	// Open only after boot; failures use the shared LCD error state.
	if (gui.start_audio()) {
		gui.say_audio_opened(false);
		gui.say_audio_running();
	} else {
		gui.audio_failed = true;
		std::fprintf(stderr, "[ios] audio start failed; panel runs silent\n");
	}
	gui.audio_ready.store(true);
	std::fprintf(stderr, "[ios] booted\n");
	return true;
}

@interface SMUAppDelegate : UIResponder <UIWindowSceneDelegate>
@property (nonatomic, strong) UIWindow *window;
@end

@implementation SMUAppDelegate

// The ROM directory is resolved here, in Objective-C, because this is a .mm and
// NSSearchPathForDirectoriesInDomains is the correct way to find a bundle on
// iOS - and because module_dir() is the fallback for when that fails.
- (void)scene:(UIScene *)scene
	willConnectToSession:(UISceneSession *)session
	options:(UISceneConnectionOptions *)opts
{
	// The very first statement, before anything that can fail or return. Two rounds
	// of debugging ended here: a stale UISceneDelegateClassName, then a view built but
	// never attached to the hierarchy. Both presented as the same thing - a black
	// screen and *no log output at all* - because a scene delegate that is never
	// called and one that is called but dies early look identical from outside.
	// fprintf to stderr rather than NSLog: NSLog goes through the unified log and
	// needs the right predicate to be seen, whereas stderr is picked up by a stream
	// on the process with no filtering.
	std::fprintf(stderr, "[ios] scene willConnectToSession\n");
	std::fflush(stderr);
	(void)session;
	(void)opts;
	if (![scene isKindOfClass:UIWindowScene.class]) {
		std::fprintf(stderr, "[ios] scene is not a UIWindowScene (%s)\n",
		             [[scene class] UTF8String]);
		return;
	}

	self.window = [[UIWindow alloc] initWithWindowScene:(UIWindowScene *)scene];
	UIViewController *vc = [[UIViewController alloc] init];
	vc.view.backgroundColor = UIColor.blackColor;
	self.window.rootViewController = vc;
	[self.window makeKeyAndVisible];

	// The shared state and the app, laid out exactly as src/gui.cpp does: a
	// bridge, four MIDI inputs (mu2000::MIDI_PORTS of them) and three outputs
	// (THRU A, THRU B, and the machine's own OUT), then the app over them.
	static ui::bridge br;
	static ui::midi_in  midi_ports[ui::IN_PORTS];
	static ui::midi_out mout, mout_b, mout_mu;
	static ui::gui_app gui(br, midi_ports, mout, mout_b, mout_mu);
	ui::g_gui = &gui;
	gui.eng = nullptr;
	// Network MIDI endpoints only exist while the session is enabled, so apply
	// the stored switch before anything enumerates ports.
	ui::apply_stored_midi_setup();
	// The editors' file requests have answers here (document pickers), which is
	// what makes them offer buttons rather than the path box they fall back to
	// when a platform has no dialogs.
	ui::enable_file_dialogs();
	// Creates the audio objects now (needs no firmware); opening them waits for
	// boot below. Without this out stays null and start_audio refuses - which is
	// exactly the silence with no log line, since nothing ever tried.
	gui.make_audio();

	// The panel needs no audio to draw, so this boots the firmware first; the
	// audio device opens afterwards below, once the firmware is up.
	const std::string dir = rom_dir();
	std::fprintf(stderr, "[ios] ROM dir: %s\n", dir.c_str());

	static ui::engine eng(br, midi_ports[0]);
	gui.eng = &eng;
	gui.state = &eng.state;

	// load_machine wants the parsed tool_args because it reads a.dir and a.usb_host
	// off them. Rather than run the argv parser - there is no command line on iOS -
	// this fills in the fields it uses and leaves the rest default.
	//
	// Shared, not a local: the ROM-import callback below reads it after the user
	// has picked a folder, long after this method has returned, and a local would
	// be a dangling reference by then - on the path every first launch takes,
	// since a new install has no ROMs and so always goes through the picker. A
	// shared_ptr captured by value in the block ties the lifetime to the callback,
	// which is the only thing that needs it alive.
	auto a = std::make_shared<ui::tool_args>();
	a->dir = dir;
	// Without --layout, look through the usual places in order: exactly what the
	// shared parser does in tool_args.h. Skipping this was why art/real was never
	// used - apply_layout("") falls back to the built-in defaults and never
	// consults find_default(), so the bundled art/real/panel.txt sat there
	// unread while the panel drew from defaults. The log names the file so the
	// next run proves which layout won rather than leaving it to inspection.
	if (a->layout_path.empty())
		a->layout_path = ui::layout::find_default();
	std::fprintf(stderr, "[ios] layout: %s\n",
	             a->layout_path.empty() ? "(built-in defaults)" : a->layout_path.c_str());
	// Boot now, and again later if this launch had nothing to boot: the picker
	// opens at the end of this method and boot_machine() runs once the install
	// lands, so the machine comes up without a relaunch.
	gui.initialize_audio_settings(*a, {});
	const bool booted = boot_machine(gui, eng, *a);

	// What ui::app::run() does for every desktop main before showing the window:
	// LCD/panel sizing, the layout file (art/real included), remembered volume.
	// iOS never calls run() - it cannot, run() pumps its own event loop - and this
	// call was missing, so the layout path found above was never applied and the
	// panel kept its built-in defaults. That was the whole "no art/real on
	// screen": find_default() succeeding looks like success, but without
	// apply_layout nothing reads a single PNG. setup_for_window opens no audio
	// device (that is open_remembered_ports/make_audio, called later in run()),
	// so it is safe this early.
	ui::window_options win_opts;
	gui.setup_for_window(*a, win_opts, false);

	// The window system: a UIView with a CAMetalLayer and a 30 Hz CADisplayLink.
	const CGRect b = [UIScreen mainScreen].bounds;
	UIView *panel_view = ui::make_ios_view(gui, (int)b.size.width, (int)b.size.height);
	if (!panel_view) {
		std::fprintf(stderr, "[ios] no view: no Metal device\n");
		return;
	}
	// The view has to be in the hierarchy or it draws nothing, and nothing says so.
	// As a pinned subview, not as the root view: a root view assigned by hand
	// keeps the fixed frame it was created with (UIScreen bounds at launch), so in
	// a smaller Stage Manager window it overflowed right and bottom while the
	// scale was computed for fullscreen. Anchors make the view track the window on
	// rotation and resize, and layoutSubviews refits the panel from the real size
	// every time.
	panel_view.translatesAutoresizingMaskIntoConstraints = NO;
	[vc.view addSubview:panel_view];
	// Pinned to the safe area on all four sides, not the view edges: the panel's
	// top strip (List/Editor/... buttons) went under the iPad menu bar and the
	// status area with edge pins, making the editor-launching buttons visible
	// but untappable. The bars this leaves are UIKit's problem (letterbox), and
	// every control stays reachable - which is the whole point of a panel.
	UILayoutGuide *safe = vc.view.safeAreaLayoutGuide;
	[NSLayoutConstraint activateConstraints:@[
		[panel_view.leadingAnchor constraintEqualToAnchor:safe.leadingAnchor],
		[panel_view.trailingAnchor constraintEqualToAnchor:safe.trailingAnchor],
		[panel_view.topAnchor constraintEqualToAnchor:safe.topAnchor],
		[panel_view.bottomAnchor constraintEqualToAnchor:safe.bottomAnchor],
	]];

// Touch works now (one finger = mouse, held second finger = right button), so
// the panel can be operated; keyboard and audio are still missing.
	std::fprintf(stderr, "[ios] running: tap to press, hold a second finger for menus\n");

	// Nothing to boot means nothing to play, so ask for the images rather than
	// showing a dead panel: the picker opens on this launch, and installing
	// boots the machine on the spot (no relaunch). Deferred one turn so the
	// scene is fully connected before anything is presented - presenting during
	// willConnectToSession is refused, silently.
	if (!booted) {
		ui::gui_app *g = &gui;
		ui::engine *e = &eng;
		set_rom_import_done([g, e, a] {
			// The container now holds a whole set, so the shared search finds
			// it on its own; refresh the path the boot will use.
			a->dir = rom_dir();
			boot_machine(*g, *e, *a);
		});
		dispatch_async(dispatch_get_main_queue(), ^{
			prompt_for_roms(panel_view);
		});
	}
}

// UIKit turns the idle timer back on when the app is sent to the background,
// so the engine being up is not enough on its own.
- (void)sceneDidEnterBackground:(UIScene *)scene
{
	UIApplication.sharedApplication.idleTimerDisabled = NO;
}

// Coming back with the machine still up means the screen stays on; if the
// engine never booted (no ROMs) or failed, the device can sleep as it likes.
//
// And the machine is checked here, because an interruption that ends without
// "should resume" leaves AVAudioEngine stopped and nothing to restart it: the
// session's own observer ignores that case on purpose (a call the system will
// not resume is not one to restart for), and the panel freezes with it - it is
// still up, still state 1, but pump_realtime() has stood down now that the
// device has produced, so nothing advances the machine until a relaunch.
// do_restart() is the shared power-cycle path: it drops the state, waits for
// in_fill and re-boots on its own thread, which is the discipline this needs and
// boot_machine() from the main thread would not have.
//
// Off the idle timer either way: this runs whether or not the machine is up, and
// keep_screen_awake() is the last word on it.
- (void)sceneWillEnterForeground:(UIScene *)scene
{
	if (ui::g_gui && s_machine_up)
		ui::g_gui->do_restart();
	keep_screen_awake();
}

@end

int main(int argc, char *argv[])
{
	@autoreleasepool {
		// nil for both: the app delegate must be absent on this iOS, and the scene
		// delegate comes from UISceneDelegateClassName in Info.plist. Passing the
		// scene delegate here instead is what produced NoSceneLifecycleAdoption.
		return UIApplicationMain(argc, argv, nil, nil);
	}
}