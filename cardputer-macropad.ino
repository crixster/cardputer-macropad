/*
  ============================================================================
  CardputerMacroPad.ino
  ============================================================================
  M5Stack Cardputer (v1.1) -> USB or BLE HID Macro Keyboard & Command Launcher
  Target host: Windows 11
  MCU: ESP32-S3 (Stamp-S3), 240x135 TFT, 56-key matrix keyboard

  ----------------------------------------------------------------------------
  WHAT THIS IS
  ----------------------------------------------------------------------------
  Turns the Cardputer into a 13-slot macro keyboard. Each slot is bound to a
  physical key on the Cardputer's keyboard (default: 1-9, z, x, c, v) and,
  when pressed, sends one of three kinds of action to whatever PC it's
  connected to (over USB cable OR Bluetooth LE -- your choice):
    - type out a fixed piece of text (e.g. a saved username/password)
    - open Windows Run (Win+R), type a command/path/URL, hit Enter
    - fire a hotkey combination (e.g. WIN+L to lock the PC)
  All 13 slots are fully editable on-device: you can change which key
  triggers a slot, what type of action it performs, its display label, and
  its payload (the text/command/combo it sends) -- no reflashing needed for
  day-to-day changes. Edits are saved to flash (Preferences / NVS) and
  survive power cycles.

  ----------------------------------------------------------------------------
  HOW TO USE IT ON THE DEVICE
  ----------------------------------------------------------------------------
  There are two screens (modes), and you switch between them with the '0' key:

  ** ACTIVE MODE (default on boot) **
    This is the "working" screen. The top bar shows your current connection
    mode (USB or BLE) and, in BLE mode, whether a host is currently paired
    and connected.
    - Press any key that's bound to a slot (shown as "[X] Label" on screen)
      and that macro fires immediately -- text gets typed / Run dialog opens
      / hotkey gets sent to whatever device you're connected to.
    - Slots with no payload set show as "--empty--" and do nothing if pressed.
    - Press '0' to enter EDITOR MODE.

  ** EDITOR MODE **
    This is where you customize slots. The left column is the slot list;
    the right panel shows the fields of whichever slot is open.
      Slot list (left column has focus):
        Up / Down  -> move the highlighted slot
        Right or Enter -> open the highlighted slot's fields (focus moves
                          to the right-hand field panel)
        'M'        -> switch connection mode (USB <-> BLE). See "SWITCHING
                      CONNECTION MODE" below -- this reboots the device.
        '0'        -> exit editor, return to Active Mode
      Field panel (right column has focus, slot is "open"):
        Up / Down  -> move between the 4 fields: Key, Type, Label, Payload
        Left       -> go back to the slot list (closes this slot)
        Enter on "Type"    -> cycles RAW_TEXT -> WIN_RUN -> HOTKEY -> ...
        Enter on "Key/Label/Payload" -> starts text-editing that field:
            - type on the Cardputer keyboard to enter text
            - Backspace/Del removes the last character
            - Enter saves the field and exits text-editing
          (the "Key" field only ever keeps 1 character -- whatever you type
          last replaces it, since a trigger key is always a single key)
        '0' is NOT available as "exit" while text-editing, so you can safely
          type a literal 0 into a payload (e.g. part of a password or URL).

  ** SWITCHING CONNECTION MODE (USB <-> BLE) **
    From the editor's slot list (list must have focus, not a field, and you
    must NOT be mid-edit of a text field), press 'M'.
    The screen shows "SWITCHING MODE... Restarting" and the device reboots
    itself after ~1.2 seconds. On boot it reads the saved mode from flash
    and initializes ONLY that one HID backend (USB or BLE) -- the other is
    never touched. This is intentional: swapping the USB and BLE stacks
    live, without a reboot, is not reliable on ESP32-S3, so the device
    always comes up clean in whichever mode you last chose, and stays in
    that mode across normal power cycles too (it's saved, not just for this
    session).

  ** PAIRING OVER BLUETOOTH (BLE mode) **
    Once booted in BLE mode, the device advertises itself as a Bluetooth LE
    keyboard named "Cardputer MacroPad".
    On the Windows 11 PC: Settings -> Bluetooth & devices -> Add device ->
    Bluetooth -> select "Cardputer MacroPad" -> it pairs like any BT
    keyboard, no PIN needed. The Cardputer's top status bar shows "BLE:..."
    while waiting and "BLE:OK" once a host is actually connected. Macro
    keys are ignored (silently) until a host is connected, so you can't
    accidentally fire a macro into nothing.
    If Windows ever shows it as paired but stuck "not connecting": remove
    the device from Windows Bluetooth settings and re-pair. That's a known
    quirk of BLE HID devices in general, not specific to this code.

  ** USING OVER USB (USB mode) **
    Just plug the Cardputer into the PC with a USB-C data cable. No pairing,
    no drivers -- Windows sees a standard USB HID keyboard immediately.

  ----------------------------------------------------------------------------
  MACRO TYPES EXPLAINED (what each Type does with its Payload)
  ----------------------------------------------------------------------------
    RAW_TEXT      Payload is typed out literally, character by character.
                  Good for usernames, passwords, snippets.
    WIN_RUN       Sends Win+R (opens the Windows Run dialog), waits briefly
                  for it to open, types the Payload, then presses Enter.
                  Good for launching apps ("notepad", "calc") or opening a
                  URL/file path directly.
    HOTKEY_COMBO  Payload is parsed as '+'-separated tokens, e.g.
                  "CTRL+SHIFT+ESC" or "WIN+L". Recognized modifier tokens:
                  WIN/GUI, CTRL/CONTROL, SHIFT, ALT. Recognized final-key
                  tokens: any single character (a-z, 0-9, etc.), ESC/ESCAPE,
                  TAB, ENTER/RETURN, SPACE, DEL/DELETE, F1 through F12.
                  All modifiers are held down, then the final key is
                  pressed, then everything is released.

  ----------------------------------------------------------------------------
  REQUIRED LIBRARIES (install via Arduino Library Manager, or list them in
  your CI workflow if you're building headless / via GitHub Actions)
  ----------------------------------------------------------------------------
    - M5Cardputer   (and its dependencies M5Unified, M5GFX)
    - "ESP32 BLE Keyboard" by T-vK  <-- provides BleKeyboard.h
    - "NimBLE-Arduino" by h2zero    <-- required dependency of the above;
                                        BleKeyboard.h will fail to compile
                                        or link without it even though
                                        nothing in this .ino includes it
                                        directly.
  If you're compiling in GitHub Actions (e.g. with arduino/compile-sketches
  or raw arduino-cli), make sure BOTH of the above are explicitly installed
  in the workflow -- they are not pulled in automatically just because
  M5Cardputer is listed, and a missing library manifests as
  "fatal error: BleKeyboard.h: No such file or directory".

  ----------------------------------------------------------------------------
  BOARD SETTINGS (Arduino IDE, Tools menu)
  ----------------------------------------------------------------------------
    USB Mode:        "USB-OTG (TinyUSB)"
    USB CDC On Boot: "Enabled"
  Required for USBHIDKeyboard to resolve/compile on ESP32-S3 targets at all,
  independent of which connection mode you actually run in.

  ----------------------------------------------------------------------------
  NOTE ON KEY-CODE MACROS (implementation detail, not user-facing)
  ----------------------------------------------------------------------------
  USBHIDKeyboard.h and BleKeyboard.h both define identically-named macros
  (KEY_LEFT_GUI, KEY_ESC, KEY_F1, ...). Including both headers in one file
  causes macro-redefinition errors. We dodge this by capturing each
  library's constants under our own prefixed names (USBKEY_*, BLEKEY_*)
  immediately after including each header, then #undef-ing the shared names
  before the next header is pulled in.
  ============================================================================
*/

#include <M5Cardputer.h>
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

// ----------------------------------------------------------------------------
// HID backend #1: USB
// ----------------------------------------------------------------------------
#include <USB.h>
#include <USBHIDKeyboard.h>

#define USBKEY_LEFT_CTRL   KEY_LEFT_CTRL
#define USBKEY_LEFT_SHIFT  KEY_LEFT_SHIFT
#define USBKEY_LEFT_ALT    KEY_LEFT_ALT
#define USBKEY_LEFT_GUI    KEY_LEFT_GUI
#define USBKEY_RETURN      KEY_RETURN
#define USBKEY_ESC         KEY_ESC
#define USBKEY_TAB         KEY_TAB
#define USBKEY_DELETE      KEY_DELETE
#define USBKEY_F1          KEY_F1

#undef KEY_LEFT_CTRL
#undef KEY_LEFT_SHIFT
#undef KEY_LEFT_ALT
#undef KEY_LEFT_GUI
#undef KEY_RETURN
#undef KEY_ESC
#undef KEY_TAB
#undef KEY_DELETE
#undef KEY_F1

// ----------------------------------------------------------------------------
// HID backend #2: BLE  (T-vK "ESP32 BLE Keyboard" library, needs NimBLE-Arduino)
// ----------------------------------------------------------------------------
#include <BleKeyboard.h>

#define BLEKEY_LEFT_CTRL   KEY_LEFT_CTRL
#define BLEKEY_LEFT_SHIFT  KEY_LEFT_SHIFT
#define BLEKEY_LEFT_ALT    KEY_LEFT_ALT
#define BLEKEY_LEFT_GUI    KEY_LEFT_GUI
#define BLEKEY_RETURN      KEY_RETURN
#define BLEKEY_ESC         KEY_ESC
#define BLEKEY_TAB         KEY_TAB
#define BLEKEY_DELETE      KEY_DELETE
#define BLEKEY_F1          KEY_F1

#undef KEY_LEFT_CTRL
#undef KEY_LEFT_SHIFT
#undef KEY_LEFT_ALT
#undef KEY_LEFT_GUI
#undef KEY_RETURN
#undef KEY_ESC
#undef KEY_TAB
#undef KEY_DELETE
#undef KEY_F1

// ----------------------------------------------------------------------------
// Connection mode
// ----------------------------------------------------------------------------
enum ConnMode : uint8_t { CONN_USB = 0, CONN_BLE = 1 };
ConnMode connMode = CONN_USB;

USBHIDKeyboard usbKeyboard;
BleKeyboard    bleKeyboard("Cardputer MacroPad", "DIY", 100);

bool bleWasConnected = false;  // for redraw-on-connect

// ----------------------------------------------------------------------------
// Named special keys, resolved to the correct backend's code at send time
// ----------------------------------------------------------------------------
enum SpecialKey {
  SK_GUI, SK_CTRL, SK_SHIFT, SK_ALT,
  SK_ESC, SK_TAB, SK_ENTER, SK_DELETE,
  SK_F1, SK_F2, SK_F3, SK_F4, SK_F5, SK_F6,
  SK_F7, SK_F8, SK_F9, SK_F10, SK_F11, SK_F12
};

uint8_t resolveKey(SpecialKey sk) {
  if (connMode == CONN_USB) {
    switch (sk) {
      case SK_GUI:    return USBKEY_LEFT_GUI;
      case SK_CTRL:   return USBKEY_LEFT_CTRL;
      case SK_SHIFT:  return USBKEY_LEFT_SHIFT;
      case SK_ALT:    return USBKEY_LEFT_ALT;
      case SK_ESC:    return USBKEY_ESC;
      case SK_TAB:    return USBKEY_TAB;
      case SK_ENTER:  return USBKEY_RETURN;
      case SK_DELETE: return USBKEY_DELETE;
      default:        return USBKEY_F1 + (sk - SK_F1); // F1..F12 are contiguous
    }
  } else {
    switch (sk) {
      case SK_GUI:    return BLEKEY_LEFT_GUI;
      case SK_CTRL:   return BLEKEY_LEFT_CTRL;
      case SK_SHIFT:  return BLEKEY_LEFT_SHIFT;
      case SK_ALT:    return BLEKEY_LEFT_ALT;
      case SK_ESC:    return BLEKEY_ESC;
      case SK_TAB:    return BLEKEY_TAB;
      case SK_ENTER:  return BLEKEY_RETURN;
      case SK_DELETE: return BLEKEY_DELETE;
      default:        return BLEKEY_F1 + (sk - SK_F1);
    }
  }
}

// ----------------------------------------------------------------------------
// HID wrapper functions -- everything else in the sketch calls these instead
// of touching usbKeyboard / bleKeyboard directly
// ----------------------------------------------------------------------------
void hidPress(uint8_t code) {
  if (connMode == CONN_USB) usbKeyboard.press(code);
  else                      bleKeyboard.press(code);
}

void hidPressSpecial(SpecialKey sk) {
  hidPress(resolveKey(sk));
}

void hidReleaseAll() {
  if (connMode == CONN_USB) usbKeyboard.releaseAll();
  else                      bleKeyboard.releaseAll();
}

void hidPrint(const char *s) {
  if (connMode == CONN_USB) usbKeyboard.print(s);
  else                      bleKeyboard.print(s);
}

void hidWriteEnter() {
  if (connMode == CONN_USB) usbKeyboard.write(USBKEY_RETURN);
  else                      bleKeyboard.write(BLEKEY_RETURN);
}

bool hidReady() {
  // USB HID has no explicit "connected" concept here (assume ready once begun);
  // BLE must actually be paired/connected to a host.
  if (connMode == CONN_USB) return true;
  return bleKeyboard.isConnected();
}

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

Preferences prefs;
M5Canvas    canvas(&M5Cardputer.Display);

enum AppMode { MODE_ACTIVE, MODE_EDITOR };
AppMode appMode = MODE_ACTIVE;

enum EditorFocus { FOCUS_LIST, FOCUS_FIELD };
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
void drawModeSwitchConfirm();
void handleActiveModeInput(const Keyboard_Class::KeysState &status);
void handleEditorInput(const Keyboard_Class::KeysState &status);
void executeMacro(int idx);
void sendRawText(const char *payload);
void sendWinRun(const char *payload);
void sendHotkeyCombo(const char *payload);
void toggleMode();
bool hidKeyPresent(const Keyboard_Class::KeysState &status, uint8_t code);
const char *typeName(uint8_t type);
void loadConnMode();
void beginActiveBackend();
void requestConnModeSwitch();

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

  prefs.begin("macrocfg", false);

  loadConnMode();       // decide USB vs BLE before touching either stack
  beginActiveBackend(); // initialize ONLY the chosen backend

  loadMacros();

  appMode = MODE_ACTIVE;
  uiDirty = true;
}

void loadConnMode() {
  uint8_t m = prefs.getUChar("connmode", CONN_USB);
  connMode = (m == CONN_BLE) ? CONN_BLE : CONN_USB;
}

void beginActiveBackend() {
  if (connMode == CONN_USB) {
    USB.begin();
    usbKeyboard.begin();
  } else {
    bleKeyboard.begin();
  }
}

void requestConnModeSwitch() {
  ConnMode newMode = (connMode == CONN_USB) ? CONN_BLE : CONN_USB;
  prefs.putUChar("connmode", newMode);
  drawModeSwitchConfirm();   // show "restarting..." before the reboot blip
  delay(1200);
  ESP.restart();
}

void loop() {
  M5Cardputer.update();

  bool nowConnected = hidReady();
  if (connMode == CONN_BLE && nowConnected != bleWasConnected) {
    bleWasConnected = nowConnected;
    uiDirty = true;
  }

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
  hidPressSpecial(SK_GUI);
  hidPress((uint8_t)'r');
  delay(40);
  hidReleaseAll();
  delay(120);
  hidPrint(payload);
  delay(40);
  hidWriteEnter();
}

// Recognizes modifier names plus ESC/TAB/ENTER/SPACE/DEL/F1-F12 as the final
// key, so combos like "CTRL+SHIFT+ESC" actually send the final key instead
// of silently dropping it.
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
      hidPressSpecial(SK_GUI);
    } else if (strcasecmp(token, "CTRL") == 0 || strcasecmp(token, "CONTROL") == 0) {
      hidPressSpecial(SK_CTRL);
    } else if (strcasecmp(token, "SHIFT") == 0) {
      hidPressSpecial(SK_SHIFT);
    } else if (strcasecmp(token, "ALT") == 0) {
      hidPressSpecial(SK_ALT);
    } else if (strcasecmp(token, "ESC") == 0 || strcasecmp(token, "ESCAPE") == 0) {
      finalCode = resolveKey(SK_ESC);   haveFinal = true;
    } else if (strcasecmp(token, "TAB") == 0) {
      finalCode = resolveKey(SK_TAB);   haveFinal = true;
    } else if (strcasecmp(token, "ENTER") == 0 || strcasecmp(token, "RETURN") == 0) {
      finalCode = resolveKey(SK_ENTER); haveFinal = true;
    } else if (strcasecmp(token, "SPACE") == 0) {
      finalCode = (uint8_t)' ';         haveFinal = true;
    } else if (strcasecmp(token, "DEL") == 0 || strcasecmp(token, "DELETE") == 0) {
      finalCode = resolveKey(SK_DELETE); haveFinal = true;
    } else if ((token[0] == 'F' || token[0] == 'f') && len >= 2 && isdigit((unsigned char)token[1])) {
      int n = atoi(token + 1);
      if (n >= 1 && n <= 12) {
        finalCode = resolveKey((SpecialKey)(SK_F1 + (n - 1)));
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
    // 'M' toggles the connection mode (USB<->BLE) and reboots -- only here,
    // not while a field is open, so it doesn't collide with typed text.
    for (char c : status.word) {
      if (tolower((unsigned char)c) == 'm') {
        requestConnModeSwitch();
        return;
      }
    }
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
  if (connMode == CONN_USB) {
    canvas.print("MACRO [ACTIVE] USB  0=Edit");
  } else {
    canvas.printf("MACRO [ACTIVE] BLE:%s 0=Edit", hidReady() ? "OK" : "...");
  }

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

  if (connMode == CONN_BLE && !hidReady()) {
    canvas.setTextColor(ORANGE, BLACK);
    canvas.setCursor(4, 122);
    canvas.print("Waiting for BLE pairing...");
  }
}

void drawEditorUI() {
  canvas.fillSprite(BLACK);

  canvas.fillRect(0, 0, 240, 14, MAROON);
  canvas.setTextColor(WHITE, MAROON);
  canvas.setCursor(4, 3);
  canvas.print("EDITOR 0=Exit  M=USB/BLE");

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
    canvas.printf("Mode: %s", connMode == CONN_USB ? "USB" : "BLE");
  }
}

void drawModeSwitchConfirm() {
  canvas.fillSprite(BLACK);
  canvas.fillRect(0, 0, 240, 14, MAROON);
  canvas.setTextColor(WHITE, MAROON);
  canvas.setCursor(4, 3);
  canvas.print("SWITCHING MODE");

  ConnMode newMode = (connMode == CONN_USB) ? CONN_BLE : CONN_USB;
  canvas.setTextColor(WHITE, BLACK);
  canvas.setCursor(10, 40);
  canvas.printf("Switching to: %s", newMode == CONN_USB ? "USB" : "BLE");
  canvas.setCursor(10, 56);
  canvas.print("Restarting...");
  canvas.pushSprite(0, 0);
}
