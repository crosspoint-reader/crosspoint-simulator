// Unit tests for PadCore: the pure finger->button decision logic behind the
// iOS button pad. The contract under test is PURE PASSTHROUGH -- a finger down
// inside a slot presses exactly that button, the matching finger up releases
// it, and nothing else ever presses, releases, stretches, or delays a button.
// There is deliberately no clock anywhere in the API: a harness that cannot
// see time cannot invent time-based behavior (the old POWER tap-stretch).
//
// Build + run (no framework, no CMake):
//   c++ -std=c++17 -I../ios tests/pad_core_test.cpp ios/PadCore.cpp -o /tmp/pad_core_test && /tmp/pad_core_test

#include "PadCore.h"

#include <cstdio>
#include <vector>

static int failures = 0;
#define CHECK(cond)                                                        \
  do {                                                                     \
    if (!(cond)) {                                                         \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);          \
      failures++;                                                          \
    }                                                                      \
  } while (0)

using Action = PadCore::Action;

static bool same(const std::vector<Action> &got,
                 const std::vector<Action> &want) {
  if (got.size() != want.size()) return false;
  for (size_t i = 0; i < got.size(); i++)
    if (got[i].type != want[i].type || got[i].slot != want[i].slot)
      return false;
  return true;
}

int main() {
  // --- tap: down then up on the same slot, one press + one release ---------
  {
    PadCore core(7);
    auto a = core.fingerDown(/*slot=*/2, /*finger=*/11);
    CHECK(same(a, {{Action::Press, 2}}));
    auto b = core.fingerUp(11);
    CHECK(same(b, {{Action::Release, 2}}));
  }

  // --- a press is NEVER generated without a finger, a release never lingers -
  {
    PadCore core(7);
    core.fingerDown(4, 1);
    auto up = core.fingerUp(1);
    CHECK(same(up, {{Action::Release, 4}}));
    // Second up for the same finger: nothing left to release.
    CHECK(core.fingerUp(1).empty());
    // Up for a finger that never touched: nothing.
    CHECK(core.fingerUp(99).empty());
  }

  // --- hold: no API exists to time out a press; only the finger ends it -----
  {
    PadCore core(7);
    core.fingerDown(1, 5);
    // Arbitrarily many unrelated events later, slot 1 is still down and the
    // ONLY thing that releases it is finger 5 lifting.
    core.fingerDown(3, 6);
    core.fingerUp(6);
    CHECK(core.isDown(1));
    auto a = core.fingerUp(5);
    CHECK(same(a, {{Action::Release, 1}}));
  }

  // --- outside touches do nothing ------------------------------------------
  {
    PadCore core(7);
    CHECK(core.fingerDown(PadCore::kNoSlot, 3).empty());
    CHECK(core.fingerUp(3).empty());
  }

  // --- second finger on an already-held slot is ignored ---------------------
  {
    PadCore core(7);
    core.fingerDown(2, 1);
    CHECK(core.fingerDown(2, 9).empty());
    // The ignored finger lifting must not release the holder's press.
    CHECK(core.fingerUp(9).empty());
    CHECK(core.isDown(2));
    CHECK(same(core.fingerUp(1), {{Action::Release, 2}}));
  }

  // --- chords: independent fingers, independent slots ------------------------
  {
    PadCore core(7);
    core.fingerDown(0, 1);
    core.fingerDown(6, 2);
    CHECK(core.isDown(0) && core.isDown(6));
    CHECK(same(core.fingerUp(2), {{Action::Release, 6}}));
    CHECK(core.isDown(0));
    CHECK(same(core.fingerUp(1), {{Action::Release, 0}}));
  }

  // --- drag off a slot cancels that press only -------------------------------
  {
    PadCore core(7);
    core.fingerDown(3, 8);
    core.fingerDown(5, 9);
    auto a = core.fingerLeftSlot(8);
    CHECK(same(a, {{Action::Release, 3}}));
    CHECK(core.isDown(5));
    // The lifted-later finger produces nothing extra for the cancelled slot.
    CHECK(core.fingerUp(8).empty());
    CHECK(same(core.fingerUp(9), {{Action::Release, 5}}));
  }

  // --- reset (backgrounding / focus loss) releases everything ----------------
  {
    PadCore core(7);
    core.fingerDown(1, 1);
    core.fingerDown(2, 2);
    auto a = core.reset();
    CHECK(a.size() == 2);
    CHECK(!core.isDown(1) && !core.isDown(2));
    CHECK(core.reset().empty());  // idempotent
  }

  if (failures == 0) std::printf("pad_core_test: all tests passed\n");
  return failures == 0 ? 0 : 1;
}
