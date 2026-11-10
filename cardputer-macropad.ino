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
    Slot list: Up/Down move, Right/Enter open slot, '0' exit editor,
               'P' open the Pairing screen (manage up to 4 remote devices).
    Fields:    Up/Down move (Key, Type, Label, Payload), Left = back,
               Enter on Type cycles RAW_TEXT -> WIN_RUN -> HOTKEY,
               Enter on Key edits the trigger key (1 char),
               Enter on Label/Payload opens a full-screen text editor
               (Enter saves, Del backspaces, '0' can be typed as text).
  PAIRING MODE (press 'P' from the editor's slot list):
    Up/Down move between the 4 device slots, Right/Enter opens a slot,
    Enter on "Enabled" toggles it on/off, Enter on Name/MAC opens the
    full keyboard editor (MAC is 12 hex characters, no colons), Left/'0'
    goes back a level. The Cardputer's own MAC is shown at the top so you
    can read it off for pairing the remote in the other direction.

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
#define MAX_PEERS      4     // Remote (stick) devices this Cardputer can pair with

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
  { false, '9', TYPE_WIN_RUN,      "chrome",     "chrome" },
  { false, 'z', TYPE_WIN_RUN,      "BMC",      "https://continentalit-smartit.onbmc.com/smartit/app/#/ticket-console" },
  { false, 'x', TYPE_WIN_RUN,      "Adam",   "D:\ADAM\ADAM.bat" },
  { false, 'c', TYPE_HOTKEY_COMBO, "Lock PC",  "WIN+L" },
  { false, 'v', TYPE_WIN_RUN,      "Shutdown",   "D:\ForcedShutdown.bat" },
};

Preferences prefs;
M5Canvas    canvas(&M5Cardputer.Display);

enum AppMode { MODE_ACTIVE, MODE_EDITOR, MODE_PAIRING };
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

// Pairing screen state (declared early so handleEditorInput can reach it
// when 'P' switches into MODE_PAIRING).
enum PairFocus { PFOCUS_LIST, PFOCUS_FIELD };
enum PairField { PF_ENABLED = 0, PF_NAME = 1, PF_MAC = 2, PF_COUNT = 3 };
PairFocus pairFocus = PFOCUS_LIST;
PairField pairField = PF_ENABLED;
int       pairSel   = 0;

// Battery
int      batteryPct   = -1;
uint32_t lastBattPoll = 0;

void loadMacros();
void saveSlot(int idx);
void drawActiveModeUI();
void drawEditorUI();
void drawTextEditOverlay(const char *fieldName, size_t limit);
void drawPairingUI();
void handleActiveModeInput(const Keyboard_Class::KeysState &status);
void handleEditorInput(const Keyboard_Class::KeysState &status);
void handlePairingInput(const Keyboard_Class::KeysState &status);
void executeMacro(int idx);
void sendRawText(const char *payload);
void sendWinRun(const char *payload);
void sendHotkeyCombo(const char *payload);
void toggleMode();
bool hidKeyPresent(const Keyboard_Class::KeysState &status, uint8_t code);
bool wordHas(const Keyboard_Class::KeysState &status, char ch);
bool isEnterKey(const Keyboard_Class::KeysState &status);
bool isDelKey(const Keyboard_Class::KeysState &status);
bool isHexChar(char c);
NavKey readNav(const Keyboard_Class::KeysState &status);
const char *typeName(uint8_t type);
void macToHexStr(const uint8_t *mac, char *out);
void hexStrToMac(const char *s, uint8_t *mac);

// ============================================================================
// REMOTE LINK (ESP-NOW) -- accepts triggers ONLY from paired devices below.
// Frames are encrypted and filtered by sender MAC. Up to MAX_PEERS remotes
// (e.g. M5StickS3 units) can be paired; each has a friendly name, an on/off
// switch, and a MAC you type in on the Pairing screen (press 'P' from the
// editor's slot list). We also broadcast an unencrypted "announce" frame
// every few seconds so a remote can auto-capture our MAC when it is put
// into its own pairing/listening mode -- see stick-remote.ino.
// ============================================================================

// Shared keys -- MUST be identical on every paired remote. Exactly 16 chars.
static const char LINK_PMK[17] = "MacroPadPMK_2026";
static const char LINK_LMK[17] = "MacroPadLMK_2026";

#define LINK_MAGIC 0x4D50524DUL
enum LinkType : uint8_t {
  MSG_TRIGGER   = 1,
  MSG_SYNC_REQ  = 2,
  MSG_SLOT_INFO = 3,
  MSG_ACK       = 4,
  MSG_ANNOUNCE  = 5   // unencrypted broadcast: "here is my MAC", for auto-pair
};

// Keep this struct IDENTICAL in the remote's sketch.
struct __attribute__((packed)) LinkMsg {
  uint32_t magic;
  uint8_t  type;
  uint8_t  slot;
  uint8_t  flags;            // SLOT_INFO: bit0 = slot has a payload. ACK: bit0 = executed OK
  uint8_t  seq;
  char     key;              // SLOT_INFO: trigger key
  char     label[LABEL_LEN]; // SLOT_INFO: label
};

struct PeerSlot {
  bool    enabled;
  uint8_t mac[6];
  char    name[LABEL_LEN];
};

// Slot 0 defaults to the MAC that used to be hardcoded as PEER_MAC. Edit any
// slot from the Pairing screen instead of recompiling.
PeerSlot peers[MAX_PEERS] = {
  { true,  {0x7C, 0xE8, 0xB1, 0xE4, 0xC5, 0xF4}, "Remote 1" },
  { false, {0, 0, 0, 0, 0, 0},                   "" },
  { false, {0, 0, 0, 0, 0, 0},                   "" },
  { false, {0, 0, 0, 0, 0, 0},                   "" },
};

static const uint8_t BROADCAST_MAC[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

volatile int     linkPendingSlot   = -1;
volatile int     linkPendingPeer   = -1;
volatile uint8_t linkPendingSeq    = 0;
volatile bool    linkSyncRequested = false;
volatile int     linkSyncRequester = -1;
bool     linkReady = false;
uint8_t  ownMac[6] = {0};
char     linkStatus[28] = "";
uint32_t linkStatusUntil = 0;

void loadPeers();
void savePeer(int idx);
void rebuildPeerRegistration();
int  findPeerIndex(const uint8_t *mac);
bool anyPeerEnabled();
void linkHandle(const uint8_t *mac, const uint8_t *data, int len);
void linkBegin();
void linkSend(const uint8_t *targetMac, uint8_t type, uint8_t slot, uint8_t flags, uint8_t seq);
void linkSendSlotInfo(int idx, const uint8_t *targetMac);
void linkSendAllSlots(const uint8_t *targetMac);
void linkBroadcastSlotInfo(int idx);
void linkSendAnnounce();
void linkLoop();

int findPeerIndex(const uint8_t *mac) {
  for (int i = 0; i < MAX_PEERS; i++) {
    if (peers[i].enabled && memcmp(mac, peers[i].mac, 6) == 0) return i;
  }
  return -1;
}

bool anyPeerEnabled() {
  for (int i = 0; i < MAX_PEERS; i++) if (peers[i].enabled) return true;
  return false;
}

// Runs in the WiFi task: only record the request, never touch HID here.
void linkHandle(const uint8_t *mac, const uint8_t *data, int len) {
  if (len != (int)sizeof(LinkMsg)) return;
  LinkMsg m;
  memcpy(&m, data, sizeof(m));
  if (m.magic != LINK_MAGIC) return;

  int idx = findPeerIndex(mac);
  if (idx < 0) return;   // not one of our paired remotes -> drop

  if (m.type == MSG_TRIGGER && m.slot < MAX_SLOTS) {
    linkPendingSeq  = m.seq;
    linkPendingPeer = idx;
    linkPendingSlot = m.slot;   // set last: loop() polls this field
  } else if (m.type == MSG_SYNC_REQ) {
    linkSyncRequester = idx;
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

void rebuildPeerRegistration() {
  // Drop any previously-registered unicast peers, then re-add the enabled
  // ones. Deleting a MAC that was never added simply fails silently.
  for (int i = 0; i < MAX_PEERS; i++) {
    esp_now_del_peer(peers[i].mac);
  }
  for (int i = 0; i < MAX_PEERS; i++) {
    if (!peers[i].enabled) continue;
    bool allZero = true;
    for (int b = 0; b < 6; b++) if (peers[i].mac[b] != 0) { allZero = false; break; }
    if (allZero) continue;

    esp_now_peer_info_t p = {};
    memcpy(p.peer_addr, peers[i].mac, 6);
    p.channel = 1;
    p.ifidx   = WIFI_IF_STA;
    p.encrypt = true;
    memcpy(p.lmk, LINK_LMK, 16);
    esp_now_add_peer(&p);
  }
}

void linkBegin() {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
  WiFi.macAddress(ownMac);

  if (esp_now_init() != ESP_OK) return;
  esp_now_set_pmk((const uint8_t *)LINK_PMK);

  // Unencrypted broadcast peer, used only to announce our MAC so a remote
  // can auto-pair without anyone typing hex digits on it.
  esp_now_peer_info_t bpeer = {};
  memcpy(bpeer.peer_addr, BROADCAST_MAC, 6);
  bpeer.channel = 1;
  bpeer.ifidx   = WIFI_IF_STA;
  bpeer.encrypt = false;
  esp_now_add_peer(&bpeer);

  rebuildPeerRegistration();
  esp_now_register_recv_cb(linkOnRecv);
  linkReady = true;
}

void linkSend(const uint8_t *targetMac, uint8_t type, uint8_t slot, uint8_t flags, uint8_t seq) {
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
  esp_now_send(targetMac, (const uint8_t *)&m, sizeof(m));
}

void linkSendSlotInfo(int idx, const uint8_t *targetMac) { linkSend(targetMac, MSG_SLOT_INFO, (uint8_t)idx, 0, 0); }

void linkSendAllSlots(const uint8_t *targetMac) {
  for (int i = 0; i < MAX_SLOTS; i++) {
    linkSendSlotInfo(i, targetMac);
    delay(8);
  }
}

// Tell every paired, enabled remote about a slot change.
void linkBroadcastSlotInfo(int idx) {
  for (int i = 0; i < MAX_PEERS; i++) {
    if (peers[i].enabled) linkSendSlotInfo(idx, peers[i].mac);
  }
}

void linkSendAnnounce() {
  linkSend(BROADCAST_MAC, MSG_ANNOUNCE, 0, 0, 0);
}

// Called from loop(): runs requested work on the main task.
void linkLoop() {
  if (!linkReady) return;

  static uint32_t lastAnnounce = 0;
  if (millis() - lastAnnounce > 3000) {
    linkSendAnnounce();
    lastAnnounce = millis();
  }

  if (linkSyncRequested) {
    linkSyncRequested = false;
    int req = linkSyncRequester;
    if (req >= 0 && req < MAX_PEERS && peers[req].enabled) linkSendAllSlots(peers[req].mac);
  }

  int slot = linkPendingSlot;
  if (slot >= 0) {
    linkPendingSlot = -1;
    uint8_t seq     = linkPendingSeq;
    int     peerIdx = linkPendingPeer;
    bool ok = (appMode == MODE_ACTIVE) && (slot < MAX_SLOTS) && (macros[slot].payload[0] != '\0');

    if (peerIdx >= 0 && peerIdx < MAX_PEERS) {
      linkSend(peers[peerIdx].mac, MSG_ACK, (uint8_t)slot, ok ? 1 : 0, seq);   // ack first, then type
      if (ok) {
        snprintf(linkStatus, sizeof(linkStatus), "%.8s: %.12s", peers[peerIdx].name, macros[slot].label);
        executeMacro(slot);
      } else {
        snprintf(linkStatus, sizeof(linkStatus), "Remote ignored (busy)");
      }
      linkStatusUntil = millis() + 3000;
      uiDirty = true;
    }
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
  loadPeers();
  linkBegin();

  batteryPct   = M5Cardputer.Power.getBatteryLevel();
  lastBattPoll = millis();

  appMode = MODE_ACTIVE;
  uiDirty = true;
}

void loop() {
  M5Cardputer.update();
  linkLoop();

  if (millis() - lastBattPoll > 5000) {
    int lvl = M5Cardputer.Power.getBatteryLevel();
    if (lvl != batteryPct) { batteryPct = lvl; uiDirty = true; }
    lastBattPoll = millis();
  }

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

      // '0' toggles ACTIVE<->EDITOR, except while actively typing into a
      // field, or anywhere in the Pairing screen (where it means "back").
      bool blockZeroToggle = (appMode == MODE_PAIRING) || (appMode == MODE_EDITOR && textEditing);

      if (hitZero && !blockZeroToggle) {
        toggleMode();
      } else if (appMode == MODE_ACTIVE) {
        handleActiveModeInput(status);
      } else if (appMode == MODE_EDITOR) {
        handleEditorInput(status);
      } else {
        handlePairingInput(status);
      }
    }
  }

  if (uiDirty) {
    if (appMode == MODE_ACTIVE) {
      drawActiveModeUI();
    } else if (appMode == MODE_EDITOR) {
      drawEditorUI();
    } else {
      drawPairingUI();
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
  // 'P' opens the Pairing screen (only while browsing the slot list, never
  // while a field is being typed into).
  if (!textEditing && editorFocus == FOCUS_LIST) {
    for (char c : status.word) {
      if (c == 'p' || c == 'P') {
        appMode    = MODE_PAIRING;
        pairFocus  = PFOCUS_LIST;
        uiDirty    = true;
        return;
      }
    }
  }

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

// ---------------------------------------------------------------------------
// Pairing screen
// ---------------------------------------------------------------------------
bool isHexChar(char c) {
  return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
}

void macToHexStr(const uint8_t *mac, char *out) {
  snprintf(out, 13, "%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

void hexStrToMac(const char *s, uint8_t *mac) {
  auto nib = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return 0;
  };
  for (int i = 0; i < 6; i++) {
    mac[i] = (uint8_t)((nib(s[i * 2]) << 4) | nib(s[i * 2 + 1]));
  }
}

void handlePairingInput(const Keyboard_Class::KeysState &status) {
  bool zero = false;
  for (char c : status.word) if (c == '0') { zero = true; break; }

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
      if (pairField == PF_NAME) {
        strncpy(peers[pairSel].name, editBuffer, LABEL_LEN - 1);
        peers[pairSel].name[LABEL_LEN - 1] = '\0';
        savePeer(pairSel);
      } else if (pairField == PF_MAC) {
        if (editBufferLen == 12) {
          hexStrToMac(editBuffer, peers[pairSel].mac);
          savePeer(pairSel);
        } else {
          snprintf(linkStatus, sizeof(linkStatus), "MAC needs 12 hex chars");
          linkStatusUntil = millis() + 2500;
        }
      }
      textEditing = false;
      uiDirty = true;
      return;
    }

    size_t destCap = (pairField == PF_NAME) ? (LABEL_LEN - 1) : 12;
    for (char c : status.word) {
      if (c == '\n' || c == '\r') continue;
      if (pairField == PF_MAC && !isHexChar(c)) continue;
      if (editBufferLen < destCap) {
        editBuffer[editBufferLen++] = (pairField == PF_MAC) ? (char)toupper((unsigned char)c) : c;
        editBuffer[editBufferLen] = '\0';
        uiDirty = true;
      }
    }
    return;
  }

  if (zero) {
    if (pairFocus == PFOCUS_FIELD) pairFocus = PFOCUS_LIST;
    else appMode = MODE_EDITOR;
    uiDirty = true;
    return;
  }

  NavKey nav = readNav(status);
  if (pairFocus == PFOCUS_LIST) {
    if (nav == NAV_UP) {
      pairSel = (pairSel - 1 + MAX_PEERS) % MAX_PEERS;
      uiDirty = true;
    } else if (nav == NAV_DOWN) {
      pairSel = (pairSel + 1) % MAX_PEERS;
      uiDirty = true;
    } else if (nav == NAV_ENTER || nav == NAV_RIGHT) {
      pairFocus = PFOCUS_FIELD;
      pairField = PF_ENABLED;
      uiDirty = true;
    }
  } else {
    if (nav == NAV_LEFT) {
      pairFocus = PFOCUS_LIST;
      uiDirty = true;
    } else if (nav == NAV_UP) {
      pairField = (PairField)((pairField - 1 + PF_COUNT) % PF_COUNT);
      uiDirty = true;
    } else if (nav == NAV_DOWN) {
      pairField = (PairField)((pairField + 1) % PF_COUNT);
      uiDirty = true;
    } else if (nav == NAV_ENTER) {
      if (pairField == PF_ENABLED) {
        peers[pairSel].enabled = !peers[pairSel].enabled;
        savePeer(pairSel);
        uiDirty = true;
      } else if (pairField == PF_NAME) {
        textEditing = true;
        strncpy(editBuffer, peers[pairSel].name, sizeof(editBuffer) - 1);
        editBuffer[sizeof(editBuffer) - 1] = '\0';
        editBufferLen = strlen(editBuffer);
        uiDirty = true;
      } else if (pairField == PF_MAC) {
        textEditing = true;
        macToHexStr(peers[pairSel].mac, editBuffer);
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

  linkBroadcastSlotInfo(idx);   // keep every paired remote's list in sync
}

void loadPeers() {
  char key[8];
  for (int i = 0; i < MAX_PEERS; i++) {
    snprintf(key, sizeof(key), "pe%d", i);
    if (!prefs.isKey(key)) continue;   // nothing saved yet for this slot -> keep compiled default

    peers[i].enabled = prefs.getUChar(key, peers[i].enabled ? 1 : 0) != 0;

    snprintf(key, sizeof(key), "pm%d", i);
    if (prefs.getBytesLength(key) == 6) {
      uint8_t mac[6];
      prefs.getBytes(key, mac, 6);
      memcpy(peers[i].mac, mac, 6);
    }

    snprintf(key, sizeof(key), "pn%d", i);
    String nm = prefs.getString(key, peers[i].name);
    strncpy(peers[i].name, nm.c_str(), LABEL_LEN - 1);
    peers[i].name[LABEL_LEN - 1] = '\0';
  }
}

void savePeer(int idx) {
  char key[8];
  snprintf(key, sizeof(key), "pe%d", idx);
  prefs.putUChar(key, peers[idx].enabled ? 1 : 0);

  snprintf(key, sizeof(key), "pm%d", idx);
  prefs.putBytes(key, peers[idx].mac, 6);

  snprintf(key, sizeof(key), "pn%d", idx);
  prefs.putString(key, peers[idx].name);

  rebuildPeerRegistration();
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
  canvas.setCursor(168, 3);
  canvas.printf("B:%d%%", batteryPct);
  canvas.setCursor(206, 3);
  canvas.print(linkReady ? "R:ok" : "R:--");

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

// Full-screen editor used for Label and Payload, so the 128-char payload
// field is actually readable while typing (word-wrapped, with a counter).
void drawTextEditOverlay(const char *fieldName, size_t limit) {
  canvas.fillSprite(BLACK);

  canvas.fillRect(0, 0, 240, 14, MAROON);
  canvas.setTextColor(WHITE, MAROON);
  canvas.setCursor(4, 3);
  canvas.printf("EDIT %s", fieldName);
  canvas.setCursor(180, 3);
  canvas.printf("%u/%u", (unsigned)editBufferLen, (unsigned)limit);

  canvas.setTextColor(WHITE, BLACK);
  const int charsPerLine = 38;
  const int lineHeight   = 11;
  int y = 20;
  size_t pos = 0;
  char lineBuf[charsPerLine + 1];
  size_t lastLineLen = 0;
  while (pos < editBufferLen && y < 110) {
    size_t n = editBufferLen - pos;
    if (n > (size_t)charsPerLine) n = charsPerLine;
    memcpy(lineBuf, editBuffer + pos, n);
    lineBuf[n] = '\0';
    canvas.setCursor(4, y);
    canvas.print(lineBuf);
    pos += n;
    lastLineLen = n;
    y += lineHeight;
  }
  if (y < 110) {
    // Blinking-style cursor marker right after the last character typed.
    int cursorX = 4 + (int)(lastLineLen * 6);
    canvas.setCursor(cursorX, y - lineHeight);
    canvas.print("_");
  }

  canvas.setTextColor(LIGHTGREY, BLACK);
  canvas.setCursor(4, 122);
  canvas.print("ENTER=save  DEL=backspace");
}

void drawEditorUI() {
  if (textEditing && (selectedField == FIELD_PAYLOAD || selectedField == FIELD_LABEL)) {
    const char *fname = (selectedField == FIELD_PAYLOAD) ? "PAYLOAD" : "LABEL";
    size_t limit = (selectedField == FIELD_PAYLOAD) ? (PAYLOAD_LEN - 1) : (LABEL_LEN - 1);
    drawTextEditOverlay(fname, limit);
    return;
  }

  canvas.fillSprite(BLACK);

  canvas.fillRect(0, 0, 240, 14, MAROON);
  canvas.setTextColor(WHITE, MAROON);
  canvas.setCursor(4, 3);
  canvas.print("EDITOR [Arrows/Enter] 0=Exit P=Pair");

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
    canvas.printf("Lbl:%.12s", macros[selectedSlot].label);
    fy += 14;

    canvas.setTextColor(selectedField == FIELD_PAYLOAD ? BLACK : WHITE,
                        selectedField == FIELD_PAYLOAD ? ORANGE : BLACK);
    canvas.setCursor(124, fy);
    canvas.printf("Pay:%.16s", macros[selectedSlot].payload);
    fy += 18;

    canvas.setTextColor(LIGHTGREY, BLACK);
    canvas.setCursor(124, fy);
    if (selectedField == FIELD_TYPE) {
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
    canvas.setCursor(124, 44);
    canvas.print("P: pairing");

    char macStr[20];
    snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
             ownMac[0], ownMac[1], ownMac[2], ownMac[3], ownMac[4], ownMac[5]);
    canvas.setCursor(124, 58);
    canvas.print("My MAC:");
    canvas.setCursor(124, 68);
    canvas.print(macStr);

    int pairedCount = 0;
    for (int i = 0; i < MAX_PEERS; i++) if (peers[i].enabled) pairedCount++;
    canvas.setCursor(124, 82);
    canvas.printf("Paired: %d/%d", pairedCount, MAX_PEERS);
    canvas.setCursor(124, 92);
    canvas.printf("Battery: %d%%", batteryPct);
    canvas.setCursor(124, 106);
    canvas.print(linkReady ? "Link: ready" : "Link: NO RADIO");
  }

  // Key debug line (shows what the keyboard reports). Delete if not wanted.
  canvas.setTextColor(DARKGREY, BLACK);
  canvas.setCursor(4, 125);
  canvas.print(dbgLine);
}

void drawPairingUI() {
  canvas.fillSprite(BLACK);

  canvas.fillRect(0, 0, 240, 14, DARKGREEN);
  canvas.setTextColor(WHITE, DARKGREEN);
  canvas.setCursor(4, 3);
  canvas.print("PAIRING  0=Back");
  canvas.setCursor(168, 3);
  canvas.printf("B:%d%%", batteryPct);

  char macStr[20];
  snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
           ownMac[0], ownMac[1], ownMac[2], ownMac[3], ownMac[4], ownMac[5]);
  canvas.setTextColor(LIGHTGREY, BLACK);
  canvas.setCursor(4, 16);
  canvas.printf("My MAC: %s", macStr);

  int y = 28;
  for (int i = 0; i < MAX_PEERS; i++) {
    bool isSel  = (pairFocus == PFOCUS_LIST)  && (i == pairSel);
    bool isOpen = (pairFocus == PFOCUS_FIELD) && (i == pairSel);
    canvas.setCursor(4, y);
    if (isSel)       canvas.setTextColor(BLACK, YELLOW);
    else if (isOpen) canvas.setTextColor(BLACK, CYAN);
    else              canvas.setTextColor(peers[i].enabled ? WHITE : DARKGREY, BLACK);

    char macBuf[13];
    macToHexStr(peers[i].mac, macBuf);
    bool blank = (strcmp(macBuf, "000000000000") == 0);
    canvas.printf("%d:%-9.9s %s %s", i + 1,
                  peers[i].name[0] ? peers[i].name : "(unnamed)",
                  blank ? "------------" : macBuf,
                  peers[i].enabled ? "ON" : "off");
    y += 11;
  }

  if (pairFocus == PFOCUS_FIELD) {
    char selMacStr[13];
    macToHexStr(peers[pairSel].mac, selMacStr);

    canvas.drawRect(4, 76, 232, 44, DARKGREY);
    int fy = 80;

    canvas.setTextColor(pairField == PF_ENABLED ? BLACK : WHITE, pairField == PF_ENABLED ? ORANGE : BLACK);
    canvas.setCursor(8, fy);
    canvas.printf("Enabled: %s", peers[pairSel].enabled ? "ON" : "off");
    fy += 12;

    canvas.setTextColor(pairField == PF_NAME ? BLACK : WHITE, pairField == PF_NAME ? ORANGE : BLACK);
    canvas.setCursor(8, fy);
    if (textEditing && pairField == PF_NAME) canvas.printf("Name: %.16s_", editBuffer);
    else canvas.printf("Name: %.16s", peers[pairSel].name);
    fy += 12;

    canvas.setTextColor(pairField == PF_MAC ? BLACK : WHITE, pairField == PF_MAC ? ORANGE : BLACK);
    canvas.setCursor(8, fy);
    if (textEditing && pairField == PF_MAC) canvas.printf("MAC: %.12s_", editBuffer);
    else canvas.printf("MAC: %.12s", selMacStr);
  }

  canvas.setTextColor(LIGHTGREY, BLACK);
  canvas.setCursor(4, 124);
  if (textEditing) canvas.print("Type.. ENTER=save DEL=bksp");
  else canvas.print("Up/Dn select  Enter=open  Left/0=back");
}
