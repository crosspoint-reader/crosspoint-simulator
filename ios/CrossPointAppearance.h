#pragma once

// The LIVE system appearance, asked of UIKit directly.
//
// WHY THIS EXISTS. SDL does not observe the system appearance continuously; it
// caches one SDL_SystemTheme in the video device and refreshes it in exactly two
// places on iOS:
//
//   _deps/sdl3-src/src/video/uikit/SDL_uikitvideo.m:80           once, at video init
//   _deps/sdl3-src/src/video/uikit/SDL_uikitviewcontroller.m:146 traitCollectionDidChange:
//
// both reading [UIScreen mainScreen].traitCollection (SDL_uikitvideo.m:199).
// traitCollectionDidChange: is deprecated as of iOS 17; Apple's replacement is
// registerForTraitChanges:withHandler:, which SDL has not adopted. UIKit only
// delivers the deprecated callback as part of a view update pass, and an SDL app
// drawing straight through Metal never runs one of its own.
//
// WHAT WAS ACTUALLY MEASURED, on iOS 26.5 in the Simulator, rather than what the
// deprecation implies: the callback still fires, and SDL's cached theme was
// never observed disagreeing with UIKit -- the window, the window scene,
// currentTraitCollection and [UIScreen mainScreen] were sampled together at 1 Hz
// across repeated flips, including flips made while the app was backgrounded,
// and all five always agreed. So SDL is not returning a wrong answer on that OS.
//
// This probe is therefore defensive, and it is worth its (near-zero) keep for
// two reasons: it removes the dependency on a deprecated callback firing at all,
// and it is faster -- it applies the change within one ~1 kHz frame of the app
// resuming, where SDL's event arrived up to a second later. The SDL event path
// is kept alongside it; it costs nothing and it is the right mechanism if SDL
// ever adopts registerForTraitChanges.
//
// It is NOT what fixed the reported "only repaints after I touch it" symptom.
// That was a discarded frame, not a stale flag -- see repaintAfterForeground in
// CrossPointIOSShim.cpp.
//
// Implemented in CrossPointAppearance.mm -- Objective-C++ rather than plain
// Objective-C so the declaration can carry the extern "C" that the C++ harness
// links against. iOS-only; nothing outside ios/ references it, and ios/ is only
// added to the build under `if(IOS)`.

#ifdef __cplusplus
extern "C" {
#endif

// 1 = dark, 0 = light, -1 = unknown (UIKit has no window to ask yet, which
// happens before the first present). Tri-state on purpose: "unknown" must not
// collapse into "light", or a pre-window call would paint the wrong palette and
// then be latched as already-applied by the edge trigger.
//
// MAIN THREAD ONLY, like every other UIKit call in this repo.
int CrossPointAppearance_isDark(void);

#ifdef __cplusplus
}
#endif
