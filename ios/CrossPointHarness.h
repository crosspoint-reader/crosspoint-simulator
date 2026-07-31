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
}
