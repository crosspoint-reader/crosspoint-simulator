#pragma once

struct SDL_Renderer;

// Simulator-only chrome drawn outside the panel.
//
// Deliberately NOT a HalDisplay method: the HAL's public surface must mirror the
// firmware's, and an on-screen button pad has no analog on real hardware. This is
// a free hook the presentation path calls, so the HAL stays the shape the
// firmware expects.
//
// The callback runs with logical presentation DISABLED and receives the real
// output size, so it draws in device pixels and can paint the letterboxed
// margins that the panel's logical coordinate space cannot reach.
namespace SimulatorOverlay {

using DrawFn = void (*)(SDL_Renderer *renderer, int outWidthPx, int outHeightPx);

// Register (or clear, with nullptr) the overlay painter.
void setDrawCallback(DrawFn fn);

// The colour the presentation path clears to before the panel is drawn: the
// field the panel sits on. Defaults to white, which matches a blank e-ink page
// so the panel edge is invisible. On desktop the window is exactly panel-sized
// and the field never shows, so nothing there needs to call this; a host that
// letterboxes the panel (the phone presents it at 2x inside a taller screen)
// sets it to whatever its own appearance calls for.
void setClearColor(unsigned char r, unsigned char g, unsigned char b);

// Ask for a repaint. The firmware only presents when it has new panel content,
// which on an e-ink device is rare -- without this a button's pressed state
// would not appear until the next page render.
void requestPresent();

} // namespace SimulatorOverlay
