/*
  ============================================================================
  stick-remote.ino  --  M5StickS3 remote for the Cardputer MacroPad
  ============================================================================
  The stick is NOT a keyboard. It sends a slot number to the Cardputer over
  ESP-NOW, and the Cardputer (plugged into the PC via USB) types the macro.

  LOCKED TO ONE PEER: frames are accepted only from the paired Cardputer's
  MAC and are encrypted with a shared key.

  PAIRING (no typing required -- the stick only has 2 buttons)
    Hold BOTH Button A and Button B together for about 1.2s to enter
    "Pairing: listening..." mode. The Cardputer broadcasts its own MAC
    every few seconds; the stick grabs the first one it hears, saves it,
    and re-pairs automatically. On the Cardputer side, make sure this
    stick's MAC is added as one of its (up to 4) paired devices from its
    own Pairing screen -- press 'P' from its editor, open a slot, and type
    this stick's MAC (shown on this screen) in as 12 hex characters.

  CONTROLS
    Button A (front)  click  -> send the highlighted macro
    Button A          hold   -> make / un-make the highlighted macro a DEFAULT
    Button B (side)   click  -> next macro
    Button B          hold   -> re-sync the macro list from the Cardputer
    Button A + B      hold together (~1.2s) -> auto-pair: listen for the
                                                 Cardputer's MAC and save it

  LIST ORDER
    Defaults (marked *) are always at the top, in slot order, followed by all
    other macros in slot order. The highlight always starts on the first
    entry, and returns to it after every successful send, so your default is
    one click away. Empty slots are hidden. Default flags are saved on the
    stick; labels are cached so the list is there at boot.
  ============================================================================
*/

#include <M5Unified.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>

#define MAX_SLOTS    13
#define LABEL_LEN    13
#define VISIBLE_ROWS 5

// Fallback used only on first boot, before any pairing has happened. After
// that, the stored (possibly auto-paired) MAC in NVS always wins. You no
// longer need to recompile to re-pair -- hold A+B to auto-pair instead.
static const uint8_t DEFAULT_PEER_MAC[6] = { 0xB0, 0x81, 0x84, 0x96, 0xA7, 0x0C };
uint8_t peerMac[6];

// Shared keys -- MUST be identical on the Cardputer. Exactly 16 characters.
static const char LINK_PMK[17] = "MacroPadPMK_2026";
static const char LINK_LMK[17] = "MacroPadLMK_2026";

#define LINK_MAGIC 0x4D50524DUL
enum LinkType : uint8_t {
  MSG_TRIGGER   = 1,
  MSG_SYNC_REQ  = 2,
  MSG_SLOT_INFO = 3,
  MSG_ACK       = 4,
  MSG_ANNOUNCE  = 5   // unencrypted broadcast from the Cardputer: "here is my MAC"
};

// Keep this struct IDENTICAL in the Cardputer sketch.
struct __attribute__((packed)) LinkMsg {
  uint32_t magic;
  uint8_t  type;
  uint8_t  slot;
  uint8_t  flags;            // SLOT_INFO: bit0 = slot has a payload. ACK: bit0 = executed OK
  uint8_t  seq;
  char     key;
  char     label[LABEL_LEN];
};

struct SlotInfo {
  uint8_t valid;             // 1 = slot has a payload on the Cardputer
  char    key;
  char    label[LABEL_LEN];
};

SlotInfo slots[MAX_SLOTS];       // loop-owned copy
SlotInfo rxSlots[MAX_SLOTS];     // written by the WiFi callback
SlotInfo savedSlots[MAX_SLOTS];  // what is currently in flash
volatile bool     rxDirty = false;
volatile uint32_t lastRxMs = 0;
portMUX_TYPE      mux = portMUX_INITIALIZER_UNLOCKED;

volatile bool    ackGot = false;
volatile uint8_t ackSeq = 0;
volatile bool    ackOk  = false;

// Auto-pair capture
volatile bool    pairCaptured = false;
volatile uint8_t pendingPairMac[6];
bool     pairScanning = false;
uint32_t pairScanUntil = 0;
uint32_t comboPressStart = 0;

Preferences prefs;
M5Canvas    canvas(&M5.Display);

uint16_t defaultMask = 0;        // bit n = slot n is a default
int      order[MAX_SLOTS];
int      orderCount = 0;
int      sel = 0;

bool     linkReady = false;
uint8_t  ownMac[6] = {0};
uint8_t  txSeq = 0;
bool     awaiting = false;
uint8_t  awaitSeq = 0;
int      awaitSlot = 0;
uint32_t awaitSince = 0;
uint32_t lastSyncReq = 0;
bool     needSave = false;
bool     dirty = true;

// Battery
int      batteryPct   = -1;
uint32_t lastBattPoll = 0;

char     toastText[40] = "";
uint16_t toastColor = WHITE;
uint32_t toastUntil = 0;

void macToStr(const uint8_t *m, char *out);
bool peerMacConfigured();
void linkHandle(const uint8_t *mac, const uint8_t *data, int len);
void linkBegin();
void registerPeer();
void sendMsg(uint8_t type, uint8_t slot, uint8_t seq);
void requestSync();
void rebuildOrder(int keepSlot);
void setToast(const char *msg, uint16_t color);
void toggleDefault();
void sendSelected();
void startPairScan();
void draw();

void macToStr(const uint8_t *m, char *out) {
  snprintf(out, 20, "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
}

bool peerMacConfigured() {
  for (int i = 0; i < 6; i++) if (peerMac[i] != 0) return true;
  return false;
}

// Runs in the WiFi task: only copy data, no drawing / flash access.
void linkHandle(const uint8_t *mac, const uint8_t *data, int len) {
  if (len != (int)sizeof(LinkMsg)) return;
  LinkMsg m;
  memcpy(&m, data, sizeof(m));
  if (m.magic != LINK_MAGIC) return;

  if (m.type == MSG_ANNOUNCE) {
    // Announce frames are unencrypted broadcasts and never matched against
    // peerMac -- that's the whole point, so we can capture an unknown MAC.
    if (pairScanning) {
      memcpy((void *)pendingPairMac, mac, 6);
      pairCaptured = true;
    }
    return;
  }

  if (memcmp(mac, peerMac, 6) != 0) return;       // not our Cardputer -> drop

  if (m.type == MSG_SLOT_INFO && m.slot < MAX_SLOTS) {
    portENTER_CRITICAL(&mux);
    rxSlots[m.slot].valid = (m.flags & 1) ? 1 : 0;
    rxSlots[m.slot].key   = m.key;
    memcpy(rxSlots[m.slot].label, m.label, LABEL_LEN);
    rxSlots[m.slot].label[LABEL_LEN - 1] = '\0';
    rxDirty  = true;
    lastRxMs = millis();
    portEXIT_CRITICAL(&mux);
  } else if (m.type == MSG_ACK) {
    ackOk  = (m.flags & 1);
    ackSeq = m.seq;
    ackGot = true;
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

void registerPeer() {
  esp_now_del_peer(peerMac);   // ok if it wasn't registered yet
  if (!peerMacConfigured()) return;

  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, peerMac, 6);
  peer.channel = 1;
  peer.ifidx   = WIFI_IF_STA;
  peer.encrypt = true;
  memcpy(peer.lmk, LINK_LMK, 16);
  esp_now_add_peer(&peer);
}

void linkBegin() {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
  WiFi.macAddress(ownMac);

  if (esp_now_init() != ESP_OK) return;
  esp_now_set_pmk((const uint8_t *)LINK_PMK);

  registerPeer();
  // Recv callback is registered even with no peer configured yet, so a
  // brand-new stick can still hear announce broadcasts and auto-pair.
  esp_now_register_recv_cb(linkOnRecv);
  linkReady = true;
}

void sendMsg(uint8_t type, uint8_t slot, uint8_t seq) {
  if (!linkReady || !peerMacConfigured()) return;
  LinkMsg m;
  memset(&m, 0, sizeof(m));
  m.magic = LINK_MAGIC;
  m.type  = type;
  m.slot  = slot;
  m.seq   = seq;
  esp_now_send(peerMac, (const uint8_t *)&m, sizeof(m));
}

void requestSync() {
  sendMsg(MSG_SYNC_REQ, 0, 0);
  lastSyncReq = millis();
}

// Defaults first (slot order), then everything else (slot order).
// keepSlot >= 0 keeps the highlight on that slot after the reorder.
void rebuildOrder(int keepSlot) {
  orderCount = 0;
  for (int i = 0; i < MAX_SLOTS; i++) {
    if (slots[i].valid && ((defaultMask >> i) & 1)) order[orderCount++] = i;
  }
  for (int i = 0; i < MAX_SLOTS; i++) {
    if (slots[i].valid && !((defaultMask >> i) & 1)) order[orderCount++] = i;
  }
  if (keepSlot >= 0) {
    for (int i = 0; i < orderCount; i++) {
      if (order[i] == keepSlot) { sel = i; break; }
    }
  }
  if (sel >= orderCount) sel = orderCount > 0 ? orderCount - 1 : 0;
  dirty = true;
}

void setToast(const char *msg, uint16_t color) {
  snprintf(toastText, sizeof(toastText), "%s", msg);
  toastColor = color;
  toastUntil = millis() + 1800;
  dirty = true;
}

void toggleDefault() {
  if (orderCount == 0) return;
  int slot = order[sel];
  defaultMask ^= (uint16_t)(1u << slot);
  prefs.putUShort("def", defaultMask);
  rebuildOrder(slot);

  char msg[40];
  if ((defaultMask >> slot) & 1) snprintf(msg, sizeof(msg), "Default: %.12s", slots[slot].label);
  else                           snprintf(msg, sizeof(msg), "Default removed");
  setToast(msg, CYAN);
}

void sendSelected() {
  if (orderCount == 0) { setToast("No macros yet", ORANGE); return; }
  if (!peerMacConfigured()) { setToast("Not paired: hold A+B", RED); return; }
  if (!linkReady) { setToast("Radio not ready", RED); return; }
  if (awaiting) return;

  int slot = order[sel];
  txSeq++;
  awaitSeq   = txSeq;
  awaitSlot  = slot;
  awaitSince = millis();
  awaiting   = true;
  ackGot     = false;
  sendMsg(MSG_TRIGGER, (uint8_t)slot, awaitSeq);

  char msg[40];
  snprintf(msg, sizeof(msg), "Sending %.12s...", slots[slot].label);
  setToast(msg, YELLOW);
}

void startPairScan() {
  pairScanning  = true;
  pairScanUntil = millis() + 15000;
  pairCaptured  = false;
  setToast("Pairing: listening...", CYAN);
}

void draw() {
  int W = M5.Display.width();
  int H = M5.Display.height();
  canvas.fillSprite(BLACK);

  // Header
  canvas.fillRect(0, 0, W, 16, NAVY);
  canvas.setTextSize(1);
  canvas.setTextColor(WHITE, NAVY);
  canvas.setCursor(4, 4);
  canvas.printf("%d macros", orderCount);
  canvas.setCursor(W - 46, 4);
  canvas.printf("B:%d%%", batteryPct);

  if (pairScanning) {
    canvas.setTextColor(CYAN, BLACK);
    canvas.setCursor(4, 24);
    canvas.print("Pairing: listening...");
    canvas.setTextColor(LIGHTGREY, BLACK);
    canvas.setCursor(4, 38);
    int secsLeft = (int)((pairScanUntil - millis()) / 1000);
    if (secsLeft < 0) secsLeft = 0;
    canvas.printf("Waiting for Cardputer (%ds)", secsLeft);
    canvas.setCursor(4, 54);
    canvas.print("Make sure this MAC is added");
    canvas.setCursor(4, 66);
    canvas.print("on the Cardputer's Pairing");
    canvas.setCursor(4, 78);
    canvas.print("screen:");

    char me[20];
    macToStr(ownMac, me);
    canvas.setTextSize(2);
    canvas.setCursor(4, 92);
    canvas.print(me);
    canvas.setTextSize(1);
  } else if (orderCount == 0) {
    char me[20], peer[20];
    macToStr(ownMac, me);
    macToStr(peerMac, peer);

    canvas.setTextColor(linkReady ? WHITE : ORANGE, BLACK);
    canvas.setCursor(4, 24);
    canvas.print(linkReady ? "Waiting for Cardputer sync..." : "Radio not ready");
    canvas.setTextColor(LIGHTGREY, BLACK);
    canvas.setCursor(4, 38);
    canvas.print("Hold A+B = auto-pair");
    canvas.setCursor(4, 50);
    canvas.print("Hold B   = request sync");

    canvas.setTextColor(WHITE, BLACK);
    canvas.setCursor(4, 66);
    canvas.print("This stick's MAC:");
    canvas.setTextSize(2);
    canvas.setCursor(4, 78);
    canvas.print(me);

    canvas.setTextSize(1);
    canvas.setTextColor(DARKGREY, BLACK);
    canvas.setCursor(4, 104);
    canvas.printf("Paired to: %s", peerMacConfigured() ? peer : "(none)");
  } else {
    // Scrolling window around the selection
    int first = sel - 2;
    int maxFirst = orderCount - VISIBLE_ROWS;
    if (maxFirst < 0) maxFirst = 0;
    if (first > maxFirst) first = maxFirst;
    if (first < 0) first = 0;

    canvas.setTextSize(2);
    for (int r = 0; r < VISIBLE_ROWS; r++) {
      int idx = first + r;
      if (idx >= orderCount) break;
      int slot = order[idx];
      bool isSel = (idx == sel);
      bool isDef = (defaultMask >> slot) & 1;
      int y = 20 + r * 19;

      canvas.fillRect(0, y - 1, W, 19, isSel ? YELLOW : BLACK);
      canvas.setTextColor(isSel ? BLACK : (isDef ? CYAN : WHITE), isSel ? YELLOW : BLACK);
      canvas.setCursor(4, y);
      canvas.printf("%c[%c] %.12s", isDef ? '*' : ' ', toupper(slots[slot].key), slots[slot].label);
    }

    // Scroll hints
    if (first > 0)
      canvas.fillTriangle(W - 6, 21, W - 12, 28, W - 0, 28, LIGHTGREY);
    if (first + VISIBLE_ROWS < orderCount)
      canvas.fillTriangle(W - 6, 114, W - 12, 107, W - 0, 107, LIGHTGREY);
  }

  // Footer: toast or hint
  canvas.setTextSize(1);
  canvas.fillRect(0, H - 15, W, 15, BLACK);
  canvas.setCursor(4, H - 12);
  if (toastUntil && millis() < toastUntil) {
    canvas.setTextColor(toastColor, BLACK);
    canvas.print(toastText);
  } else {
    canvas.setTextColor(DARKGREY, BLACK);
    canvas.print("A:send B:next | hold A:default B:sync A+B:pair");
  }
}

void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);

  // Use whichever rotation is landscape (wider than tall).
  {
    const uint8_t tryOrder[4] = {1, 3, 0, 2};
    for (uint8_t r : tryOrder) {
      M5.Display.setRotation(r);
      if (M5.Display.width() > M5.Display.height()) break;
    }
  }
  M5.Display.setBrightness(80);

  canvas.setColorDepth(8);
  canvas.createSprite(M5.Display.width(), M5.Display.height());

  M5.BtnA.setHoldThresh(700);
  M5.BtnB.setHoldThresh(700);

  memset(slots, 0, sizeof(slots));
  prefs.begin("remote", false);
  defaultMask = prefs.getUShort("def", 0);
  if (prefs.getBytesLength("slots") == sizeof(slots)) {
    prefs.getBytes("slots", slots, sizeof(slots));
  }
  memcpy(rxSlots, slots, sizeof(slots));
  memcpy(savedSlots, slots, sizeof(slots));
  rebuildOrder(-1);
  sel = 0;

  // Load the paired Cardputer's MAC from flash, falling back to the
  // compiled-in default on first boot (no NVS entry yet).
  memcpy(peerMac, DEFAULT_PEER_MAC, 6);
  if (prefs.getBytesLength("peermac") == 6) {
    prefs.getBytes("peermac", peerMac, 6);
  }

  batteryPct   = M5.Power.getBatteryLevel();
  lastBattPoll = millis();

  linkBegin();
  requestSync();
}

void loop() {
  M5.update();
  uint32_t now = millis();

  if (now - lastBattPoll > 5000) {
    int lvl = M5.Power.getBatteryLevel();
    if (lvl != batteryPct) { batteryPct = lvl; dirty = true; }
    lastBattPoll = now;
  }

  // Merge macro list updates received from the Cardputer
  if (rxDirty) {
    portENTER_CRITICAL(&mux);
    memcpy(slots, rxSlots, sizeof(slots));
    rxDirty = false;
    portEXIT_CRITICAL(&mux);
    int cur = (orderCount > 0) ? order[sel] : -1;
    rebuildOrder(cur);
    needSave = true;
  }
  // Save to flash once the burst of updates has settled, only if changed
  if (needSave && (now - lastRxMs > 800)) {
    if (memcmp(slots, savedSlots, sizeof(slots)) != 0) {
      prefs.putBytes("slots", slots, sizeof(slots));
      memcpy(savedSlots, slots, sizeof(slots));
    }
    needSave = false;
  }

  // Auto-pair capture
  if (pairScanning) {
    if (pairCaptured) {
      pairCaptured = false;
      memcpy(peerMac, (const uint8_t *)pendingPairMac, 6);
      prefs.putBytes("peermac", peerMac, 6);
      registerPeer();
      pairScanning = false;
      setToast("Paired!", GREEN);
      requestSync();
    } else if (now > pairScanUntil) {
      pairScanning = false;
      setToast("Pairing timed out", ORANGE);
    }
  }

  // Detect A+B held together for the auto-pair combo.
  if (M5.BtnA.isPressed() && M5.BtnB.isPressed()) {
    if (comboPressStart == 0) {
      comboPressStart = now;
    } else if (!pairScanning && (now - comboPressStart > 1200)) {
      startPairScan();
      comboPressStart = 0;
    }
  } else {
    comboPressStart = 0;
  }

  // Buttons (ignored while a pairing scan is in progress, and the combo
  // above already consumes isPressed() for A+B so lone clicks/holds won't
  // double-fire once a scan has started).
  if (!pairScanning) {
    if (M5.BtnB.wasHold()) {
      requestSync();
      setToast("Syncing...", YELLOW);
    } else if (M5.BtnB.wasClicked()) {
      if (orderCount > 0) { sel = (sel + 1) % orderCount; dirty = true; }
    }

    if (M5.BtnA.wasHold()) {
      toggleDefault();
    } else if (M5.BtnA.wasClicked()) {
      sendSelected();
    }
  }

  // Wait for the Cardputer's acknowledgement
  if (awaiting) {
    if (ackGot && ackSeq == awaitSeq) {
      ackGot = false;
      awaiting = false;
      if (ackOk) {
        char msg[40];
        snprintf(msg, sizeof(msg), "Sent: %.12s", slots[awaitSlot].label);
        setToast(msg, GREEN);
        sel = 0;                       // back to the top / default
      } else {
        setToast("Cardputer busy (editor?)", ORANGE);
      }
    } else if (now - awaitSince > 1000) {
      awaiting = false;
      setToast("No reply from Cardputer", RED);
    }
  } else if (ackGot) {
    ackGot = false;
  }

  // Keep asking until the list arrives
  if (linkReady && peerMacConfigured() && orderCount == 0 && !pairScanning && (now - lastSyncReq > 3000)) {
    requestSync();
  }

  if (toastUntil && now >= toastUntil) {
    toastUntil = 0;
    dirty = true;
  }

  if (dirty || pairScanning) {
    draw();
    canvas.pushSprite(0, 0);
    dirty = false;
  }
  delay(5);
}
