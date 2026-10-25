#pragma once

#include <cstdint>

// Keep the small portion of the FreeInk BoardConfig surface used outside the
// device HAL available without pulling ESP32-only GPIO headers into the native
// build. Device and panel-controller selection are compile-time in the
// simulator so each production hardware revision can be exercised explicitly.
#define FREEINK_LOG_TRANSPORT_HWCDC 0
#define FREEINK_LOG_TRANSPORT_ROM_PRINTF 1
#define FREEINK_LOG_TRANSPORT FREEINK_LOG_TRANSPORT_HWCDC

#if (defined(SIMULATOR_DEVICE_X3) + defined(SIMULATOR_DEVICE_X4_PRO) +         \
     defined(SIMULATOR_DEVICE_X4_CLASSIC) + defined(SIMULATOR_DEVICE_STICKY) + \
     defined(SIMULATOR_DEVICE_PAPERMONO) +                                     \
     defined(SIMULATOR_DEVICE_METALIO_EINK4) +                                 \
     defined(SIMULATOR_DEVICE_EEGO_A4) + defined(SIMULATOR_DEVICE_PICCO)) > 1
#error "Select at most one simulated device"
#endif

#if defined(SIMULATOR_DISPLAY_UC8179) && defined(SIMULATOR_DISPLAY_UC8279)
#error "Select at most one simulated display controller"
#endif

#if defined(SIMULATOR_DEVICE_X3) && defined(SIMULATOR_DISPLAY_UC8179)
#error "Xteink X3 revisions use UC8253 or UC8279d, not UC8179"
#endif

#if (defined(SIMULATOR_DEVICE_STICKY) ||                                       \
     defined(SIMULATOR_DEVICE_PAPERMONO) ||                                    \
     defined(SIMULATOR_DEVICE_METALIO_EINK4)) &&                               \
    (defined(SIMULATOR_DISPLAY_UC8179) || defined(SIMULATOR_DISPLAY_UC8279))
#error "This device uses SSD1677; do not select an Xteink controller override"
#endif

#if defined(SIMULATOR_DEVICE_EEGO_A4) &&                                       \
    (defined(SIMULATOR_DISPLAY_UC8179) || defined(SIMULATOR_DISPLAY_UC8279))
#error "EEGO A4 uses UC8279C; do not select an Xteink controller override"
#endif

#if defined(SIMULATOR_EEGO_NO_FRONTLIGHT) && !defined(SIMULATOR_DEVICE_EEGO_A4)
#error "The lightless variant requires EEGO A4"
#endif

#if defined(SIMULATOR_DEVICE_PICCO) &&                                         \
    (defined(SIMULATOR_DISPLAY_UC8179) || defined(SIMULATOR_DISPLAY_UC8279))
#error                                                                         \
    "Picco uses its own SSD1677/SSD2677 panel probe; do not select an Xteink controller override"
#endif

#undef FREEINK_DEVICE_X4
#undef FREEINK_DEVICE_X3
#undef FREEINK_DEVICE_X4PRO
#undef FREEINK_DEVICE_X4CLASSIC
#undef FREEINK_DEVICE_STICKY
#undef FREEINK_DEVICE_PAPERMONO
#undef FREEINK_DEVICE_METALIO_EINK4
#undef FREEINK_DEVICE_EEGO_A4
#undef FREEINK_DEVICE_PICCO

#if defined(SIMULATOR_DEVICE_PICCO)
#define FREEINK_DEVICE_PICCO 1
#else
#define FREEINK_DEVICE_PICCO 0
#endif

#if defined(SIMULATOR_DEVICE_EEGO_A4)
#define FREEINK_DEVICE_EEGO_A4 1
#else
#define FREEINK_DEVICE_EEGO_A4 0
#endif

#if defined(SIMULATOR_DEVICE_METALIO_EINK4)
#define FREEINK_DEVICE_METALIO_EINK4 1
#define FREEINK_CAP_HAPTIC 1
#else
#define FREEINK_DEVICE_METALIO_EINK4 0
#define FREEINK_CAP_HAPTIC 0
#endif

#if defined(SIMULATOR_DEVICE_PICCO)
#define FREEINK_DEVICE_X4 0
#define FREEINK_DEVICE_X3 0
#define FREEINK_DEVICE_X4PRO 0
#define FREEINK_DEVICE_X4CLASSIC 0
#define FREEINK_DEVICE_STICKY 0
#define FREEINK_DEVICE_PAPERMONO 0
#define FREEINK_CAP_TOUCH 1
#define FREEINK_CAP_FRONTLIGHT 1
#elif defined(SIMULATOR_DEVICE_EEGO_A4)
#define FREEINK_DEVICE_X4 0
#define FREEINK_DEVICE_X3 0
#define FREEINK_DEVICE_X4PRO 0
#define FREEINK_DEVICE_X4CLASSIC 0
#define FREEINK_DEVICE_STICKY 0
#define FREEINK_DEVICE_PAPERMONO 0
#define FREEINK_CAP_TOUCH 1
#define FREEINK_CAP_FRONTLIGHT 1
#elif defined(SIMULATOR_DEVICE_METALIO_EINK4)
#define FREEINK_DEVICE_X4 0
#define FREEINK_DEVICE_X3 0
#define FREEINK_DEVICE_X4PRO 0
#define FREEINK_DEVICE_X4CLASSIC 0
#define FREEINK_DEVICE_STICKY 0
#define FREEINK_DEVICE_PAPERMONO 0
#define FREEINK_CAP_TOUCH 1
#define FREEINK_CAP_FRONTLIGHT 0
#elif defined(SIMULATOR_DEVICE_PAPERMONO)
#define FREEINK_DEVICE_X4 0
#define FREEINK_DEVICE_X3 0
#define FREEINK_DEVICE_X4PRO 0
#define FREEINK_DEVICE_X4CLASSIC 0
#define FREEINK_DEVICE_STICKY 0
#define FREEINK_DEVICE_PAPERMONO 1
#define FREEINK_CAP_TOUCH 1
#define FREEINK_CAP_FRONTLIGHT 1
#elif defined(SIMULATOR_DEVICE_STICKY)
#define FREEINK_DEVICE_X4 0
#define FREEINK_DEVICE_X3 0
#define FREEINK_DEVICE_X4PRO 0
#define FREEINK_DEVICE_X4CLASSIC 0
#define FREEINK_DEVICE_STICKY 1
#define FREEINK_DEVICE_PAPERMONO 0
#define FREEINK_CAP_TOUCH 1
#define FREEINK_CAP_FRONTLIGHT 0
#elif defined(SIMULATOR_DEVICE_X4_PRO)
#define FREEINK_DEVICE_X4 0
#define FREEINK_DEVICE_X3 0
#define FREEINK_DEVICE_X4PRO 1
#define FREEINK_DEVICE_X4CLASSIC 0
#define FREEINK_DEVICE_STICKY 0
#define FREEINK_DEVICE_PAPERMONO 0
#define FREEINK_CAP_TOUCH 1
#define FREEINK_CAP_FRONTLIGHT 1
#elif defined(SIMULATOR_DEVICE_X4_CLASSIC)
#define FREEINK_DEVICE_X4 0
#define FREEINK_DEVICE_X3 0
#define FREEINK_DEVICE_X4PRO 0
#define FREEINK_DEVICE_X4CLASSIC 1
#define FREEINK_DEVICE_STICKY 0
#define FREEINK_DEVICE_PAPERMONO 0
#define FREEINK_CAP_TOUCH 0
#define FREEINK_CAP_FRONTLIGHT 0
#elif defined(SIMULATOR_DEVICE_X3)
#define FREEINK_DEVICE_X4 0
#define FREEINK_DEVICE_X3 1
#define FREEINK_DEVICE_X4PRO 0
#define FREEINK_DEVICE_X4CLASSIC 0
#define FREEINK_DEVICE_STICKY 0
#define FREEINK_DEVICE_PAPERMONO 0
#define FREEINK_CAP_TOUCH 0
#define FREEINK_CAP_FRONTLIGHT 0
#else
#define FREEINK_DEVICE_X4 1
#define FREEINK_DEVICE_X3 0
#define FREEINK_DEVICE_X4PRO 0
#define FREEINK_DEVICE_X4CLASSIC 0
#define FREEINK_DEVICE_STICKY 0
#define FREEINK_DEVICE_PAPERMONO 0
#define FREEINK_CAP_TOUCH 0
#define FREEINK_CAP_FRONTLIGHT 0
#endif

#define FREEINK_MCU_C3 (FREEINK_DEVICE_X3 || FREEINK_DEVICE_X4)
#define FREEINK_CAP_WARMLIGHT                                                  \
  (FREEINK_DEVICE_X4PRO || FREEINK_DEVICE_EEGO_A4 || FREEINK_DEVICE_PICCO)

namespace BoardConfig {

enum class Board {
  XteinkX4,
  XteinkX3,
  XteinkX3Uc8279,
  XteinkX4Pro,
  XteinkX4Classic,
  Sticky,
  PaperMono,
  MetalioEInk4,
  EegoA4,
  Picco,
};

enum class DisplayController {
  SSD1677 = 0,
  UC8253 = 2,
  ED2208 = 3,
  LgfxEpd = 4,
  IT8951 = 5,
  UC8279 = 6,
  UC8179 = 7,
  UC8279C = 8,
};

enum class TouchController : uint8_t {
  None,
  Chsc6x,
  Gt911,
  Ft5x06,
  Ft6336u,
  Gslx680,
  Cst816s,
  PiccoCst
};

struct TouchConfig {
  TouchController controller = TouchController::None;
};

struct ViewableInsets {
  uint8_t top = 9;
  uint8_t right = 3;
  uint8_t bottom = 3;
  uint8_t left = 3;
};

struct BoardProfile {
  Board board;
  const char *name;
  DisplayController displayController;
  uint8_t displayControllerVariant;
  struct {
    int8_t up;
    int8_t down;
    int8_t toggleSwitch = -1;
  } input;
  ViewableInsets viewableInsets = {};
  uint16_t displayWidth = 800;
  uint16_t displayHeight = 480;
  TouchConfig touch = {};
  int8_t headerOffsetY = 0;
  uint8_t cornerRadius = 0;
  uint8_t bottomCornerRadius = 0;
  int8_t statusBarOffsetY = 0;
  int8_t tabBarOffsetY = 0;
  uint8_t tabBarHeightPct = 100;
  uint8_t tabBarWidthPct = 100;
  uint8_t wifiMaxTxPower = 0;
};

#if defined(SIMULATOR_DISPLAY_UC8179)
inline constexpr DisplayController X4_DISPLAY_CONTROLLER =
    DisplayController::UC8179;
inline constexpr uint8_t X4_DISPLAY_CONTROLLER_VARIANT = 0x01;
#elif defined(SIMULATOR_DISPLAY_UC8279)
inline constexpr DisplayController X4_DISPLAY_CONTROLLER =
    DisplayController::UC8279;
// The SDK supports LUT_VER 0x02, 0x68, and reserved 0x69. Model the documented
// 0x68 production variant by default; callers can still inspect the controller.
inline constexpr uint8_t X4_DISPLAY_CONTROLLER_VARIANT = 0x68;
#else
inline constexpr DisplayController X4_DISPLAY_CONTROLLER =
    DisplayController::SSD1677;
inline constexpr uint8_t X4_DISPLAY_CONTROLLER_VARIANT = 0;
#endif

inline constexpr BoardProfile XTEINK_X4 = {Board::XteinkX4, "xteink_x4",
                                           X4_DISPLAY_CONTROLLER,
                                           X4_DISPLAY_CONTROLLER_VARIANT,
                                           {4, 5}};
inline constexpr BoardProfile XTEINK_X3 = {Board::XteinkX3, "xteink_x3",
                                           DisplayController::UC8253, 0, {4, 5}};
inline constexpr BoardProfile XTEINK_X3_UC8279 = {
    Board::XteinkX3Uc8279, "xteink_x3_uc8279", DisplayController::UC8279, 0,
    {4, 5}};
inline constexpr BoardProfile XTEINK_X4_PRO = {
    Board::XteinkX4Pro, "xteink_x4_pro", X4_DISPLAY_CONTROLLER,
    X4_DISPLAY_CONTROLLER_VARIANT, {0, 7}, {}, 800, 480,
    {TouchController::Gt911}};
inline constexpr BoardProfile XTEINK_X4_CLASSIC = {
    Board::XteinkX4Classic, "xteink_x4_classic", X4_DISPLAY_CONTROLLER,
    X4_DISPLAY_CONTROLLER_VARIANT, {0, 7}, {9, 7, 3, 7}};
inline constexpr BoardProfile STICKY = {
    Board::Sticky, "sticky", DisplayController::SSD1677, 0, {5, 6}, {}, 800,
    480, {TouchController::Gt911}};
inline constexpr BoardProfile PAPER_MONO = {
    Board::PaperMono, "m5stack_paper_mono", DisplayController::SSD1677, 0,
    {0, 7}, {9, 7, 3, 7}, 800, 480, {TouchController::Ft6336u}};

inline constexpr BoardProfile METALIO_EINK4 = {Board::MetalioEInk4,
                                               "metalio_eink4",
                                               DisplayController::SSD1677,
                                               0,
                                               {-1, -1},
                                               {},
                                               800,
                                               480,
                                               {TouchController::Cst816s}};

inline constexpr BoardProfile EEGO_A4 = {Board::EegoA4,
                                         "eego_a4",
                                         DisplayController::UC8279C,
                                         0,
                                         {5, 7},
                                         {24, 24, 8, 24},
                                         768,
                                         552,
                                         {TouchController::Gslx680},
                                         6,
                                         24,
                                         18,
                                         80,
                                         80,
                                         60};

inline constexpr BoardProfile PICCO = {
    Board::Picco, "picco", DisplayController::SSD1677, 0, {21, 9, 6}, {},
    800,          480,     {TouchController::PiccoCst}};

#if defined(SIMULATOR_DEVICE_PICCO)
inline BoardProfile ACTIVE = PICCO;
#elif defined(SIMULATOR_DEVICE_EEGO_A4)
inline BoardProfile ACTIVE = EEGO_A4;
#elif defined(SIMULATOR_DEVICE_METALIO_EINK4)
inline BoardProfile ACTIVE = METALIO_EINK4;
#elif defined(SIMULATOR_DEVICE_PAPERMONO)
inline BoardProfile ACTIVE = PAPER_MONO;
#elif defined(SIMULATOR_DEVICE_STICKY)
inline BoardProfile ACTIVE = STICKY;
#elif defined(SIMULATOR_DEVICE_X4_PRO)
inline BoardProfile ACTIVE = XTEINK_X4_PRO;
#elif defined(SIMULATOR_DEVICE_X4_CLASSIC)
inline BoardProfile ACTIVE = XTEINK_X4_CLASSIC;
#elif defined(SIMULATOR_DEVICE_X3)
#if defined(SIMULATOR_DISPLAY_UC8279)
inline BoardProfile ACTIVE = XTEINK_X3_UC8279;
#else
inline BoardProfile ACTIVE = XTEINK_X3;
#endif
#else
inline BoardProfile ACTIVE = XTEINK_X4;
#endif

inline bool selectDevice(Board board) {
  switch (board) {
  case Board::XteinkX4:
    ACTIVE = XTEINK_X4;
    return true;
  case Board::XteinkX3:
    ACTIVE = XTEINK_X3;
    return true;
  case Board::XteinkX3Uc8279:
    ACTIVE = XTEINK_X3_UC8279;
    return true;
  case Board::XteinkX4Pro:
    ACTIVE = XTEINK_X4_PRO;
    return true;
  case Board::XteinkX4Classic:
    ACTIVE = XTEINK_X4_CLASSIC;
    return true;
  case Board::Sticky:
    ACTIVE = STICKY;
    return true;
  case Board::MetalioEInk4:
    ACTIVE = METALIO_EINK4;
    return true;
  case Board::PaperMono:
    ACTIVE = PAPER_MONO;
    return true;
  case Board::EegoA4:
    ACTIVE = EEGO_A4;
    return true;
  case Board::Picco:
    ACTIVE = PICCO;
    return true;
  }
  return false;
}

inline bool isX4Pro() { return ACTIVE.board == Board::XteinkX4Pro; }
inline bool isX4Classic() { return ACTIVE.board == Board::XteinkX4Classic; }
inline bool isSticky() { return ACTIVE.board == Board::Sticky; }
inline bool isPaperMono() { return ACTIVE.board == Board::PaperMono; }
inline bool isMetalioEInk4() { return ACTIVE.board == Board::MetalioEInk4; }
inline bool isEegoA4() { return ACTIVE.board == Board::EegoA4; }
inline bool isPicco() { return ACTIVE.board == Board::Picco; }
inline bool hasTouch() {
  return isX4Pro() || isSticky() || isPaperMono() || isMetalioEInk4() ||
         isEegoA4() || isPicco();
}
inline bool hasHomeKey() { return isX4Pro() || isMetalioEInk4() || isEegoA4(); }
inline bool hasPwmFrontlight() { return isX4Pro() || isPaperMono(); }
inline bool hasI2cFrontlight() { return isEegoA4() || isPicco(); }

inline void holdPowerRails() {}

} // namespace BoardConfig
