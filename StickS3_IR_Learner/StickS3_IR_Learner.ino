/*
  ============================================================================
  CardputerMacroPad.ino
  ============================================================================
  M5Stack Cardputer (v1.1) -> USB HID Macro Keyboard & Command Launcher
  Target host: Windows 11
  MCU: ESP32-S3 (Stamp-S3), 240x135 TFT, 56-key matrix keyboard

  OPERATIONAL SUMMARY
  --------------------
  - On boot the device enumerates as a native USB HID keyboard and sits in
    ACTIVE MODE. Every physical keypress is looked up in a fixed-size table
    of macro slots. If the key is not bound to a slot with a non-empty
    payload, NOTHING is sent over USB -- the device is silent for that key.
  - Pressing ESC (Fn + `) toggles between ACTIVE MODE and EDITOR MODE.
  - EDITOR MODE lets you walk through macro slots, change the execution
    engine (RAW_TEXT / WIN_RUN / HOTKEY_COMBO), and live-edit the Label and
    Payload strings using the Cardputer's own keyboard. Changes are written
    to NVS (via Preferences) as soon as you commit them with ENTER, so the
    configuration survives power loss and reflashing of this same sketch.

  SECURITY NOTE
  --------------
  RAW_TEXT payloads (e.g. passwords/snippets) are stored in plaintext in
  NVS flash, exactly as requested. Anyone with physical access to the
  device (or the ability to read its flash) can recover these strings.
  Do not store anything on this device you are not comfortable having
  exposed if the hardware is lost or stolen.

  LIBRARIES REQUIRED (install via Arduino Library Manager / Board Manager)
  --------------------------------------------------------------------------
    - M5Cardputer          (M5Stack)         -> pulls in M5GFX / M5Unified
    - USB                  (bundled with ESP32 Arduino core, S3/S2 only)
    - USBHIDKeyboard        (bundled with ESP32 Arduino core, S3/S2 only)
    - Preferences           (bundled with ESP32 Arduino core)

  ============================================================================
*/

#include <M5Cardputer.h>
#include <M5Canvas.h>
#include <USB.h>
#include <USBHIDKeyboard.h>
#include <Preferences.h>

// ---------------------------------------------------------------------------
// Hardware power line (Stamp-S3) -- REQUIRED per board bring-up requirements
// ---------------------------------------------------------------------------
#define STAMP_S3_POWER_HOLD_PIN 38

// ---------------------------------------------------------------------------
// Raw USB HID Usage IDs (Keyboard/Keypad page) used to read the Cardputer's
// non-printable / Fn-combo keys out of keysState().hid_keys.
// These are standard USB HID usage table values; adjust only if your
// installed M5Cardputer library version reports different raw codes.
// ---------------------------------------------------------------------------
#define HID_KEY_ESC    0x29
#define HID_KEY_UP     0x52
#define HID_KEY_DOWN   0x51
#define HID_KEY_LEFT   0x50
#define HID_KEY_RIGHT  0x4F

// ---------------------------------------------------------------------------
// Macro slot model
// ---------------------------------------------------------------------------
#define MAX_SLOTS     14
#define LABEL_LEN     13   // 12 visible chars + null terminator
#define PAYLOAD_LEN   49   // 48 visible chars + null terminator

enum MacroType : uint8_t {
  TYPE_RAW_TEXT     = 0,  // type payload verbatim
  TYPE_WIN_RUN      = 1,  // Win+R, type payload, Enter
  TYPE_HOTKEY_COMBO = 2   // payload parsed as "MOD+MOD+KEY", e.g. "WIN+L"
};

struct MacroSlot {
  bool    special;              // false: key is a plain ASCII char, read from
                                 //        status.word (e.g. '1', 'a').
                                 // true:  key is a raw HID usage code, read
                                 //        from status.hid_keys (arrows, etc.)
  uint8_t key;                  // ASCII char (as uint8_t) OR HID usage code
  uint8_t type;                 // MacroType
  char    label[LABEL_LEN];     // short human-readable name shown on screen
  char    payload[PAYLOAD_LEN]; // text / command / combo descriptor
};

// Slots 0-9 are bound to the top number row '1'..'9','0' -- easy to reach,
// never collide with normal typing in ACTIVE MODE because ACTIVE MODE
// intentionally eats every keystroke and only forwards mapped ones.
// Slots 10-13 are bound to the Fn-combo arrow keys (Up/Left/Down/Right),
// which never appear in status.word -- they're read from status.hid_keys
// instead, hence "special = true" and a raw HID_KEY_* code rather than an
// ASCII char. ALL label/type/payload fields below are freely editable from
// EDITOR MODE and persist to NVS the moment you commit them with ENTER --
// only the trigger key itself (the ASCII char or HID_KEY_* code) is fixed
// in firmware for this build, since Editor Mode reserves the arrow keys
// for its own list/field navigation.
MacroSlot macros[MAX_SLOTS] = {
  { false, '1',         TYPE_RAW_TEXT,     "User1",   "user1" },
  { false, '2',         TYPE_RAW_TEXT,     "Pass1",   "pass1" },
  { false, '3',         TYPE_RAW_TEXT,     "User2",   "user2" },
  { false, '4',         TYPE_RAW_TEXT,     "Pass2",   "pass2" },
  { false, '5',         TYPE_HOTKEY_COMBO, "Lock",    "WIN+L" },
  { false, '6',         TYPE_HOTKEY_COMBO, "TaskMgr", "CTRL+SHIFT+ESC" },
  { false, '7',         TYPE_RAW_TEXT,     "Email",   "user@example.com" },
  { false, '8',         TYPE_RAW_TEXT,     "",        "" },
  { false, '9',         TYPE_RAW_TEXT,     "",        "" },
  { false, '0',         TYPE_RAW_TEXT,     "",        "" },
  { true,  HID_KEY_UP,    TYPE_WIN_RUN, "Edge",  "msedge" },
  { true,  HID_KEY_LEFT,  TYPE_WIN_RUN, "Run A", "C:\\path\\to\\file.exe" },
  { true,  HID_KEY_DOWN,  TYPE_WIN_RUN, "Run B", "https://example.com" },
  { true,  HID_KEY_RIGHT, TYPE_WIN_RUN, "Run C", "notepad" },
};

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------
USBHIDKeyboard Keyboard;
Preferences    prefs;
M5Canvas       canvas(&M5Cardputer.Display);

enum AppMode { MODE_ACTIVE, MODE_EDITOR };
AppMode appMode = MODE_ACTIVE;

// Editor sub-state
enum EditorFocus { FOCUS_LIST, FOCUS_FIELD };
enum EditorField  { FIELD_TYPE = 0, FIELD_LABEL = 1, FIELD_PAYLOAD = 2, FIELD_COUNT = 3 };

EditorFocus editorFocus   = FOCUS_LIST;
int         selectedSlot  = 0;
int         selectedField = FIELD_TYPE;
bool        textEditing   = false;      // true while live-typing a field
char        editBuffer[PAYLOAD_LEN];    // scratch buffer for live text edit
size_t      editBufferLen = 0;
bool        uiDirty       = true;       // redraw flag

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------
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
const char *keyLabel(const MacroSlot &m, char buf[4]);

// ===========================================================================
// setup()
// ===========================================================================
void setup() {
  // Required Stamp-S3 power hold line -- must be asserted before anything
  // else draws meaningful current.
  pinMode(STAMP_S3_POWER_HOLD_PIN, OUTPUT);
  digitalWrite(STAMP_S3_POWER_HOLD_PIN, HIGH);

  auto cfg = M5.config();
  M5Cardputer.begin(cfg, true);
  M5Cardputer.Display.setRotation(1);
  M5Cardputer.Display.setBrightness(80);

  canvas.setColorDepth(8);
  canvas.createSprite(M5Cardputer.Display.width(), M5Cardputer.Display.height());
  canvas.setTextSize(1);

  // USB HID bring-up
  USB.begin();
  Keyboard.begin();

  prefs.begin("macrocfg", false);
  loadMacros();

  appMode = MODE_ACTIVE;
  uiDirty = true;
}

// ===========================================================================
// loop()
// ===========================================================================
void loop() {
  M5Cardputer.update();

  if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
    Keyboard_Class::KeysState status = M5Cardputer.Keyboard.keysState();

    if (hidKeyPresent(status, HID_KEY_ESC)) {
      // ESC (Fn + `) is reserved exclusively for the mode toggle and is
      // never passed through to either macro lookup or text editing.
      toggleMode();
    } else if (appMode == MODE_ACTIVE) {
      handleActiveModeInput(status);
    } else {
      handleEditorInput(status);
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

// ===========================================================================
// Mode toggle
// ===========================================================================
void toggleMode() {
  if (appMode == MODE_ACTIVE) {
    appMode = MODE_EDITOR;
    editorFocus   = FOCUS_LIST;
    textEditing   = false;
    selectedSlot  = 0;
    selectedField = FIELD_TYPE;
  } else {
    // Leaving the editor: make sure a half-finished text edit is discarded,
    // not silently committed.
    textEditing = false;
    appMode = MODE_ACTIVE;
  }
  uiDirty = true;
}

// ===========================================================================
// ACTIVE MODE: strict lookup-or-ignore key handling
// ===========================================================================
void handleActiveModeInput(const Keyboard_Class::KeysState &status) {
  // Any key (or combination) that is not bound to a slot with a non-empty
  // payload produces zero USB HID output -- this is the core "silent by
  // default" requirement. Two independent trigger sources are scanned:
  //   - status.word:     printable characters (number row, letters, etc.)
  //   - status.hid_keys: non-printable / Fn-combo keys (arrows, etc.)
  for (char c : status.word) {
    for (int i = 0; i < MAX_SLOTS; i++) {
      if (!macros[i].special && macros[i].key == (uint8_t)c && macros[i].payload[0] != '\0') {
        executeMacro(i);
        break;
      }
    }
  }
  for (uint8_t code : status.hid_keys) {
    for (int i = 0; i < MAX_SLOTS; i++) {
      if (macros[i].special && macros[i].key == code && macros[i].payload[0] != '\0') {
        executeMacro(i);
        break;
      }
    }
  }
}

// ===========================================================================
// Macro execution dispatch
// ===========================================================================
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
  Keyboard.print(payload);
}

void sendWinRun(const char *payload) {
  Keyboard.press(KEY_LEFT_GUI);
  Keyboard.press('r');
  delay(40);
  Keyboard.releaseAll();
  delay(120); // give the Run dialog time to appear before we start typing
  Keyboard.print(payload);
  delay(40);
  Keyboard.write(KEY_RETURN);
}

// Parses payload strings like "WIN+L", "CTRL+SHIFT+ESC", "ALT+F4" and
// presses all modifiers + the final key together, then releases everything.
void sendHotkeyCombo(const char *payload) {
  char buf[PAYLOAD_LEN];
  strncpy(buf, payload, sizeof(buf));
  buf[sizeof(buf) - 1] = '\0';

  char *token = strtok(buf, "+");
  uint8_t finalKey = 0;
  bool finalKeyIsChar = false;
  char finalChar = 0;

  while (token != nullptr) {
    // Trim leading/trailing spaces
    while (*token == ' ') token++;
    size_t len = strlen(token);
    while (len > 0 && token[len - 1] == ' ') { token[len - 1] = '\0'; len--; }

    if (strcasecmp(token, "WIN") == 0 || strcasecmp(token, "GUI") == 0) {
      Keyboard.press(KEY_LEFT_GUI);
    } else if (strcasecmp(token, "CTRL") == 0 || strcasecmp(token, "CONTROL") == 0) {
      Keyboard.press(KEY_LEFT_CTRL);
    } else if (strcasecmp(token, "SHIFT") == 0) {
      Keyboard.press(KEY_LEFT_SHIFT);
    } else if (strcasecmp(token, "ALT") == 0) {
      Keyboard.press(KEY_LEFT_ALT);
    } else if (strcasecmp(token, "ESC") == 0 || strcasecmp(token, "ESCAPE") == 0) {
      finalKey = KEY_ESC; finalKeyIsChar = false;
    } else if (strcasecmp(token, "TAB") == 0) {
      finalKey = KEY_TAB; finalKeyIsChar = false;
    } else if (strcasecmp(token, "DEL") == 0 || strcasecmp(token, "DELETE") == 0) {
      finalKey = KEY_DELETE; finalKeyIsChar = false;
    } else if (strcasecmp(token, "ENTER") == 0 || strcasecmp(token, "RETURN") == 0) {
      finalKey = KEY_RETURN; finalKeyIsChar = false;
    } else if (token[0] == 'F' && isdigit((unsigned char)token[1])) {
      int fnum = atoi(token + 1);
      if (fnum >= 1 && fnum <= 12) {
        finalKey = KEY_F1 + (fnum - 1);
        finalKeyIsChar = false;
      }
    } else if (len == 1) {
      // Single printable character, e.g. "L" in WIN+L
      finalChar = tolower((unsigned char)token[0]);
      finalKeyIsChar = true;
    }

    token = strtok(nullptr, "+");
  }

  if (finalKeyIsChar && finalChar != 0) {
    Keyboard.press(finalChar);
  } else if (finalKey != 0) {
    Keyboard.press(finalKey);
  }

  delay(50);
  Keyboard.releaseAll();
}

// ===========================================================================
// EDITOR MODE input handling
// ===========================================================================
void handleEditorInput(const Keyboard_Class::KeysState &status) {
  if (textEditing) {
    // --- Live text entry for Label or Payload ---
    if (status.del) {
      if (editBufferLen > 0) {
        editBufferLen--;
        editBuffer[editBufferLen] = '\0';
        uiDirty = true;
      }
      return;
    }
    if (status.enter) {
      // Commit to the in-RAM slot and persist to NVS
      char *dest = (selectedField == FIELD_LABEL) ? macros[selectedSlot].label
                                                    : macros[selectedSlot].payload;
      size_t destCap = (selectedField == FIELD_LABEL) ? LABEL_LEN : PAYLOAD_LEN;
      strncpy(dest, editBuffer, destCap - 1);
      dest[destCap - 1] = '\0';
      saveSlot(selectedSlot);
      textEditing = false;
      uiDirty = true;
      return;
    }
    // Append any typed printable characters (bounded by buffer capacity)
    size_t destCap = (selectedField == FIELD_LABEL) ? (LABEL_LEN - 1) : (PAYLOAD_LEN - 1);
    for (char c : status.word) {
      if (editBufferLen < destCap) {
        editBuffer[editBufferLen++] = c;
        editBuffer[editBufferLen] = '\0';
        uiDirty = true;
      }
    }
    return;
  }

  if (editorFocus == FOCUS_LIST) {
    // --- Navigating the slot list ---
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
    // --- Editing fields (Type / Label / Payload) within the selected slot ---
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
      } else {
        // Enter live text-edit mode, pre-loaded with the current value
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

// ===========================================================================
// NVS persistence
// ===========================================================================
void loadMacros() {
  char keyName[8];
  for (int i = 0; i < MAX_SLOTS; i++) {
    snprintf(keyName, sizeof(keyName), "t%d", i);
    if (prefs.isKey(keyName)) {
      macros[i].type = prefs.getUChar(keyName, macros[i].type);

      snprintf(keyName, sizeof(keyName), "k%d", i);
      macros[i].key = prefs.getUChar(keyName, macros[i].key);

      snprintf(keyName, sizeof(keyName), "s%d", i);
      macros[i].special = prefs.getBool(keyName, macros[i].special);

      snprintf(keyName, sizeof(keyName), "l%d", i);
      String lbl = prefs.getString(keyName, macros[i].label);
      strncpy(macros[i].label, lbl.c_str(), LABEL_LEN - 1);
      macros[i].label[LABEL_LEN - 1] = '\0';

      snprintf(keyName, sizeof(keyName), "p%d", i);
      String pl = prefs.getString(keyName, macros[i].payload);
      strncpy(macros[i].payload, pl.c_str(), PAYLOAD_LEN - 1);
      macros[i].payload[PAYLOAD_LEN - 1] = '\0';
    }
    // If no NVS entry exists yet for this slot, the compiled-in default
    // above remains in effect until the user edits and saves it.
  }
}

void saveSlot(int idx) {
  char keyName[8];
  snprintf(keyName, sizeof(keyName), "t%d", idx);
  prefs.putUChar(keyName, macros[idx].type);

  snprintf(keyName, sizeof(keyName), "k%d", idx);
  prefs.putUChar(keyName, macros[idx].key);

  snprintf(keyName, sizeof(keyName), "s%d", idx);
  prefs.putBool(keyName, macros[idx].special);

  snprintf(keyName, sizeof(keyName), "l%d", idx);
  prefs.putString(keyName, macros[idx].label);

  snprintf(keyName, sizeof(keyName), "p%d", idx);
  prefs.putString(keyName, macros[idx].payload);
}

// ===========================================================================
// UI rendering
// ===========================================================================
const char *typeName(uint8_t type) {
  switch (type) {
    case TYPE_RAW_TEXT:     return "RAW_TEXT";
    case TYPE_WIN_RUN:      return "WIN_RUN";
    case TYPE_HOTKEY_COMBO: return "HOTKEY";
    default:                return "?";
  }
}

// Renders a slot's trigger as a short printable string: the literal
// character for normal slots, or a 2-letter mnemonic for special
// (HID-code) slots like the arrow keys. buf must be at least 4 bytes.
const char *keyLabel(const MacroSlot &m, char buf[4]) {
  if (!m.special) {
    buf[0] = (char)m.key;
    buf[1] = '\0';
    return buf;
  }
  switch (m.key) {
    case HID_KEY_UP:    return "Up";
    case HID_KEY_DOWN:  return "Dn";
    case HID_KEY_LEFT:  return "Lt";
    case HID_KEY_RIGHT: return "Rt";
    default:            return "??";
  }
}

void drawActiveModeUI() {
  canvas.fillSprite(BLACK);

  canvas.fillRect(0, 0, 240, 14, NAVY);
  canvas.setTextColor(WHITE, NAVY);
  canvas.setCursor(4, 3);
  canvas.print("MACRO KEYBOARD  [ACTIVE]  ESC=Edit");

  canvas.setTextColor(GREENYELLOW, BLACK);
  int y = 18;
  int col = 0;
  char kbuf[4];
  for (int i = 0; i < MAX_SLOTS; i++) {
    int x = (col == 0) ? 4 : 124;
    canvas.setCursor(x, y);
    if (macros[i].payload[0] != '\0') {
      canvas.setTextColor(WHITE, BLACK);
      canvas.printf("[%2s] %-9.9s", keyLabel(macros[i], kbuf), macros[i].label);
    } else {
      canvas.setTextColor(DARKGREY, BLACK);
      canvas.printf("[%2s] --empty--", keyLabel(macros[i], kbuf));
    }
    col++;
    if (col == 2) { col = 0; y += 11; }
  }
}

void drawEditorUI() {
  canvas.fillSprite(BLACK);

  canvas.fillRect(0, 0, 240, 14, MAROON);
  canvas.setTextColor(WHITE, MAROON);
  canvas.setCursor(4, 3);
  canvas.print("EDITOR MODE   ESC=Exit");

  // Left column: slot list (13 slots at 8px rows fits the 117px list area)
  int y = 18;
  char kbuf[4];
  for (int i = 0; i < MAX_SLOTS; i++) {
    bool isSel = (editorFocus == FOCUS_LIST) && (i == selectedSlot);
    bool isOpen = (editorFocus == FOCUS_FIELD) && (i == selectedSlot);
    canvas.setCursor(4, y);
    if (isSel) {
      canvas.setTextColor(BLACK, YELLOW);
    } else if (isOpen) {
      canvas.setTextColor(BLACK, CYAN);
    } else {
      canvas.setTextColor(WHITE, BLACK);
    }
    canvas.printf("[%2s]%-8.8s", keyLabel(macros[i], kbuf),
                  macros[i].label[0] ? macros[i].label : "(empty)");
    y += 8;
  }

  // Right panel: fields for the open slot
  if (editorFocus == FOCUS_FIELD) {
    canvas.drawRect(120, 16, 118, 118, DARKGREY);
    int fy = 20;

    canvas.setTextColor(selectedField == FIELD_TYPE ? BLACK : WHITE,
                         selectedField == FIELD_TYPE ? ORANGE : BLACK);
    canvas.setCursor(124, fy);
    canvas.printf("Type: %s", typeName(macros[selectedSlot].type));
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
      canvas.print("ENTER=edit text");
    }
  } else {
    canvas.setTextColor(LIGHTGREY, BLACK);
    canvas.setCursor(124, 20);
    canvas.print("Up/Down: select");
    canvas.setCursor(124, 32);
    canvas.print("Enter/Right: open");
  }
}
