#include "CrossPointPrefs.h"

#import <UIKit/UIKit.h>

// Keys must match ios/Settings.bundle/Root.plist. A typo is silent:
// -boolForKey: on a missing key returns NO, which here would read as "never
// allow sleep" and hold the screen on forever. checkKnown() below turns that
// into a log line instead of a mystery.
static NSString *const kAllowSleepOnBattery = @"allowSleepOnBattery";
static NSString *const kAllowSleepWhileCharging = @"allowSleepWhileCharging";

// Same trap, worse failure: -integerForKey: on a missing key returns 0, which on
// this scale means "tone equals the field", i.e. an invisible outline AND an
// invisible press — a pad that shows nothing at all.
static NSString *const kPadOutlineContrastLight = @"padOutlineContrastLight";
static NSString *const kPadOutlineContrastDark = @"padOutlineContrastDark";
static NSString *const kPadFillContrastLight = @"padFillContrastLight";
static NSString *const kPadFillContrastDark = @"padFillContrastDark";

// THE DEFAULTS LIVE IN Root.plist AND NOWHERE ELSE.
//
// A Settings.bundle needs its defaults stated twice by construction, and the
// two halves do different jobs, so neither can simply be deleted:
//
//   DefaultValue in Root.plist   what Settings.app DISPLAYS for a key the owner
//                                has never touched. It does not write anything.
//   registerDefaults             what -boolForKey: RETURNS for that same
//                                untouched key. Settings.app cannot see it.
//
// Omit the first and the switch reads OFF while the app behaves as ON. Omit the
// second and every untouched key answers NO — here, "never allow sleep". So the
// duplication is unavoidable; what is avoidable is the DRIFT. This reads the
// shipped Root.plist and builds the registration domain out of its own
// DefaultValues, so there is one place to edit and adding a switch needs no
// code change at all.
static NSDictionary *defaultsFromSettingsBundle(void) {
  NSString *path = [[NSBundle mainBundle] pathForResource:@"Root"
                                                   ofType:@"plist"
                                              inDirectory:@"Settings.bundle"];
  NSDictionary *root = path ? [NSDictionary dictionaryWithContentsOfFile:path] : nil;
  NSArray *specifiers = root[@"PreferenceSpecifiers"];
  if (![specifiers isKindOfClass:NSArray.class]) return nil;

  NSMutableDictionary *defaults = [NSMutableDictionary dictionary];
  for (NSDictionary *spec in specifiers) {
    if (![spec isKindOfClass:NSDictionary.class]) continue;
    id key = spec[@"Key"];              // group specifiers have neither
    id value = spec[@"DefaultValue"];
    if ([key isKindOfClass:NSString.class] && value != nil) defaults[key] = value;
  }
  return defaults.count ? defaults : nil;
}

static void ensureDefaults(void) {
  static dispatch_once_t once;
  dispatch_once(&once, ^{
    NSDictionary *fromBundle = defaultsFromSettingsBundle();
    if (fromBundle) {
      [[NSUserDefaults standardUserDefaults] registerDefaults:fromBundle];
    } else {
      // Root.plist missing or unreadable — a packaging fault, not a
      // configuration. Fall back to letting the phone sleep, which is the
      // do-no-harm answer: the alternative failure mode holds a stranger's
      // screen awake indefinitely on battery. The pad levels fall back to the
      // shipped tones for the same reason — the alternative is 0, an invisible
      // pad, and an unlabelled control that draws nothing is not recoverable
      // from inside the app.
      NSLog(@"[CrossPoint] Settings.bundle/Root.plist unreadable; defaulting to allow-sleep");
      [[NSUserDefaults standardUserDefaults] registerDefaults:@{
        kAllowSleepOnBattery : @YES,
        kAllowSleepWhileCharging : @YES,
        kPadOutlineContrastLight : @(-1),
        kPadOutlineContrastDark : @(1),
        kPadFillContrastLight : @(-1),
        kPadFillContrastDark : @(1),
      }];
    }

    // Off by default; without it batteryState is always UIDeviceBatteryStateUnknown.
    [UIDevice currentDevice].batteryMonitoringEnabled = YES;
  });
}

// Catches the one drift the derivation above cannot: a key renamed in the plist
// but not here, or vice versa. Logged once per key rather than asserted — a
// wrong sleep setting is not worth killing a reader mid-page over.
static void checkKnown(NSString *key) {
#if !defined(NDEBUG) || defined(ENABLE_SERIAL_LOG)
  static NSMutableSet *warned;
  static dispatch_once_t once;
  dispatch_once(&once, ^{ warned = [NSMutableSet set]; });
  if ([[NSUserDefaults standardUserDefaults] objectForKey:key] == nil && ![warned containsObject:key]) {
    [warned addObject:key];
    NSLog(@"[CrossPoint] pref key '%@' is not in Settings.bundle/Root.plist", key);
  }
#else
  (void)key;
#endif
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

  NSString *key = charging ? kAllowSleepWhileCharging : kAllowSleepOnBattery;
  checkKnown(key);

  // Read live rather than cached. NSUserDefaults is an in-memory store, so this
  // is a dictionary lookup, and reading it here means a change made in
  // Settings.app while the app was backgrounded takes effect on the first frame
  // after returning — no notification observer, nothing to forget to unregister.
  const BOOL allowSleep = [[NSUserDefaults standardUserDefaults] boolForKey:key];
  return allowSleep ? 0 : 1;
}

// Read live, same as above and for the same reason: NSUserDefaults is an
// in-memory store, so this is a dictionary lookup, and a level changed in
// Settings.app while the app was backgrounded lands on the first frame after it
// returns. No observer, nothing to unregister.
static int padContrast(NSString *key) {
  ensureDefaults();
  checkKnown(key);
  NSInteger level = [[NSUserDefaults standardUserDefaults] integerForKey:key];
  // Clamp rather than trust: the value comes from a plist a user can edit
  // through a jailbroken Settings or a restored backup, and the caller indexes
  // a 19-entry table with it.
  if (level < -9) level = -9;
  if (level > 9) level = 9;
  return static_cast<int>(level);
}

int CrossPointPrefs_padOutlineContrast(int dark) {
  return padContrast(dark ? kPadOutlineContrastDark : kPadOutlineContrastLight);
}

int CrossPointPrefs_padFillContrast(int dark) {
  return padContrast(dark ? kPadFillContrastDark : kPadFillContrastLight);
}
