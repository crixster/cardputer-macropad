// ============================================================================
// hid_ble.cpp -- BLE HID backend (only compiled when BUILD_BLE is defined).
// MUST NOT include M5Cardputer.h (its KEY_* macros collide with BleKeyboard.h).
// ============================================================================
#if defined(BUILD_BLE)

#include <Arduino.h>
#include "hid_backend.h"
#include <BleKeyboard.h>

static BleKeyboard bleKeyboard("Cardputer MacroPad", "DIY", 100);

static uint8_t mapKey(uint8_t c) {
  switch (c) {
    case MP_KEY_LEFT_CTRL:  return KEY_LEFT_CTRL;
    case MP_KEY_LEFT_SHIFT: return KEY_LEFT_SHIFT;
    case MP_KEY_LEFT_ALT:   return KEY_LEFT_ALT;
    case MP_KEY_LEFT_GUI:   return KEY_LEFT_GUI;
    case MP_KEY_RETURN:     return KEY_RETURN;
    case MP_KEY_ESC:        return KEY_ESC;
    case MP_KEY_TAB:        return KEY_TAB;
    case MP_KEY_DELETE:     return KEY_DELETE;
    default: break;
  }
  if (c >= MP_KEY_F1 && c <= MP_KEY_F12) {
    return (uint8_t)(KEY_F1 + (c - MP_KEY_F1));
  }
  return c;
}

void hidBegin()              { bleKeyboard.begin(); }
void hidPress(uint8_t code)  { bleKeyboard.press(mapKey(code)); }
void hidReleaseAll()         { bleKeyboard.releaseAll(); }
void hidPrint(const char *s) { bleKeyboard.print(s); }
void hidWriteEnter()         { bleKeyboard.write(KEY_RETURN); }
bool hidReady()              { return bleKeyboard.isConnected(); }

#endif // BUILD_BLE
