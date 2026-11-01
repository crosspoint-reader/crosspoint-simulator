#pragma once

// With CROSSPOINT_SIM_SCREEN_TEXT=1, each display refresh prints the text on
// screen as one "[SIM] SCREEN:" serial line, so scripts can check what a
// screen shows without reading screenshots. The build script patches
// GfxRenderer::drawText to report its strings (see run_simulator.py).

// Called by the patched GfxRenderer::drawText with logical coordinates.
void simulatorScreenText(int x, int y, const char *text);

namespace sim_screen_text {

// The framebuffer was cleared: forget the text drawn so far.
void clear();
// The framebuffer was shown: print its text if it changed.
void flush();

} // namespace sim_screen_text
