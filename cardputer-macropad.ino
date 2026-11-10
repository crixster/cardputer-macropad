/*
  ============================================================================
  cardputer-macropad.ino
  ============================================================================
  M5Stack Cardputer (v1.1) -> USB or BLE HID Macro Keyboard & Command Launcher
  Target host: Windows 11

  FILES (all must sit in the same sketch folder):
    cardputer-macropad.ino   this file (UI, macros, storage)
    hid_backend.h            neutral HID interface + MP_KEY_* constants
    hid_usb.cpp              USB backend (USBHIDKeyboard)
    hid_ble.cpp              BLE backend (ESP32 BLE Keyboard by T-vK)

  WHY THE HID CODE IS IN SEPARATE FILES
  M5Cardputer.h defines KEY_LEFT_CTRL, KEY_TAB, etc. as macros. BleKeyboard.h
  declares the same names as real constants, and the USB and BLE libraries
  also both define a "KeyReport" struct. Each .cpp file is its own
  compilation unit, so isolating each HID library in its own file (which
  never includes M5Cardputer.h) makes these conflicts impossible.
  This file therefore never uses library KEY_* names; it uses MP_KEY_*.

  USB vs BLE
  One backend per firmware build, chosen by the BUILD_BLE flag:
    not defined -> USB firmware
    -DBUILD_BLE -> BLE firmware
  The GitHub Actions workflow builds both. Flash the .bin you want.

  USAGE
  ACTIVE MODE (default): press a bound key to fire its macro. '0' = editor.
  EDITOR MODE:
    Slot list: Up/Down move, Right/Enter open slot, '0' exit editor.
    Fields:    Up/Down move (Key, Type, Label, Payload), Left = back,
               Enter on Type cycles RAW_TEXT -> WIN_RUN -> HOTKEY,
               Enter on Key/Label/Payload edits text (Enter saves,
               Del removes a char). '0' can be typed while text-editing.

  BLE pairing: Windows 11 -> Settings -> Bluetooth & devices -> Add device
  -> Bluetooth -> "Cardputer MacroPad". If stuck "not connecting", remove
  the device in Windows and pair again.

  MACRO TYPES
    RAW_TEXT      types the payload literally.
    WIN_RUN       Win+R, types the payload, presses Enter.
    HOTKEY_COMBO  '+'-separated, e.g. "CTRL+SHIFT+ESC", "WIN+L".
                  Modifiers: WIN/GUI, CTRL/CONTROL, SHIFT, ALT.
                  Final key: single char, ESC, TAB, ENTER, SPACE, DEL, F1-F12.

  LIBRARIES
    Always: M5Cardputer
    BLE build: "ESP32 BLE Keyboard" by T-vK (git install) and optionally
               NimBLE-Arduino (only used if USE_NIMBLE is defined).

  BOARD SETTINGS: USB Mode "USB-OTG (TinyUSB)", USB CDC On Boot "Enabled".
  For a local BLE build in the Arduino IDE, add -DBUILD_BLE via arduino-cli:
    --build-property "compiler.cpp.extra_flags=-DBUILD_BLE"
  ============================================================================
*/

#include <M5Cardputer.h>
#include <Preferences.h>
#include "hid_backend.h"

#define STAMP_S3_POWER_HOLD_PIN 38

// Raw HID usage codes for Cardputer arrow keys
#define HID_KEY_UP     0x52
#define HID_KEY_DOWN   0x51
#define HID_KEY_LEFT   0x50
#define HID_KEY_RIGHT  0x4F

#define MAX_SLOTS      13
#define LABEL_LEN      13
#define PAYLOAD_LEN    128   // Long enough for URLs and paths

#ifdef HID_BACKEND_BLE
bool bleWasConnected = false;  // for redraw-on-connect
#endif

enum MacroType : uint8_t {
  TYPE_RAW_TEXT     = 0,  // Types text directly (usernames/passwords)
  TYPE_WIN_RUN      = 1,  // Opens Win+R, types command/path, hits Enter
  TYPE_HOTKEY_COMBO = 2   // Executes hotkey combination (e.g., WIN+L)
};

struct MacroSlot {
  bool    special;
  uint8_t key;
  uint8_t type;
  char    label[LABEL_LEN];
  char    payload[PAYLOAD_LEN];
};

MacroSlot macros[MAX_SLOTS] = {
  { false, '1', TYPE_RAW_TEXT,     "User1",    "user1" },
  { false, '2', TYPE_RAW_TEXT,     "Pass1",    "pass1" },
  { false, '3', TYPE_RAW_TEXT,     "User2",    "user2" },
  { false, '4', TYPE_RAW_TEXT,     "Pass2",    "pass2" },
  { false, '5', TYPE_HOTKEY_COMBO, "Lock PC",  "WIN+L" },
  { false, '6', TYPE_HOTKEY_COMBO, "TaskMgr",  "CTRL+SHIFT+ESC" },
  { false, '7', TYPE_WIN_RUN,      "Notepad",  "notepad" },
  { false, '8', TYPE_WIN_RUN,      "Edge",     "msedge" },
  { false, '9', TYPE_WIN_RUN,      "Calc",     "calc" },
  { false, 'z', TYPE_WIN_RUN,      "BMC",      "https://continentalit-smartit.onbmc.com/smartit/app/#/ticket-console" },
  { false, 'x', TYPE_WIN_RUN,      "App 2",    "C:\\path\\to\\app2.exe" },
  { false, 'c', TYPE_WIN_RUN,      "Site A",   "https://example.com" },
  { false, 'v', TYPE_WIN_RUN,      "Site B",   "https://google.com" },
};

Preferences prefs;
M5Canvas    canvas(&M5Cardputer.Display);

enum AppMode { MODE_ACTIVE, MODE_EDITOR };
AppMode appMode = MODE_ACTIVE;

enum EditorFocus { FOCUS_LIST, FOCUS_FIELD };
enum EditorField { FIELD_TYPE = 0, FIELD_LABEL = 1, FIELD_PAYLOAD = 2, FIELD_KEY = 3, FIELD_COUNT = 4 };

EditorFocus editorFocus    = FOCUS_LIST;
int         selectedSlot   = 0;
int         selectedField  = FIELD_TYPE;
bool        textEditing    = false;
char        editBuffer[PAYLOAD_LEN];
size_t      editBufferLen  = 0;
bool        uiDirty        = true;

void loadMacros();
void saveSlot(int idx);
void drawActiveModeUI();
void drawEditorUI();
void handleActiveModeInput(const Keyboard_Class::KeysState &status);
void handleEditorInput(const Keyboard_Class::KeysState &status);
void executeMacro(int idx);
void sendRawText(const char *payload);
void sendWinRun(const char *payload);
void sendHotkeyCombo(const char *payload);
void toggleMode();
bool hidKeyPresent(const Keyboard_Class::KeysState &status, uint8_t code);
const char *typeName(uint8_t type);

void setup() {
  pinMode(STAMP_S3_POWER_HOLD_PIN, OUTPUT);
  digitalWrite(STAMP_S3_POWER_HOLD_PIN, HIGH);

  auto cfg = M5.config();
  M5Cardputer.begin(cfg, true);

  M5Cardputer.Display.setRotation(1);
  M5Cardputer.Display.setBrightness(80);

  canvas.setColorDepth(8);
  canvas.createSprite(M5Cardputer.Display.width(), M5Cardputer.Display.height());
  canvas.setTextSize(1);

  hidBegin();

  prefs.begin("macrocfg", false);
  loadMacros();

  appMode = MODE_ACTIVE;
  uiDirty = true;
}

void loop() {
  M5Cardputer.update();

#ifdef HID_BACKEND_BLE
  bool nowConnected = hidReady();
  if (nowConnected != bleWasConnected) {
    bleWasConnected = nowConnected;
    uiDirty = true;
  }
#endif

  if (M5Cardputer.Keyboard.isChange()) {
    if (M5Cardputer.Keyboard.isPressed()) {
      Keyboard_Class::KeysState status = M5Cardputer.Keyboard.keysState();

      bool hitZero = false;
      for (char c : status.word) {
        if (c == '0') { hitZero = true; break; }
      }

      // Don't let '0' toggle mode while actively typing into a field.
      if (hitZero && !(appMode == MODE_EDITOR && textEditing)) {
        toggleMode();
      } else if (appMode == MODE_ACTIVE) {
        handleActiveModeInput(status);
      } else {
        handleEditorInput(status);
      }
    }
  }

  if (uiDirty) {
    if (appMode == MODE_ACTIVE) {
      drawActiveModeUI();
    } else {
      drawEditorUI();
    }
    canvas.pushSprite(0, 0);
    uiDirty = false;
  }
}

void toggleMode() {
  if (appMode == MODE_ACTIVE) {
    appMode = MODE_EDITOR;
    editorFocus    = FOCUS_LIST;
    textEditing    = false;
    selectedSlot   = 0;
    selectedField  = FIELD_TYPE;
  } else {
    textEditing = false;
    appMode = MODE_ACTIVE;
  }
  uiDirty = true;
}

void handleActiveModeInput(const Keyboard_Class::KeysState &status) {
  if (!hidReady()) return; // BLE not paired yet -- ignore macro keys
  for (char c : status.word) {
    for (int i = 0; i < MAX_SLOTS; i++) {
      if (macros[i].key == (uint8_t)tolower((unsigned char)c) && macros[i].payload[0] != '\0') {
        executeMacro(i);
        break;
      }
    }
  }
}

void executeMacro(int idx) {
  MacroSlot &m = macros[idx];
  switch (m.type) {
    case TYPE_RAW_TEXT:     sendRawText(m.payload);     break;
    case TYPE_WIN_RUN:      sendWinRun(m.payload);      break;
    case TYPE_HOTKEY_COMBO: sendHotkeyCombo(m.payload); break;
    default: break;
  }
}

void sendRawText(const char *payload) {
  hidPrint(payload);
}

void sendWinRun(const char *payload) {
  hidPress(MP_KEY_LEFT_GUI);
  hidPress((uint8_t)'r');
  delay(40);
  hidReleaseAll();
  delay(120);
  hidPrint(payload);
  delay(40);
  hidWriteEnter();
}

// Modifiers plus ESC/TAB/ENTER/SPACE/DEL/F1-F12 are recognised as the final
// key, so combos like "CTRL+SHIFT+ESC" send the final key correctly.
void sendHotkeyCombo(const char *payload) {
  char buf[PAYLOAD_LEN];
  strncpy(buf, payload, sizeof(buf));
  buf[sizeof(buf) - 1] = '\0';

  char *token = strtok(buf, "+");
  bool haveFinal = false;
  uint8_t finalCode = 0;

  while (token != nullptr) {
    while (*token == ' ') token++;
    size_t len = strlen(token);
    while (len > 0 && token[len - 1] == ' ') { token[len - 1] = '\0'; len--; }

    if (strcasecmp(token, "WIN") == 0 || strcasecmp(token, "GUI") == 0) {
      hidPress(MP_KEY_LEFT_GUI);
    } else if (strcasecmp(token, "CTRL") == 0 || strcasecmp(token, "CONTROL") == 0) {
      hidPress(MP_KEY_LEFT_CTRL);
    } else if (strcasecmp(token, "SHIFT") == 0) {
      hidPress(MP_KEY_LEFT_SHIFT);
    } else if (strcasecmp(token, "ALT") == 0) {
      hidPress(MP_KEY_LEFT_ALT);
    } else if (strcasecmp(token, "ESC") == 0 || strcasecmp(token, "ESCAPE") == 0) {
      finalCode = MP_KEY_ESC;      haveFinal = true;
    } else if (strcasecmp(token, "TAB") == 0) {
      finalCode = MP_KEY_TAB;      haveFinal = true;
    } else if (strcasecmp(token, "ENTER") == 0 || strcasecmp(token, "RETURN") == 0) {
      finalCode = MP_KEY_RETURN;   haveFinal = true;
    } else if (strcasecmp(token, "SPACE") == 0) {
      finalCode = (uint8_t)' ';    haveFinal = true;
    } else if (strcasecmp(token, "DEL") == 0 || strcasecmp(token, "DELETE") == 0) {
      finalCode = MP_KEY_DELETE;   haveFinal = true;
    } else if ((token[0] == 'F' || token[0] == 'f') && len >= 2 && isdigit((unsigned char)token[1])) {
      int n = atoi(token + 1);
      if (n >= 1 && n <= 12) {
        finalCode = (uint8_t)(MP_KEY_F1 + (n - 1));
        haveFinal = true;
      }
    } else if (len == 1) {
      finalCode = (uint8_t)tolower((unsigned char)token[0]);
      haveFinal = true;
    }
    token = strtok(nullptr, "+");
  }

  if (haveFinal) {
    hidPress(finalCode);
  }

  delay(50);
  hidReleaseAll();
}

void handleEditorInput(const Keyboard_Class::KeysState &status) {
  if (textEditing) {
    if (status.del) {
      if (editBufferLen > 0) {
        editBufferLen--;
        editBuffer[editBufferLen] = '\0';
        uiDirty = true;
      }
      return;
    }
    if (status.enter) {
      if (selectedField == FIELD_KEY) {
        if (editBufferLen > 0) {
          macros[selectedSlot].key = (uint8_t)tolower((unsigned char)editBuffer[0]);
        }
      } else {
        char *dest = (selectedField == FIELD_LABEL) ? macros[selectedSlot].label
                                                     : macros[selectedSlot].payload;
        size_t destCap = (selectedField == FIELD_LABEL) ? LABEL_LEN : PAYLOAD_LEN;
        strncpy(dest, editBuffer, destCap - 1);
        dest[destCap - 1] = '\0';
      }
      saveSlot(selectedSlot);
      textEditing = false;
      uiDirty = true;
      return;
    }

    size_t destCap = (selectedField == FIELD_KEY)   ? 1
                    : (selectedField == FIELD_LABEL) ? (LABEL_LEN - 1)
                                                      : (PAYLOAD_LEN - 1);
    for (char c : status.word) {
      if (selectedField == FIELD_KEY) {
        editBuffer[0] = c;
        editBuffer[1] = '\0';
        editBufferLen = 1;
        uiDirty = true;
      } else if (editBufferLen < destCap) {
        editBuffer[editBufferLen++] = c;
        editBuffer[editBufferLen] = '\0';
        uiDirty = true;
      }
    }
    return;
  }

  if (editorFocus == FOCUS_LIST) {
    if (hidKeyPresent(status, HID_KEY_UP)) {
      selectedSlot = (selectedSlot - 1 + MAX_SLOTS) % MAX_SLOTS;
      uiDirty = true;
    } else if (hidKeyPresent(status, HID_KEY_DOWN)) {
      selectedSlot = (selectedSlot + 1) % MAX_SLOTS;
      uiDirty = true;
    } else if (status.enter || hidKeyPresent(status, HID_KEY_RIGHT)) {
      editorFocus = FOCUS_FIELD;
      selectedField = FIELD_TYPE;
      uiDirty = true;
    }
  } else {
    if (hidKeyPresent(status, HID_KEY_LEFT)) {
      editorFocus = FOCUS_LIST;
      uiDirty = true;
    } else if (hidKeyPresent(status, HID_KEY_UP)) {
      selectedField = (selectedField - 1 + FIELD_COUNT) % FIELD_COUNT;
      uiDirty = true;
    } else if (hidKeyPresent(status, HID_KEY_DOWN)) {
      selectedField = (selectedField + 1) % FIELD_COUNT;
      uiDirty = true;
    } else if (status.enter) {
      if (selectedField == FIELD_TYPE) {
        macros[selectedSlot].type = (macros[selectedSlot].type + 1) % 3;
        saveSlot(selectedSlot);
        uiDirty = true;
      } else if (selectedField == FIELD_KEY) {
        textEditing = true;
        editBuffer[0] = (char)macros[selectedSlot].key;
        editBuffer[1] = '\0';
        editBufferLen = 1;
        uiDirty = true;
      } else {
        textEditing = true;
        const char *src = (selectedField == FIELD_LABEL) ? macros[selectedSlot].label
                                                         : macros[selectedSlot].payload;
        strncpy(editBuffer, src, sizeof(editBuffer) - 1);
        editBuffer[sizeof(editBuffer) - 1] = '\0';
        editBufferLen = strlen(editBuffer);
        uiDirty = true;
      }
    }
  }
}

bool hidKeyPresent(const Keyboard_Class::KeysState &status, uint8_t code) {
  for (uint8_t k : status.hid_keys) {
    if (k == code) return true;
  }
  return false;
}

void loadMacros() {
  char keyName[8];
  for (int i = 0; i < MAX_SLOTS; i++) {
    snprintf(keyName, sizeof(keyName), "t%d", i);
    if (prefs.isKey(keyName)) {
      macros[i].type = prefs.getUChar(keyName, macros[i].type);

      snprintf(keyName, sizeof(keyName), "k%d", i);
      macros[i].key = prefs.getUChar(keyName, macros[i].key);

      snprintf(keyName, sizeof(keyName), "l%d", i);
      String lbl = prefs.getString(keyName, macros[i].label);
      strncpy(macros[i].label, lbl.c_str(), LABEL_LEN - 1);
      macros[i].label[LABEL_LEN - 1] = '\0';

      snprintf(keyName, sizeof(keyName), "p%d", i);
      String pl = prefs.getString(keyName, macros[i].payload);
      strncpy(macros[i].payload, pl.c_str(), PAYLOAD_LEN - 1);
      macros[i].payload[PAYLOAD_LEN - 1] = '\0';
    }
  }
}

void saveSlot(int idx) {
  char keyName[8];
  snprintf(keyName, sizeof(keyName), "t%d", idx);
  prefs.putUChar(keyName, macros[idx].type);

  snprintf(keyName, sizeof(keyName), "k%d", idx);
  prefs.putUChar(keyName, macros[idx].key);

  snprintf(keyName, sizeof(keyName), "l%d", idx);
  prefs.putString(keyName, macros[idx].label);

  snprintf(keyName, sizeof(keyName), "p%d", idx);
  prefs.putString(keyName, macros[idx].payload);
}

const char *typeName(uint8_t type) {
  switch (type) {
    case TYPE_RAW_TEXT:     return "RAW_TEXT";
    case TYPE_WIN_RUN:      return "WIN_RUN";
    case TYPE_HOTKEY_COMBO: return "HOTKEY";
    default:                return "?";
  }
}

void drawActiveModeUI() {
  canvas.fillSprite(BLACK);

  canvas.fillRect(0, 0, 240, 14, NAVY);
  canvas.setTextColor(WHITE, NAVY);
  canvas.setCursor(4, 3);
#ifdef HID_BACKEND_USB
  canvas.print("MACRO [ACTIVE] USB  0=Edit");
#else
  canvas.printf("MACRO [ACTIVE] BLE:%s 0=Edit", hidReady() ? "OK" : "...");
#endif

  canvas.setTextColor(GREENYELLOW, BLACK);
  int y = 18;
  int col = 0;
  for (int i = 0; i < MAX_SLOTS; i++) {
    int x = (col == 0) ? 4 : 124;
    canvas.setCursor(x, y);
    if (macros[i].payload[0] != '\0') {
      canvas.setTextColor(WHITE, BLACK);
      canvas.printf("[%c] %-10.10s", toupper(macros[i].key), macros[i].label);
    } else {
      canvas.setTextColor(DARKGREY, BLACK);
      canvas.printf("[%c] --empty--", toupper(macros[i].key));
    }
    col++;
    if (col == 2) { col = 0; y += 11; }
  }

#ifdef HID_BACKEND_BLE
  if (!hidReady()) {
    canvas.setTextColor(ORANGE, BLACK);
    canvas.setCursor(4, 122);
    canvas.print("Waiting for BLE pairing...");
  }
#endif
}

void drawEditorUI() {
  canvas.fillSprite(BLACK);

  canvas.fillRect(0, 0, 240, 14, MAROON);
  canvas.setTextColor(WHITE, MAROON);
  canvas.setCursor(4, 3);
  canvas.print("EDITOR [Arrows/Enter] 0=Exit");

  int y = 18;
  for (int i = 0; i < MAX_SLOTS; i++) {
    bool isSel  = (editorFocus == FOCUS_LIST)  && (i == selectedSlot);
    bool isOpen = (editorFocus == FOCUS_FIELD) && (i == selectedSlot);
    canvas.setCursor(4, y);
    if (isSel) {
      canvas.setTextColor(BLACK, YELLOW);
    } else if (isOpen) {
      canvas.setTextColor(BLACK, CYAN);
    } else {
      canvas.setTextColor(WHITE, BLACK);
    }
    canvas.printf("[%c]%-9.9s", toupper(macros[i].key),
                  macros[i].label[0] ? macros[i].label : "(empty)");
    y += 8;
  }

  if (editorFocus == FOCUS_FIELD) {
    canvas.drawRect(120, 16, 118, 118, DARKGREY);
    int fy = 20;

    canvas.setTextColor(selectedField == FIELD_KEY ? BLACK : WHITE,
                        selectedField == FIELD_KEY ? ORANGE : BLACK);
    canvas.setCursor(124, fy);
    if (textEditing && selectedField == FIELD_KEY) {
      canvas.printf("Key:%c_", editBuffer[0] ? toupper(editBuffer[0]) : ' ');
    } else {
      canvas.printf("Key:%c", toupper(macros[selectedSlot].key));
    }
    fy += 14;

    canvas.setTextColor(selectedField == FIELD_TYPE ? BLACK : WHITE,
                        selectedField == FIELD_TYPE ? ORANGE : BLACK);
    canvas.setCursor(124, fy);
    canvas.printf("Type:%s", typeName(macros[selectedSlot].type));
    fy += 14;

    canvas.setTextColor(selectedField == FIELD_LABEL ? BLACK : WHITE,
                        selectedField == FIELD_LABEL ? ORANGE : BLACK);
    canvas.setCursor(124, fy);
    if (textEditing && selectedField == FIELD_LABEL) {
      canvas.printf("Lbl:%.12s_", editBuffer);
    } else {
      canvas.printf("Lbl:%.12s", macros[selectedSlot].label);
    }
    fy += 14;

    canvas.setTextColor(selectedField == FIELD_PAYLOAD ? BLACK : WHITE,
                        selectedField == FIELD_PAYLOAD ? ORANGE : BLACK);
    canvas.setCursor(124, fy);
    if (textEditing && selectedField == FIELD_PAYLOAD) {
      canvas.printf("Pay:%.16s_", editBuffer);
    } else {
      canvas.printf("Pay:%.16s", macros[selectedSlot].payload);
    }
    fy += 18;

    canvas.setTextColor(LIGHTGREY, BLACK);
    canvas.setCursor(124, fy);
    if (textEditing) {
      canvas.print("Type.. ENTER=save");
    } else if (selectedField == FIELD_TYPE) {
      canvas.print("ENTER=cycle type");
    } else {
      canvas.print("ENTER=edit");
    }
  } else {
    canvas.setTextColor(LIGHTGREY, BLACK);
    canvas.setCursor(124, 20);
    canvas.print("Up/Dn: select");
    canvas.setCursor(124, 32);
    canvas.print("Right/Enter: open");
    canvas.setCursor(124, 48);
#ifdef HID_BACKEND_USB
    canvas.print("Build: USB");
#else
    canvas.print("Build: BLE");
#endif
  }
}
