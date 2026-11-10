/*
  ============================================================================
  cardputer-macropad.ino
  ============================================================================
  M5Stack Cardputer (v1.1) -> USB HID Macro Keyboard & Command Launcher
  Target host: Windows 11

  Turns the Cardputer into a 13-slot macro keyboard. Each slot is bound to a
  key (default: 1-9, z, x, c, v) and sends one of three kinds of action:
    RAW_TEXT      types the payload literally (usernames, passwords, snippets)
    WIN_RUN       Win+R, types the payload, presses Enter (apps, URLs, paths)
    HOTKEY_COMBO  '+'-separated combo, e.g. "WIN+L" or "CTRL+SHIFT+ESC"
                  Modifiers: WIN/GUI, CTRL/CONTROL, SHIFT, ALT
                  Final key: single char, ESC, TAB, ENTER, SPACE, DEL, F1-F12

  Slots are editable on the device and saved to flash (Preferences / NVS).

  USAGE
  ACTIVE MODE (default): press a bound key to fire its macro. '0' = editor.
  EDITOR MODE:
    Slot list: Up/Down move, Right/Enter open slot, '0' exit editor.
    Fields:    Up/Down move (Key, Type, Label, Payload), Left = back,
               Enter on Type cycles RAW_TEXT -> WIN_RUN -> HOTKEY,
               Enter on Key/Label/Payload edits text (Enter saves,
               Del removes a char). '0' can be typed while text-editing.

  Just plug the Cardputer into the PC with a USB-C data cable. No drivers.

  REQUIRED LIBRARY: M5Cardputer (USB/USBHIDKeyboard ship with the ESP32 core)
  BOARD SETTINGS: USB Mode "USB-OTG (TinyUSB)", USB CDC On Boot "Enabled"
  ============================================================================
*/

#include <M5Cardputer.h>
#include <Preferences.h>
#include <USB.h>
#include <USBHIDKeyboard.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>

#define STAMP_S3_POWER_HOLD_PIN 38

// Raw HID usage codes for Cardputer arrow keys
#define HID_KEY_UP     0x52
#define HID_KEY_DOWN   0x51
#define HID_KEY_LEFT   0x50
#define HID_KEY_RIGHT  0x4F

#define MAX_SLOTS      13
#define LABEL_LEN      13
#define PAYLOAD_LEN    128   // Long enough for URLs and paths

USBHIDKeyboard usbKeyboard;

enum MacroType : uint8_t {
  TYPE_RAW_TEXT     = 0,
  TYPE_WIN_RUN      = 1,
  TYPE_HOTKEY_COMBO = 2
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

enum NavKey { NAV_NONE, NAV_UP, NAV_DOWN, NAV_LEFT, NAV_RIGHT, NAV_ENTER };
char dbgLine[48] = "";

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
bool wordHas(const Keyboard_Class::KeysState &status, char ch);
bool isEnterKey(const Keyboard_Class::KeysState &status);
bool isDelKey(const Keyboard_Class::KeysState &status);
NavKey readNav(const Keyboard_Class::KeysState &status);
const char *typeName(uint8_t type);

// ============================================================================
// REMOTE LINK (ESP-NOW) -- accepts triggers ONLY from the paired M5StickS3.
// Frames are encrypted and filtered by sender MAC.
// ============================================================================
// >>> Put the M5StickS3's MAC address here (shown on the stick's screen). <<<
static const uint8_t PEER_MAC[6] = { 0x7C, 0xE8, 0xB1, 0xE4, 0xC5, 0xF4 };

// Shared keys -- MUST be identical on the stick. Exactly 16 characters each.
static const char LINK_PMK[17] = "MacroPadPMK_2026";
static const char LINK_LMK[17] = "MacroPadLMK_2026";

#define LINK_MAGIC 0x4D50524DUL
enum LinkType : uint8_t { MSG_TRIGGER = 1, MSG_SYNC_REQ = 2, MSG_SLOT_INFO = 3, MSG_ACK = 4 };

// Keep this struct IDENTICAL in the stick sketch.
struct __attribute__((packed)) LinkMsg {
  uint32_t magic;
  uint8_t  type;
  uint8_t  slot;
  uint8_t  flags;            // SLOT_INFO: bit0 = slot has a payload. ACK: bit0 = executed OK
  uint8_t  seq;
  char     key;              // SLOT_INFO: trigger key
  char     label[LABEL_LEN]; // SLOT_INFO: label
};

volatile int     linkPendingSlot   = -1;
volatile uint8_t linkPendingSeq    = 0;
volatile bool    linkSyncRequested = false;
bool     linkReady = false;
uint8_t  ownMac[6] = {0};
char     linkStatus[28] = "";
uint32_t linkStatusUntil = 0;

bool linkPeerConfigured();
void linkHandle(const uint8_t *mac, const uint8_t *data, int len);
void linkBegin();
void linkSend(uint8_t type, uint8_t slot, uint8_t flags, uint8_t seq);
void linkSendSlotInfo(int idx);
void linkSendAllSlots();
void linkLoop();

bool linkPeerConfigured() {
  for (int i = 0; i < 6; i++) if (PEER_MAC[i] != 0) return true;
  return false;
}

// Runs in the WiFi task: only record the request, never touch HID here.
void linkHandle(const uint8_t *mac, const uint8_t *data, int len) {
  if (len != (int)sizeof(LinkMsg)) return;
  if (memcmp(mac, PEER_MAC, 6) != 0) return;       // not our stick -> drop
  LinkMsg m;
  memcpy(&m, data, sizeof(m));
  if (m.magic != LINK_MAGIC) return;

  if (m.type == MSG_TRIGGER && m.slot < MAX_SLOTS) {
    linkPendingSeq  = m.seq;
    linkPendingSlot = m.slot;
  } else if (m.type == MSG_SYNC_REQ) {
    linkSyncRequested = true;
  }
}

#if ESP_ARDUINO_VERSION_MAJOR >= 3
void linkOnRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  linkHandle(info->src_addr, data, len);
}
#else
void linkOnRecv(const uint8_t *mac, const uint8_t *data, int len) {
  linkHandle(mac, data, len);
}
#endif

void linkBegin() {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
  WiFi.macAddress(ownMac);

  if (!linkPeerConfigured()) return;
  if (esp_now_init() != ESP_OK) return;
  esp_now_set_pmk((const uint8_t *)LINK_PMK);

  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, PEER_MAC, 6);
  peer.channel = 1;
  peer.ifidx   = WIFI_IF_STA;
  peer.encrypt = true;
  memcpy(peer.lmk, LINK_LMK, 16);
  if (esp_now_add_peer(&peer) != ESP_OK) return;

  esp_now_register_recv_cb(linkOnRecv);
  linkReady = true;
}

void linkSend(uint8_t type, uint8_t slot, uint8_t flags, uint8_t seq) {
  if (!linkReady) return;
  LinkMsg m;
  memset(&m, 0, sizeof(m));
  m.magic = LINK_MAGIC;
  m.type  = type;
  m.slot  = slot;
  m.seq   = seq;
  m.flags = flags;
  if (type == MSG_SLOT_INFO && slot < MAX_SLOTS) {
    m.key = (char)macros[slot].key;
    strncpy(m.label, macros[slot].label, LABEL_LEN - 1);
    if (macros[slot].payload[0] != '\0') m.flags |= 1;
  }
  esp_now_send(PEER_MAC, (const uint8_t *)&m, sizeof(m));
}

void linkSendSlotInfo(int idx)  { linkSend(MSG_SLOT_INFO, (uint8_t)idx, 0, 0); }

void linkSendAllSlots() {
  for (int i = 0; i < MAX_SLOTS; i++) {
    linkSendSlotInfo(i);
    delay(8);
  }
}

// Called from loop(): runs requested work on the main task.
void linkLoop() {
  if (!linkReady) return;

  if (linkSyncRequested) {
    linkSyncRequested = false;
    linkSendAllSlots();
  }

  int slot = linkPendingSlot;
  if (slot >= 0) {
    linkPendingSlot = -1;
    uint8_t seq = linkPendingSeq;
    bool ok = (appMode == MODE_ACTIVE) && (slot < MAX_SLOTS) && (macros[slot].payload[0] != '\0');

    linkSend(MSG_ACK, (uint8_t)slot, ok ? 1 : 0, seq);   // ack first, then type

    if (ok) {
      snprintf(linkStatus, sizeof(linkStatus), "Remote: %.12s", macros[slot].label);
      executeMacro(slot);
    } else {
      snprintf(linkStatus, sizeof(linkStatus), "Remote ignored (busy)");
    }
    linkStatusUntil = millis() + 3000;
    uiDirty = true;
  }

  if (linkStatusUntil && millis() > linkStatusUntil) {
    linkStatusUntil = 0;
    linkStatus[0] = '\0';
    uiDirty = true;
  }
}

void setup() {
  pinMode(STAMP_S3_POWER_HOLD_PIN, OUTPUT);
  digitalWrite(STAMP_S3_POWER_HOLD_PIN, HIGH);

  auto cfg = M5.config();
  M5Cardputer.begin(cfg, true);

  // Pick a rotation that is actually landscape (wider than tall), instead of
  // assuming rotation 1 is landscape on every M5GFX version.
  {
    const uint8_t tryOrder[4] = {1, 3, 0, 2};
    for (uint8_t r : tryOrder) {
      M5Cardputer.Display.setRotation(r);
      if (M5Cardputer.Display.width() > M5Cardputer.Display.height()) break;
    }
  }
  M5Cardputer.Display.setBrightness(80);

  canvas.setColorDepth(8);
  canvas.createSprite(M5Cardputer.Display.width(), M5Cardputer.Display.height());
  canvas.setTextSize(1);

  USB.begin();
  usbKeyboard.begin();

  prefs.begin("macrocfg", false);
  loadMacros();
  linkBegin();

  appMode = MODE_ACTIVE;
  uiDirty = true;
}

void loop() {
  M5Cardputer.update();
  linkLoop();

  if (M5Cardputer.Keyboard.isChange()) {
    if (M5Cardputer.Keyboard.isPressed()) {
      Keyboard_Class::KeysState status = M5Cardputer.Keyboard.keysState();

      snprintf(dbgLine, sizeof(dbgLine), "en%d de%d fn%d h:%02X w:%c",
               (int)status.enter, (int)status.del, (int)status.fn,
               status.hid_keys.empty() ? 0 : status.hid_keys[0],
               status.word.empty() ? '-' : status.word[0]);
      uiDirty = true;

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
  usbKeyboard.print(payload);
}

void sendWinRun(const char *payload) {
  usbKeyboard.press(KEY_LEFT_GUI);
  usbKeyboard.press('r');
  delay(40);
  usbKeyboard.releaseAll();
  delay(120);
  usbKeyboard.print(payload);
  delay(40);
  usbKeyboard.write(KEY_RETURN);
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
      usbKeyboard.press(KEY_LEFT_GUI);
    } else if (strcasecmp(token, "CTRL") == 0 || strcasecmp(token, "CONTROL") == 0) {
      usbKeyboard.press(KEY_LEFT_CTRL);
    } else if (strcasecmp(token, "SHIFT") == 0) {
      usbKeyboard.press(KEY_LEFT_SHIFT);
    } else if (strcasecmp(token, "ALT") == 0) {
      usbKeyboard.press(KEY_LEFT_ALT);
    } else if (strcasecmp(token, "ESC") == 0 || strcasecmp(token, "ESCAPE") == 0) {
      finalCode = KEY_ESC;      haveFinal = true;
    } else if (strcasecmp(token, "TAB") == 0) {
      finalCode = KEY_TAB;      haveFinal = true;
    } else if (strcasecmp(token, "ENTER") == 0 || strcasecmp(token, "RETURN") == 0) {
      finalCode = KEY_RETURN;   haveFinal = true;
    } else if (strcasecmp(token, "SPACE") == 0) {
      finalCode = (uint8_t)' '; haveFinal = true;
    } else if (strcasecmp(token, "DEL") == 0 || strcasecmp(token, "DELETE") == 0) {
      finalCode = KEY_DELETE;   haveFinal = true;
    } else if ((token[0] == 'F' || token[0] == 'f') && len >= 2 && isdigit((unsigned char)token[1])) {
      int n = atoi(token + 1);
      if (n >= 1 && n <= 12) {
        finalCode = KEY_F1 + (n - 1);
        haveFinal = true;
      }
    } else if (len == 1) {
      finalCode = (uint8_t)tolower((unsigned char)token[0]);
      haveFinal = true;
    }
    token = strtok(nullptr, "+");
  }

  if (haveFinal) {
    usbKeyboard.press(finalCode);
  }

  delay(50);
  usbKeyboard.releaseAll();
}

void handleEditorInput(const Keyboard_Class::KeysState &status) {
  NavKey nav = readNav(status);

  if (textEditing) {
    if (isDelKey(status)) {
      if (editBufferLen > 0) {
        editBufferLen--;
        editBuffer[editBufferLen] = '\0';
        uiDirty = true;
      }
      return;
    }
    if (isEnterKey(status)) {
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
      if (c == '\n' || c == '\r') continue;
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
    if (nav == NAV_UP) {
      selectedSlot = (selectedSlot - 1 + MAX_SLOTS) % MAX_SLOTS;
      uiDirty = true;
    } else if (nav == NAV_DOWN) {
      selectedSlot = (selectedSlot + 1) % MAX_SLOTS;
      uiDirty = true;
    } else if (nav == NAV_ENTER || nav == NAV_RIGHT) {
      editorFocus = FOCUS_FIELD;
      selectedField = FIELD_TYPE;
      uiDirty = true;
    }
  } else {
    if (nav == NAV_LEFT) {
      editorFocus = FOCUS_LIST;
      uiDirty = true;
    } else if (nav == NAV_UP) {
      selectedField = (selectedField - 1 + FIELD_COUNT) % FIELD_COUNT;
      uiDirty = true;
    } else if (nav == NAV_DOWN) {
      selectedField = (selectedField + 1) % FIELD_COUNT;
      uiDirty = true;
    } else if (nav == NAV_ENTER) {
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

bool wordHas(const Keyboard_Class::KeysState &status, char ch) {
  for (char c : status.word) {
    if (c == ch) return true;
  }
  return false;
}

// Enter / Delete are detected several ways because the flags and HID lists
// differ between M5Cardputer library versions.
bool isEnterKey(const Keyboard_Class::KeysState &status) {
  return status.enter || hidKeyPresent(status, 0x28) || hidKeyPresent(status, 0x58) ||
         wordHas(status, '\n') || wordHas(status, '\r');
}

bool isDelKey(const Keyboard_Class::KeysState &status) {
  return status.del || hidKeyPresent(status, 0x2A) ||
         wordHas(status, '\b') || wordHas(status, 127);
}

// Navigation: real arrow HID codes (Fn+arrow keys) OR the unshifted
// ; . , / keys that carry the arrow legends on the Cardputer.
//   ; = Up    . = Down    , = Left    / = Right
NavKey readNav(const Keyboard_Class::KeysState &status) {
  if (hidKeyPresent(status, HID_KEY_UP)    || wordHas(status, ';')) return NAV_UP;
  if (hidKeyPresent(status, HID_KEY_DOWN)  || wordHas(status, '.')) return NAV_DOWN;
  if (hidKeyPresent(status, HID_KEY_LEFT)  || wordHas(status, ',')) return NAV_LEFT;
  if (hidKeyPresent(status, HID_KEY_RIGHT) || wordHas(status, '/')) return NAV_RIGHT;
  if (isEnterKey(status)) return NAV_ENTER;
  return NAV_NONE;
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

  linkSendSlotInfo(idx);   // keep the stick's list in sync
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
  canvas.print("MACRO [ACTIVE] USB  0=Edit");
  canvas.setCursor(200, 3);
  canvas.print(linkReady ? "RF:ok" : "RF:--");

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

  if (linkStatus[0]) {
    canvas.setTextColor(GREENYELLOW, BLACK);
    canvas.setCursor(4, 122);
    canvas.print(linkStatus);
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
    canvas.print("Build: USB");

    char macStr[20];
    snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
             ownMac[0], ownMac[1], ownMac[2], ownMac[3], ownMac[4], ownMac[5]);
    canvas.setCursor(124, 64);
    canvas.print("My MAC:");
    canvas.setCursor(124, 74);
    canvas.print(macStr);
    canvas.setCursor(124, 88);
    canvas.print(linkReady ? "Remote: ready" : "Remote: NO PEER");
  }

  // Key debug line (shows what the keyboard reports). Delete if not wanted.
  canvas.setTextColor(DARKGREY, BLACK);
  canvas.setCursor(4, 125);
  canvas.print(dbgLine);
}
