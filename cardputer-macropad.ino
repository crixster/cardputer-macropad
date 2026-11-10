/*
  ============================================================================
  CardputerMacroPad.ino
  ============================================================================
  M5Stack Cardputer (v1.1) -> USB HID Macro Keyboard & Command Launcher
  Target host: Windows 11
  MCU: ESP32-S3 (Stamp-S3), 240x135 TFT, 56-key matrix keyboard

  BOARD SETTINGS (Arduino IDE, required for USBHIDKeyboard to even compile):
    Tools -> USB Mode: "USB-OTG (TinyUSB)"
    Tools -> USB CDC On Boot: "Enabled"
  If these aren't set correctly for an ESP32-S3 target, USB.h / USBHIDKeyboard.h
  will fail to resolve and you'll get undeclared-identifier errors at compile time.
  ============================================================================
*/

#include <M5Cardputer.h>
#include <USB.h>
#include <USBHIDKeyboard.h>
#include <Preferences.h>

#define STAMP_S3_POWER_HOLD_PIN 38

// Raw HID usage codes for Cardputer arrow keys
#define HID_KEY_UP     0x52
#define HID_KEY_DOWN   0x51
#define HID_KEY_LEFT   0x50
#define HID_KEY_RIGHT  0x4F

#define MAX_SLOTS      13
#define LABEL_LEN      13
#define PAYLOAD_LEN    128   // Increased to support long URLs and paths

enum MacroType : uint8_t {
  TYPE_RAW_TEXT     = 0,  // Types text directly (for usernames/passwords)
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

USBHIDKeyboard Keyboard;
Preferences    prefs;
M5Canvas       canvas(&M5Cardputer.Display);

enum AppMode { MODE_ACTIVE, MODE_EDITOR };
AppMode appMode = MODE_ACTIVE;

enum EditorFocus { FOCUS_LIST, FOCUS_FIELD };
// Added FIELD_KEY so the trigger key itself can be rebound from the editor.
enum EditorField  { FIELD_TYPE = 0, FIELD_LABEL = 1, FIELD_PAYLOAD = 2, FIELD_KEY = 3, FIELD_COUNT = 4 };

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
const char *fieldName(int field);

void setup() {
  pinMode(STAMP_S3_POWER_HOLD_PIN, OUTPUT);
  digitalWrite(STAMP_S3_POWER_HOLD_PIN, HIGH);

  auto cfg = M5.config();
  M5Cardputer.begin(cfg, true);

  M5Cardputer.Display.setRotation(3);
  M5Cardputer.Display.setBrightness(80);

  canvas.setColorDepth(8);
  canvas.createSprite(M5Cardputer.Display.width(), M5Cardputer.Display.height());
  canvas.setTextSize(1);

  USB.begin();
  Keyboard.begin();

  prefs.begin("macrocfg", false);
  loadMacros();

  appMode = MODE_ACTIVE;
  uiDirty = true;
}

void loop() {
  M5Cardputer.update();

  if (M5Cardputer.Keyboard.isChange()) {
    if (M5Cardputer.Keyboard.isPressed()) {
      Keyboard_Class::KeysState status = M5Cardputer.Keyboard.keysState();

      bool hitZero = false;
      for (char c : status.word) {
        if (c == '0') { hitZero = true; break; }
      }

      // Don't let '0' toggle mode while the user is actively typing text into
      // a field — otherwise they can never type a literal '0' into a payload.
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
  Keyboard.print(payload);
}

void sendWinRun(const char *payload) {
  Keyboard.press(KEY_LEFT_GUI);
  Keyboard.press('r');
  delay(40);
  Keyboard.releaseAll();
  delay(120);
  Keyboard.print(payload);
  delay(40);
  Keyboard.write(KEY_RETURN);
}

// Fixed: now recognizes common named keys (ESC, TAB, ENTER, SPACE, DEL, F1-F12)
// in addition to single characters, so combos like "CTRL+SHIFT+ESC" actually
// send the final key instead of silently dropping it.
void sendHotkeyCombo(const char *payload) {
  char buf[PAYLOAD_LEN];
  strncpy(buf, payload, sizeof(buf));
  buf[sizeof(buf) - 1] = '\0';

  char *token = strtok(buf, "+");
  uint8_t finalKey = 0;
  bool    finalKeyIsChar = false;
  char    finalChar = 0;

  while (token != nullptr) {
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
      finalKey = KEY_ESC;
      finalKeyIsChar = false;
    } else if (strcasecmp(token, "TAB") == 0) {
      finalKey = KEY_TAB;
      finalKeyIsChar = false;
    } else if (strcasecmp(token, "ENTER") == 0 || strcasecmp(token, "RETURN") == 0) {
      finalKey = KEY_RETURN;
      finalKeyIsChar = false;
    } else if (strcasecmp(token, "SPACE") == 0) {
      finalChar = ' ';
      finalKeyIsChar = true;
    } else if (strcasecmp(token, "DEL") == 0 || strcasecmp(token, "DELETE") == 0) {
      finalKey = KEY_DELETE;
      finalKeyIsChar = false;
    } else if ((token[0] == 'F' || token[0] == 'f') && len >= 2 && isdigit((unsigned char)token[1])) {
      int n = atoi(token + 1);
      if (n >= 1 && n <= 12) {
        finalKey = KEY_F1 + (n - 1);
        finalKeyIsChar = false;
      }
    } else if (len == 1) {
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

    // FIELD_KEY only ever holds a single character.
    size_t destCap = (selectedField == FIELD_KEY)    ? 1
                    : (selectedField == FIELD_LABEL)  ? (LABEL_LEN - 1)
                                                       : (PAYLOAD_LEN - 1);
    for (char c : status.word) {
      if (selectedField == FIELD_KEY) {
        // Always overwrite, never accumulate, for a single-char field.
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

const char *fieldName(int field) {
  switch (field) {
    case FIELD_TYPE:    return "Type";
    case FIELD_LABEL:   return "Lbl";
    case FIELD_PAYLOAD: return "Pay";
    case FIELD_KEY:     return "Key";
    default:             return "?";
  }
}

void drawActiveModeUI() {
  canvas.fillSprite(BLACK);

  canvas.fillRect(0, 0, 240, 14, NAVY);
  canvas.setTextColor(WHITE, NAVY);
  canvas.setCursor(4, 3);
  canvas.print("MACRO [ACTIVE]  0=Edit");

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

    // Key field
    canvas.setTextColor(selectedField == FIELD_KEY ? BLACK : WHITE,
                       selectedField == FIELD_KEY ? ORANGE : BLACK);
    canvas.setCursor(124, fy);
    if (textEditing && selectedField == FIELD_KEY) {
      canvas.printf("Key:%c_", editBuffer[0] ? toupper(editBuffer[0]) : ' ');
    } else {
      canvas.printf("Key:%c", toupper(macros[selectedSlot].key));
    }
    fy += 14;

    // Type field
    canvas.setTextColor(selectedField == FIELD_TYPE ? BLACK : WHITE,
                       selectedField == FIELD_TYPE ? ORANGE : BLACK);
    canvas.setCursor(124, fy);
    canvas.printf("Type:%s", typeName(macros[selectedSlot].type));
    fy += 14;

    // Label field
    canvas.setTextColor(selectedField == FIELD_LABEL ? BLACK : WHITE,
                       selectedField == FIELD_LABEL ? ORANGE : BLACK);
    canvas.setCursor(124, fy);
    if (textEditing && selectedField == FIELD_LABEL) {
      canvas.printf("Lbl:%.12s_", editBuffer);
    } else {
      canvas.printf("Lbl:%.12s", macros[selectedSlot].label);
    }
    fy += 14;

    // Payload field
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
  }
}
