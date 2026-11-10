#pragma once
// ============================================================================
// hid_backend.h
// Backend-neutral HID interface. Includes NO HID library and NO M5 headers,
// so it is safe to include from the .ino and from both backend .cpp files.
// ============================================================================
#include <stdint.h>

#if defined(BUILD_BLE)
  #define HID_BACKEND_BLE
#else
  #define HID_BACKEND_USB
#endif

// Neutral key codes. These are constants (not macros) with a unique prefix,
// so they can never collide with M5Cardputer's or the HID libraries' KEY_*.
// Each backend translates them to its own library's codes.
constexpr uint8_t MP_KEY_LEFT_CTRL  = 0x80;
constexpr uint8_t MP_KEY_LEFT_SHIFT = 0x81;
constexpr uint8_t MP_KEY_LEFT_ALT   = 0x82;
constexpr uint8_t MP_KEY_LEFT_GUI   = 0x83;
constexpr uint8_t MP_KEY_RETURN     = 0xB0;
constexpr uint8_t MP_KEY_ESC        = 0xB1;
constexpr uint8_t MP_KEY_TAB        = 0xB3;
constexpr uint8_t MP_KEY_DELETE     = 0xD4;
constexpr uint8_t MP_KEY_F1         = 0xC2;   // F1..F12 are contiguous
constexpr uint8_t MP_KEY_F12        = 0xCD;
// Printable keys: pass the ASCII character directly.

void hidBegin();
void hidPress(uint8_t code);
void hidReleaseAll();
void hidPrint(const char *s);
void hidWriteEnter();
bool hidReady();
