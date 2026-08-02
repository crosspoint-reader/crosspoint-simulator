#pragma once

// The iPhone harness: gesture recognition that sits ABOVE SDL and translates
// touches into the X3's seven button scancodes. See CrossPointIOSShim.cpp.
//
// Declared with plain C linkage so simulator_main.cpp can call into it without
// dragging the harness's internals into the shared build.

extern "C" {

// Point the process CWD at a writable directory. HalStorage prefixes paths with
// ./fs_, which is meaningless against an iOS bundle. Call before setup().
void CrossPointHarness_prepareFilesystem();

// Install the on-screen button pad and its event watch. Requires SDL to be
// initialised, so call after setup() (HalDisplay::begin does the SDL_Init).
//
// There is no per-frame pump: every control is down-on-touch and up-on-lift, so
// presses are driven entirely by touch events and carry their real duration.
void CrossPointHarness_begin();

// The harness's per-frame hook. Call once per frame from main(), on the main
// thread, next to presentIfNeeded(). Two jobs, both about keeping what is on
// the glass true:
//
//   1. Follow the system light/dark appearance, read live from UIKit rather
//      than from SDL's cache (CrossPointAppearance.h says why).
//   2. Re-ask for a present for a short window after the app returns to the
//      foreground, because iOS discards frames presented into that transition
//      and this app presents too rarely to produce another by itself.
//
// EDGE-TRIGGERED, both of them. The steady-state cost is one UIKit read, one
// integer compare and one timestamp compare; no present is requested unless
// something actually changed. That matters more here than in a normal app --
// the panel is an e-ink simulation whose presentation model assumes it presents
// rarely, so a per-frame present would be a real regression, not just waste.
//
// It reads no SDL events, so HalGPIO keeps sole ownership of the pump, and it
// holds no gesture state, so PadCore stays clock-free.
void CrossPointHarness_perFrame();
}
