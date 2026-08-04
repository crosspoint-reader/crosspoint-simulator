#include "CrossPointPrefs.h"

#import <UIKit/UIKit.h>

// Keys must match ios/Settings.bundle/Root.plist exactly. A typo here is
// silent: -boolForKey: on a missing key returns NO, which would read as
// "never allow sleep" and hold the screen on forever.
static NSString *const kAllowSleepOnBattery = @"allowSleepOnBattery";
static NSString *const kAllowSleepWhileCharging = @"allowSleepWhileCharging";

// Both default YES — the app has always let the phone sleep normally, and a
// setting that changes behaviour the first time it is merely *added* would be a
// surprise. registerDefaults only fills keys the owner has never touched, so it
// cannot overwrite a choice, and it is what makes the Settings.bundle's own
// DefaultValue agree with what the code does before Settings is ever opened.
static void ensureDefaults(void) {
  static dispatch_once_t once;
  dispatch_once(&once, ^{
    [[NSUserDefaults standardUserDefaults] registerDefaults:@{
      kAllowSleepOnBattery : @YES,
      kAllowSleepWhileCharging : @YES,
    }];
    // Off by default; without it batteryState is always UIDeviceBatteryStateUnknown.
    [UIDevice currentDevice].batteryMonitoringEnabled = YES;
  });
}

int CrossPointPrefs_wantsScreenAwake(void) {
  ensureDefaults();

  const UIDeviceBatteryState state = [UIDevice currentDevice].batteryState;
  // Full counts as charging: the phone is on a cable, which is the thing the
  // owner is actually distinguishing. Unknown counts as battery — the
  // conservative side, since holding a phone awake on battery is the outcome
  // with a real cost.
  const BOOL charging =
      (state == UIDeviceBatteryStateCharging || state == UIDeviceBatteryStateFull);

  NSUserDefaults *d = [NSUserDefaults standardUserDefaults];
  // Read live rather than cached. NSUserDefaults is an in-memory store, so this
  // is a dictionary lookup, and reading it here means a change made in
  // Settings.app while the app was backgrounded takes effect on the first frame
  // after returning — no notification observer, nothing to forget to unregister.
  const BOOL allowSleep =
      charging ? [d boolForKey:kAllowSleepWhileCharging] : [d boolForKey:kAllowSleepOnBattery];

  return allowSleep ? 0 : 1;
}
