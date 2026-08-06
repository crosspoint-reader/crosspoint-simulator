#pragma once

// iOS-native preferences, read from the Settings app (Settings.bundle).
//
// SEPARATE FROM THE FIRMWARE'S SETTINGS ON PURPOSE. Everything else the owner
// can configure lives in settings.json and is edited on the emulated e-ink
// panel, because it is the reader's own state and travels with the SD card.
// These are not: they are properties of THIS PHONE running the app, they mean
// nothing on device hardware, and the natural place to look for them is
// Settings > CrossPoint X3.
//
// The firmware's own "Keep Screen Awake" row still exists and is untouched;
// which of the two wins on iOS is an open decision, see the note in
// simulator_main.cpp's applyKeepScreenAwake().

#ifdef __cplusplus
extern "C" {
#endif

// Should the host display be held awake right now?
//
// 1 = keep awake, 0 = let iOS dim and lock normally. Answers for the CURRENT
// power state: the owner sets sleep behaviour separately for battery and for
// charging, because reading propped on a charger and reading in bed are
// different situations and the same answer does not serve both.
//
// Safe to call every frame and from a non-Objective-C translation unit. Main
// thread only — it touches UIDevice.
int CrossPointPrefs_wantsScreenAwake(void);

// How far the button pad's outline / pressed wash sits from the field behind it.
//
// One signed scale, -9..9, clamped. 0 means the tone EQUALS the field, so the
// control draws nothing; negative is darker than the field, positive is
// lighter, and +/-9 is the end of the gamut. The two tones are set separately
// because the stroke covers a line and the wash covers a whole cell, and the
// answer is stored per appearance because light and dark have opposite
// headroom. `dark` is 1 for the dark appearance, 0 for light.
//
// The level indexes the delta tables in CrossPointIOSShim.cpp; the mapping from
// level to a measured contrast ratio lives there and in Root.plist's row
// labels, not here.
//
// Safe to call every frame. Main thread only.
int CrossPointPrefs_padOutlineContrast(int dark);
int CrossPointPrefs_padFillContrast(int dark);

#ifdef __cplusplus
}
#endif
