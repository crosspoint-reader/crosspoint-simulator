// The live system appearance, asked of UIKit rather than read from SDL's
// cache. See CrossPointAppearance.h for what that buys and what it does not.

#include "CrossPointAppearance.h"

#import <UIKit/UIKit.h>

namespace {

int styleOf(UITraitCollection *tc) {
  if (!tc) return -1;
  switch (tc.userInterfaceStyle) {
    case UIUserInterfaceStyleDark:
      return 1;
    case UIUserInterfaceStyleLight:
      return 0;
    default:
      return -1;  // UIUserInterfaceStyleUnspecified
  }
}

// Cached because the caller runs at the main loop's ~1 kHz and the window never
// changes: this app has exactly one, created by SDL. Walking connectedScenes
// every frame would allocate a set enumerator per frame to rediscover the same
// pointer. __weak so a scene teardown cannot leave this dangling, and so a nil
// read simply re-resolves.
__weak UIWindow *g_window = nil;

UIWindow *resolveWindow() {
  UIWindow *cached = g_window;
  if (cached) return cached;
  UIWindow *fallback = nil;
  for (UIScene *scene in UIApplication.sharedApplication.connectedScenes) {
    if (![scene isKindOfClass:UIWindowScene.class]) continue;
    for (UIWindow *w in ((UIWindowScene *)scene).windows) {
      if (w.isKeyWindow) {
        g_window = w;
        return w;
      }
      if (!fallback) fallback = w;
    }
  }
  g_window = fallback;
  return fallback;
}

}  // namespace

// THE WINDOW IS THE SOURCE, not UITraitCollection.currentTraitCollection.
//
// currentTraitCollection is only contractual INSIDE a UIKit view-update
// callback -- UIKit sets it around calls like traitCollectionDidChange and
// layoutSubviews, and its value outside those is not promised. This is called
// from main()'s loop, which is nowhere near a view update, so the window's own
// traitCollection is the source and currentTraitCollection is kept only as a
// fallback for the startup frames, before SDL has created a window.
//
// MEASURED, on iOS 26.5 in the Simulator: the window, the window scene,
// currentTraitCollection and [UIScreen mainScreen] (the one SDL reads) were
// sampled together at 1 Hz across repeated light/dark flips, including flips
// made while the app was backgrounded. They never disagreed -- so this probe is
// not correcting a wrong answer from SDL on that OS, it is removing the
// DEPENDENCY on a deprecated callback firing at all. What it does buy, measured:
// the poll applies the change within one ~1 kHz frame of the app resuming, ~65
// ms, and consistently wins the race against SDL's event, which arrived up to a
// second later.
int CrossPointAppearance_isDark(void) {
  // Property getters can hand back autoreleased objects. main()'s loop is not a
  // UIKit callback, so there is no pool being drained around it -- at ~1 kHz an
  // unbounded pool would be a slow leak. Draining our own is a few nanoseconds.
  @autoreleasepool {
    const int fromWindow = styleOf(resolveWindow().traitCollection);
    if (fromWindow >= 0) return fromWindow;
    return styleOf(UITraitCollection.currentTraitCollection);
  }
}
