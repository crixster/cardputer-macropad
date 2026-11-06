/*
 * StickS3 IR Learner / Replayer  -  v6 (folders + labels + robust learning + HOLD mode)
 * -----------------------------------------------------------
 * Board   : M5Stack StickS3 (M5StickS3)  - ESP32-S3
 * IR TX   : GPIO46   IR RX : GPIO42
 * Needs   : M5Stack board package >= 3.2.5, M5Unified >= 0.2.12, M5GFX >= 0.2.18
 * Partition scheme: must include SPIFFS (e.g. "Default 8MB with spiffs")
 *
 * Structure: NUM_FOLDERS folders x SLOTS_PER_FOLDER slots (default 8 x 20 = 160)
 *
 * HOLD MODE (v6, per folder, default ON; serial: hold on|off)
 *   Click A to listen, then HOLD the remote button (or tap it 2-3 times). The stick
 *   collects up to 8 frames, finds the ones that agree with each other, averages those
 *   and saves the result ("Saved 5/6 match"). It finishes by itself after 1 s of IR
 *   silence, or click A to finish right away.
 *   - If no two frames agree it says "No agreement" and keeps listening.
 *   - If only ONE frame arrives (single tap, or NEC-style remotes that send only short
 *     repeat codes while held) it behaves like a normal press (verify on/off applies).
 *   - Keep idle 60 for AC folders. A held Sony remote sends frames only ~20 ms apart, so
 *     use idle 12 there, otherwise the frames merge into one long capture.
 *   - AC remotes usually send one state per press and change it on every press, so
 *     several taps may legitimately differ. Tap once and wait; it saves that frame.
 *
 * What's new in v5 (fixes for frequent "Mismatch" while learning)
 *   - Software glitch filter: tiny pulses/gaps (< MIN_ELEMENT_US) that the IR receiver
 *     module sometimes produces are merged away. Previously one extra blip shifted every
 *     later entry by one position and the whole compare failed.
 *   - Stray bursts / NEC repeat codes (< MIN_VALID_DURS entries) are ignored, and during
 *     the 2nd press a much shorter burst no longer replaces your good 1st capture.
 *   - Receiver is re-armed BEFORE serial printing / comparing, so the next part of a
 *     long AC signal or the 2nd press is never missed while we are busy.
 *   - Compare tolerance scales with signal length (long AC frames get a few % slack).
 *   - Serial prints why a mismatch happened (length difference, bad count, first bad index).
 *
 * Buttons
 *   Button A (front)  click : LEARN mode -> start listening; click again to finish a
 *                             hold capture (if frames collected) or cancel
 *                             SEND  mode -> transmit current slot
 *   Button A          hold  : toggle LEARN / SEND mode
 *   Button B (side)   click : next slot (wraps around)
 *   Button B          hold  : next folder
 *
 * Serial Monitor commands (115200 baud, send with newline):
 *   fname <text>  rename current folder      label <text>  label current slot
 *   del           delete current slot        delall        delete ALL slots in current folder
 *   list          show folder contents       help          show commands
 *   idle <ms>     RX end-of-capture gap (10-65) for current folder
 *   verify on|off require two matching presses for current folder
 *   hold on|off   hold mode (collect several frames, keep the agreeing ones)
 *   (Arduino IDE: Tools -> USB CDC On Boot = Enabled to use the Serial Monitor)
 *
 * TIP for air conditioners (e.g. Acson): select the AC folder, then
 *   idle 60  and  verify off
 * ACs send several frames with 20-40 ms gaps, and pressing a key twice often sends a
 * different state, so two-press verification does not make sense there.
 */

#include <M5Unified.h>
#include <LittleFS.h>
#include <Preferences.h>
#include "driver/rmt_tx.h"
#include "driver/rmt_rx.h"
#include "driver/rmt_encoder.h"

// ---------------- configuration ----------------
#define IR_TX_PIN          46
#define IR_RX_PIN          42
#define NUM_FOLDERS        8
#define SLOTS_PER_FOLDER   20
#define MAX_DURS           1200        // max mark/space entries stored per slot
#define CARRIER_HZ         38000       // change to 36000/40000/56000 if needed
#define CARRIER_DUTY       0.33f
#define MIN_VALID_DURS     12          // captures shorter than this are noise / NEC repeat codes
#define MIN_ELEMENT_US     120         // marks/spaces shorter than this are receiver glitches
#define MATCH_MIN_TOL_US   250         // absolute tolerance when comparing two presses
#define RX_RES_HZ          500000      // RX tick = 2 us (RX idle can go up to ~65 ms)
#define RX_TICK_US         2
#define MAX_IDLE_MS        65

// Hold mode (collects several frames while you hold / tap the remote button)
#define HOLD_MAX_FRAMES    8           // frames kept per hold capture
#define HOLD_QUIET_MS      1000        // finish after this much IR silence (once >= 1 frame)
#define HOLD_MAX_MS        4000        // ...or this long after the first frame

// results of the single-press decision logic
#define ACT_NOISE          0
#define ACT_WAIT           1
#define ACT_SAVE           2

// Per-folder defaults (change later with serial commands: idle <ms>, verify on|off)
//   idle   = silence that ends a capture. Short (30) for TV/audio, long (60) for
//            air conditioners, which send several frames with 20-40 ms gaps.
//   verify = require two matching presses (good for TVs; NOT for ACs, because
//            pressing an AC key twice usually sends two different states).
static const uint16_t DEFAULT_IDLE_MS[NUM_FOLDERS] = { 30, 60, 30, 30, 30, 30, 30, 30 };
static const bool     DEFAULT_VERIFY[NUM_FOLDERS]  = { true, false, true, true, true, true, true, true };

static const char *DEFAULT_FOLDERS[NUM_FOLDERS] = {
  "TV", "AC", "Audio", "Light", "Fan", "Projector", "Misc1", "Misc2"
};

// ---------------- globals ----------------
static rmt_channel_handle_t tx_chan = nullptr;
static rmt_channel_handle_t rx_chan = nullptr;
static rmt_encoder_handle_t copy_enc = nullptr;
static QueueHandle_t rx_queue = nullptr;

#define RX_BUF_SYMBOLS_MAX 1024
static rmt_symbol_word_t rx_buf[RX_BUF_SYMBOLS_MAX];
static size_t rx_buf_syms = RX_BUF_SYMBOLS_MAX;

static rmt_symbol_word_t tx_syms[MAX_DURS + 16];
static uint16_t durs[MAX_DURS];                          // working buffer
static uint16_t capBuf[MAX_DURS];                        // latest capture
static uint16_t firstBuf[MAX_DURS];                      // first capture awaiting confirmation
static uint16_t firstN = 0;
static int      stage = 0;                               // 0 = waiting 1st press, 1 = waiting 2nd
static String   rxStatus = "";
static uint16_t slotLen[NUM_FOLDERS][SLOTS_PER_FOLDER];  // 0 = empty

static Preferences prefs;
static String folderName[NUM_FOLDERS];
static uint16_t folderIdleMs[NUM_FOLDERS];
static bool     folderVerify[NUM_FOLDERS];
static bool     folderHold[NUM_FOLDERS];

// hold-mode frame collection
static uint16_t holdBuf[HOLD_MAX_FRAMES][MAX_DURS];
static uint16_t holdLen[HOLD_MAX_FRAMES];
static uint8_t  holdCount = 0;
static uint32_t holdFirstMs = 0;
static uint32_t holdLastMs = 0;

static int  curFolder = 0;
static int  curSlot = 0;
static bool learnMode = true;
static bool learning = false;
static String msg = "";
static String serialBuf = "";

// ---------------- storage ----------------
static String slotPath(int f, int s) {
  char b[24];
  snprintf(b, sizeof b, "/f%d_s%d.bin", f, s);
  return String(b);
}
static String labelKey(int f, int s) {
  char b[16];
  snprintf(b, sizeof b, "lb%d_%d", f, s);
  return String(b);
}
static String folderKey(int f) {
  char b[8];
  snprintf(b, sizeof b, "fn%d", f);
  return String(b);
}
static String idleKey(int f) { char b[8]; snprintf(b, sizeof b, "fi%d", f); return String(b); }
static String verifyKey(int f) { char b[8]; snprintf(b, sizeof b, "fv%d", f); return String(b); }
static String holdKey(int f) { char b[8]; snprintf(b, sizeof b, "fh%d", f); return String(b); }

static String getLabel(int f, int s) { return prefs.getString(labelKey(f, s).c_str(), ""); }

static bool saveSlot(int f, int s, const uint16_t *data, uint16_t n) {
  File file = LittleFS.open(slotPath(f, s), "w");
  if (!file) return false;
  file.write((uint8_t *)&n, 2);
  file.write((const uint8_t *)data, n * 2);
  file.close();
  slotLen[f][s] = n;
  return true;
}

// loads into global `durs`, returns count (0 on failure)
static uint16_t loadSlot(int f, int s) {
  File file = LittleFS.open(slotPath(f, s), "r");
  if (!file) return 0;
  uint16_t n = 0;
  if (file.read((uint8_t *)&n, 2) != 2 || n == 0 || n > MAX_DURS) { file.close(); return 0; }
  size_t got = file.read((uint8_t *)durs, n * 2);
  file.close();
  return (got == (size_t)(n * 2)) ? n : 0;
}

static void deleteSlot(int f, int s) {
  LittleFS.remove(slotPath(f, s));
  prefs.remove(labelKey(f, s).c_str());
  slotLen[f][s] = 0;
}

static void scanSlots() {
  for (int f = 0; f < NUM_FOLDERS; f++) {
    for (int s = 0; s < SLOTS_PER_FOLDER; s++) {
      slotLen[f][s] = 0;
      File file = LittleFS.open(slotPath(f, s), "r");
      if (!file) continue;
      uint16_t n = 0;
      if (file.read((uint8_t *)&n, 2) == 2 && n > 0 && n <= MAX_DURS) slotLen[f][s] = n;
      file.close();
    }
  }
}

// ---------------- UI ----------------
static String clip(const String &s, size_t n) { return s.length() > n ? s.substring(0, n) : s; }

static void drawUI() {
  auto &d = M5.Display;
  d.fillScreen(TFT_BLACK);
  d.setTextSize(2);

  // line 1: mode + folder
  d.setTextColor(learnMode ? TFT_ORANGE : TFT_GREEN);
  d.setCursor(4, 4);
  d.print(learnMode ? "LEARN" : "SEND");
  d.setTextColor(TFT_WHITE);
  d.setCursor(84, 4);
  d.printf("%d:%s", curFolder + 1, clip(folderName[curFolder], 10).c_str());

  // line 2: slot
  d.setTextColor(TFT_WHITE);
  d.setCursor(4, 32);
  d.printf("Slot %d/%d", curSlot + 1, SLOTS_PER_FOLDER);

  // line 3: label / empty
  d.setCursor(4, 58);
  if (slotLen[curFolder][curSlot]) {
    String lb = getLabel(curFolder, curSlot);
    d.setTextColor(TFT_CYAN);
    d.print(lb.length() ? clip(lb, 19).c_str() : "(no label)");
    d.setTextSize(1);
    d.setTextColor(TFT_DARKGREY);
    d.setCursor(4, 78);
    d.printf("%u pulses", (unsigned)slotLen[curFolder][curSlot]);
    d.setTextSize(2);
  } else {
    d.setTextColor(TFT_DARKGREY);
    d.print("(empty)");
  }

  // line 4: status
  d.setCursor(4, 88);
  d.setTextColor(learning ? TFT_RED : TFT_YELLOW);
  d.print(learning ? clip(rxStatus, 19).c_str() : clip(msg, 19).c_str());

  // hints
  d.setTextSize(1);
  d.setTextColor(TFT_DARKGREY);
  d.setCursor(4, 108);
  d.printf("idle %ums verify %s hold %s", (unsigned)folderIdleMs[curFolder],
           folderVerify[curFolder] ? "on" : "off", folderHold[curFolder] ? "on" : "off");
  d.setCursor(4, 120);
  d.print(learnMode ? "A:listen hA:mode B:slot hB:folder" : "A:send   hA:mode B:slot hB:folder");
}

// ---------------- RMT TX ----------------
static bool setupTx() {
  rmt_tx_channel_config_t t = {};
  t.gpio_num = (gpio_num_t)IR_TX_PIN;
  t.clk_src = RMT_CLK_SRC_DEFAULT;
  t.resolution_hz = 1000000;
  t.mem_block_symbols = 64;
  t.trans_queue_depth = 4;
  if (rmt_new_tx_channel(&t, &tx_chan) != ESP_OK) return false;

  rmt_carrier_config_t cc = {};
  cc.frequency_hz = CARRIER_HZ;
  cc.duty_cycle = CARRIER_DUTY;
  cc.flags.polarity_active_low = false;
  if (rmt_apply_carrier(tx_chan, &cc) != ESP_OK) return false;

  rmt_copy_encoder_config_t ec = {};
  if (rmt_new_copy_encoder(&ec, &copy_enc) != ESP_OK) return false;
  return rmt_enable(tx_chan) == ESP_OK;
}

static void sendSlot(int f, int s) {
  uint16_t n = loadSlot(f, s);
  if (!n) { msg = "Slot empty"; return; }

  size_t k = 0;
  const size_t kMax = sizeof(tx_syms) / sizeof(tx_syms[0]) - 2;
  for (uint16_t j = 0; j < n && k < kMax; j += 2) {
    uint32_t mark = durs[j];
    uint32_t space = (j + 1 < n) ? durs[j + 1] : 1000;
    if (mark < 1) mark = 1;
    if (mark > 32767) mark = 32767;
    if (space < 1) space = 1;

    uint32_t first = space > 32767 ? 32767 : space;
    rmt_symbol_word_t sym;
    sym.level0 = 1;
    sym.duration0 = mark;
    sym.level1 = 0;
    sym.duration1 = first;
    tx_syms[k++] = sym;

    uint32_t rest = space - first;   // gaps longer than 32.7 ms are split
    while (rest > 0 && k < kMax) {
      uint32_t part = rest > 32767 ? 32767 : rest;
      rmt_symbol_word_t gap;
      gap.level0 = 0;
      gap.duration0 = part;
      gap.level1 = 0;
      gap.duration1 = 1;
      tx_syms[k++] = gap;
      rest -= part;
    }
  }

  rmt_transmit_config_t tc = {};
  tc.loop_count = 0;
  if (rmt_transmit(tx_chan, copy_enc, tx_syms, k * sizeof(rmt_symbol_word_t), &tc) != ESP_OK) {
    msg = "TX error";
    return;
  }
  rmt_tx_wait_all_done(tx_chan, 3000);
  msg = "Sent!";
}

// ---------------- RMT RX ----------------
static bool IRAM_ATTR rxDoneCb(rmt_channel_handle_t ch, const rmt_rx_done_event_data_t *edata, void *user) {
  BaseType_t woken = pdFALSE;
  xQueueSendFromISR((QueueHandle_t)user, edata, &woken);
  return woken == pdTRUE;
}

static bool setupRx() {
  rx_queue = xQueueCreate(4, sizeof(rmt_rx_done_event_data_t));

  rmt_rx_channel_config_t c = {};
  c.gpio_num = (gpio_num_t)IR_RX_PIN;
  c.clk_src = RMT_CLK_SRC_DEFAULT;
  c.resolution_hz = RX_RES_HZ;
  c.mem_block_symbols = 1024;
  c.flags.with_dma = 1;
  rx_buf_syms = RX_BUF_SYMBOLS_MAX;

  esp_err_t e = rmt_new_rx_channel(&c, &rx_chan);
  if (e != ESP_OK) {
    Serial.printf("RX channel (DMA) failed: %s\n", esp_err_to_name(e));
    c.mem_block_symbols = 128;
    c.flags.with_dma = 0;
    rx_buf_syms = 128;
    e = rmt_new_rx_channel(&c, &rx_chan);
  }
  if (e != ESP_OK) {
    Serial.printf("RX channel (no DMA) failed: %s\n", esp_err_to_name(e));
    return false;
  }

  rmt_rx_event_callbacks_t cbs = {};
  cbs.on_recv_done = rxDoneCb;
  e = rmt_rx_register_event_callbacks(rx_chan, &cbs, rx_queue);
  if (e != ESP_OK) {
    Serial.printf("RX callbacks failed: %s\n", esp_err_to_name(e));
    return false;
  }
  e = rmt_enable(rx_chan);
  if (e != ESP_OK) {
    Serial.printf("RX enable failed: %s\n", esp_err_to_name(e));
    return false;
  }
  return true;
}

static bool armReceive() {
  rmt_receive_config_t rc = {};
  rc.signal_range_min_ns = 1250;
  rc.signal_range_max_ns = (uint32_t)folderIdleMs[curFolder] * 1000000UL;
  return rmt_receive(rx_chan, rx_buf, rx_buf_syms * sizeof(rmt_symbol_word_t), &rc) == ESP_OK;
}

static void startLearning() {
  xQueueReset(rx_queue);
  stage = 0;
  firstN = 0;
  holdCount = 0;
  rxStatus = folderHold[curFolder] ? "Hold/tap remote" : "Listening...";
  if (armReceive()) { learning = true; msg = ""; }
  else msg = "RX arm failed";
}

static void stopReceiver() {
  rmt_disable(rx_chan);
  rmt_enable(rx_chan);
  xQueueReset(rx_queue);
}

static void cancelLearning() {
  if (!learning) return;
  stopReceiver();
  learning = false;
  holdCount = 0;
}

// IR receiver modules are active-LOW: level 0 == mark.
static uint16_t symbolsToDurations(const rmt_symbol_word_t *s, size_t count, uint16_t *out) {
  uint16_t n = 0;
  int lastLevel = -1;
  uint32_t acc = 0;
  bool started = false;

  for (size_t i = 0; i < count; i++) {
    for (int half = 0; half < 2; half++) {
      int lvl = half ? s[i].level1 : s[i].level0;
      uint32_t raw = half ? s[i].duration1 : s[i].duration0;
      if (raw == 0) continue;
      uint32_t dur = raw * RX_TICK_US;
      if (!started) {
        if (lvl != 0) continue;
        started = true;
      }
      if (lvl == lastLevel) {
        acc += dur;
      } else {
        if (lastLevel != -1) {
          if (n >= MAX_DURS) return n;
          out[n++] = acc > 65535 ? 65535 : acc;
        }
        lastLevel = lvl;
        acc = dur;
      }
    }
  }
  if (lastLevel != -1 && n < MAX_DURS) out[n++] = acc > 65535 ? 65535 : acc;
  return n;
}

// Removes receiver glitches. Real IR protocols have no mark/space shorter than ~150 us,
// so anything below MIN_ELEMENT_US is an artefact. A glitch inside the signal is merged
// with its two neighbours (e.g. space 800 + mark 60 + space 830 -> space 1690), which keeps
// the mark/space alternation intact. A glitch at the very start/end is simply dropped.
static uint16_t cleanGlitches(uint16_t *a, uint16_t n) {
  uint16_t i = 0;
  while (i < n) {
    if (a[i] >= MIN_ELEMENT_US) { i++; continue; }
    if (i == 0) {                         // blip before the real signal: drop it + the gap after it
      uint16_t drop = n >= 2 ? 2 : 1;
      memmove(a, a + drop, (n - drop) * 2);
      n -= drop;
    } else if (i == n - 1) {              // blip at the very end
      n--;
    } else {                              // blip inside: merge with neighbours
      uint32_t merged = (uint32_t)a[i - 1] + a[i] + a[i + 1];
      a[i - 1] = merged > 65535 ? 65535 : merged;
      memmove(a + i, a + i + 2, (n - i - 2) * 2);
      n -= 2;
    }
  }
  return n;
}

static bool closeEnough(uint16_t a, uint16_t b) {
  uint32_t big = a > b ? a : b;
  uint32_t diff = a > b ? a - b : b - a;
  uint32_t tol = big / 4;
  if (tol < MATCH_MIN_TOL_US) tol = MATCH_MIN_TOL_US;
  return diff <= tol;
}

// Two presses match if length and number of off-spec entries are within ~3% (minimum 2).
// Reports how many entries were bad and where the first one was, for debugging.
static bool capturesMatch(const uint16_t *a, uint16_t na, const uint16_t *b, uint16_t nb,
                          uint16_t *badOut, int *firstBadOut) {
  uint16_t lenDiff = na > nb ? na - nb : nb - na;
  uint16_t n = na < nb ? na : nb;
  uint16_t allowed = n / 32;
  if (allowed < 2) allowed = 2;
  uint16_t bad = 0;
  int firstBad = -1;
  for (uint16_t i = 0; i < n; i++) {
    if (!closeEnough(a[i], b[i])) {
      if (firstBad < 0) firstBad = i;
      bad++;
    }
  }
  *badOut = bad;
  *firstBadOut = firstBad;
  return lenDiff <= allowed && bad <= allowed;
}

static void finishSave(const uint16_t *data, uint16_t n, const char *okMsg) {
  learning = false;
  stage = 0;
  holdCount = 0;
  if (saveSlot(curFolder, curSlot, data, n)) {
    if (okMsg) msg = okMsg;
    else msg = folderVerify[curFolder] ? "Saved (verified)" : "Saved!";
    Serial.printf("Folder %d slot %d saved, %u entries\n", curFolder + 1, curSlot + 1, n);
  } else {
    msg = "Save failed";
  }
}

// ---- normal (single press) logic: verify on = two matching presses, verify off = save at once ----
// The new frame is in capBuf (n entries). Returns ACT_*; fills saveN/average for ACT_SAVE and
// `note` with a mismatch explanation when relevant. Sets rxStatus for the screen.
static int decideSingle(uint16_t n, uint16_t *saveN, bool *average, char *note, size_t noteSz) {
  if (n < MIN_VALID_DURS) {
    rxStatus = "Noise, retry";
    return ACT_NOISE;
  }
  if (!folderVerify[curFolder]) {
    *saveN = n;
    return ACT_SAVE;
  }
  if (stage == 0) {
    memcpy(firstBuf, capBuf, n * 2);
    firstN = n;
    stage = 1;
    rxStatus = "Again (" + String(n) + ")";
    return ACT_WAIT;
  }
  if ((uint32_t)n * 2 < firstN) {
    // far shorter than the 1st press: stray burst / repeat code, keep the good 1st capture
    rxStatus = "Noise, retry";
    return ACT_NOISE;
  }
  uint16_t bad = 0;
  int firstBad = -1;
  if (capturesMatch(firstBuf, firstN, capBuf, n, &bad, &firstBad)) {
    *saveN = n < firstN ? n : firstN;
    *average = true;
    return ACT_SAVE;
  }
  snprintf(note, noteSz, "MISMATCH: len %u vs %u, %u bad, first at idx %d",
           (unsigned)firstN, (unsigned)n, (unsigned)bad, firstBad);
  memcpy(firstBuf, capBuf, n * 2);
  rxStatus = "Mismatch " + String(firstN) + "/" + String(n);
  firstN = n;
  return ACT_WAIT;
}

static void saveDecided(uint16_t saveN, bool average) {
  if (average) {
    for (uint16_t i = 0; i < saveN; i++) capBuf[i] = ((uint32_t)firstBuf[i] + capBuf[i]) / 2;
  }
  finishSave(capBuf, saveN, nullptr);
}

static void printCapture(size_t symbols, uint16_t rawN, uint16_t n) {
  Serial.printf("RX frame: %u symbols -> %u entries -> %u after glitch filter\n",
                (unsigned)symbols, (unsigned)rawN, (unsigned)n);
  for (uint16_t i = 0; i < n; i++) Serial.printf("%u%s", capBuf[i], (i + 1 < n) ? "," : "\n");
}

// ---- hold mode ----
// Finds the frame that agrees with the most other frames, averages that group into capBuf.
// Returns false when no two frames agree.
static bool pickBestFrames(uint16_t *outN, uint8_t *agreeOut) {
  int best = -1;
  int bestCnt = 0;
  uint32_t bestBad = 0xFFFFFFFFUL;
  for (int i = 0; i < holdCount; i++) {
    int cnt = 0;
    uint32_t totBad = 0;
    for (int j = 0; j < holdCount; j++) {
      if (i == j) continue;
      uint16_t bad = 0;
      int fb = -1;
      if (capturesMatch(holdBuf[i], holdLen[i], holdBuf[j], holdLen[j], &bad, &fb)) {
        cnt++;
        totBad += bad;
      }
    }
    if (cnt > bestCnt || (cnt == bestCnt && cnt > 0 && totBad < bestBad)) {
      best = i;
      bestCnt = cnt;
      bestBad = totBad;
    }
  }
  if (best < 0 || bestCnt < 1) return false;

  bool inSet[HOLD_MAX_FRAMES];
  uint16_t m = holdLen[best];
  int members = 1;
  for (int j = 0; j < holdCount; j++) {
    inSet[j] = false;
    if (j == best) { inSet[j] = true; continue; }
    uint16_t bad = 0;
    int fb = -1;
    if (capturesMatch(holdBuf[best], holdLen[best], holdBuf[j], holdLen[j], &bad, &fb)) {
      inSet[j] = true;
      members++;
      if (holdLen[j] < m) m = holdLen[j];
    }
  }
  for (uint16_t k = 0; k < m; k++) {
    uint32_t sum = 0;
    for (int j = 0; j < holdCount; j++) {
      if (inSet[j]) sum += holdBuf[j][k];
    }
    capBuf[k] = sum / members;
  }
  *outN = m;
  *agreeOut = members;
  return true;
}

static void holdFinish() {
  stopReceiver();
  uint8_t total = holdCount;
  Serial.printf("Hold capture finished: %u frame(s) collected\n", (unsigned)total);
  if (total == 0) {
    if (!armReceive()) { learning = false; msg = "RX arm failed"; }
    return;
  }

  if (total == 1) {
    // only one frame (single tap, or NEC-style repeat codes): same path as a normal press
    uint16_t n = holdLen[0];
    memcpy(capBuf, holdBuf[0], n * 2);
    holdCount = 0;
    uint16_t saveN = 0;
    bool average = false;
    char note[64] = "";
    int action = decideSingle(n, &saveN, &average, note, sizeof note);
    if (note[0]) Serial.println(note);
    if (action == ACT_SAVE) saveDecided(saveN, average);
    else if (!armReceive()) { learning = false; msg = "RX arm failed"; }
    return;
  }

  uint16_t n = 0;
  uint8_t agree = 0;
  if (pickBestFrames(&n, &agree)) {
    Serial.printf("%u of %u frames agree -> saving %u entries\n", (unsigned)agree, (unsigned)total, (unsigned)n);
    char ok[24];
    snprintf(ok, sizeof ok, "Saved %u/%u match", (unsigned)agree, (unsigned)total);
    finishSave(capBuf, n, ok);
  } else {
    Serial.print("No two frames agree. Lengths:");
    for (int i = 0; i < total; i++) Serial.printf(" %u", (unsigned)holdLen[i]);
    Serial.println();
    holdCount = 0;
    rxStatus = "No agreement";
    if (!armReceive()) { learning = false; msg = "RX arm failed"; }
  }
}

// One RX event while in hold mode: store it (if it is a real frame) and keep listening.
static void handleHoldFrame(size_t symbols, uint16_t rawN, uint16_t n) {
  bool stored = false;
  if (n >= MIN_VALID_DURS && holdCount < HOLD_MAX_FRAMES) {
    uint16_t longest = 0;
    for (int i = 0; i < holdCount; i++) {
      if (holdLen[i] > longest) longest = holdLen[i];
    }
    // ignore bursts far shorter than what we already have (repeat codes, noise)
    if ((uint32_t)n * 2 >= longest) {
      if (holdCount == 0) holdFirstMs = millis();
      memcpy(holdBuf[holdCount], capBuf, n * 2);
      holdLen[holdCount] = n;
      holdCount++;
      stored = true;
    }
  }
  if (holdCount > 0) {
    holdLastMs = millis();   // any IR activity (frame or short repeat) = button still active
    rxStatus = "Got " + String(holdCount) + " - A: save";
  } else {
    rxStatus = "Noise, retry";
  }

  // re-arm first, print afterwards
  if (!armReceive()) { learning = false; msg = "RX arm failed"; }
  printCapture(symbols, rawN, n);
  Serial.printf("hold: %s (%u collected)\n", stored ? "frame stored" : "ignored", (unsigned)holdCount);
}

static void handleCapture(const rmt_rx_done_event_data_t &ev) {
  // convert + clean (this copies everything out of rx_buf, so it is free to reuse)
  uint16_t rawN = symbolsToDurations(ev.received_symbols, ev.num_symbols, capBuf);
  uint16_t n = cleanGlitches(capBuf, rawN);

  if (folderHold[curFolder]) {
    handleHoldFrame(ev.num_symbols, rawN, n);
    return;
  }

  uint16_t saveN = 0;
  bool average = false;
  char note[64] = "";
  int action = decideSingle(n, &saveN, &average, note, sizeof note);

  // re-arm FIRST, so the next frame (AC frame #2, 2nd press, ...) is not missed
  // while we print to serial / write flash below
  if (action != ACT_SAVE) {
    if (!armReceive()) { learning = false; msg = "RX arm failed"; }
  }

  printCapture(ev.num_symbols, rawN, n);
  if (note[0]) Serial.println(note);

  if (action == ACT_SAVE) saveDecided(saveN, average);
}

// ---------------- serial commands ----------------
static void printHelp() {
  Serial.println("Commands: fname <text> | label <text> | del | delall | list | idle <10-65> | verify on|off | hold on|off | help");
}

static void handleSerialLine(String line) {
  line.trim();
  if (!line.length()) return;
  String cmd = line, arg = "";
  int sp = line.indexOf(' ');
  if (sp > 0) { cmd = line.substring(0, sp); arg = line.substring(sp + 1); arg.trim(); }
  cmd.toLowerCase();

  if (cmd == "fname" && arg.length()) {
    arg = arg.substring(0, 12);
    folderName[curFolder] = arg;
    prefs.putString(folderKey(curFolder).c_str(), arg);
    msg = "Folder renamed";
    Serial.printf("Folder %d is now '%s'\n", curFolder + 1, arg.c_str());
  } else if (cmd == "label" && arg.length()) {
    arg = arg.substring(0, 20);
    prefs.putString(labelKey(curFolder, curSlot).c_str(), arg);
    msg = "Label set";
    Serial.printf("Folder %d slot %d label '%s'\n", curFolder + 1, curSlot + 1, arg.c_str());
  } else if (cmd == "idle" && arg.length()) {
    int v = arg.toInt();
    if (v < 10) v = 10;
    if (v > MAX_IDLE_MS) v = MAX_IDLE_MS;
    folderIdleMs[curFolder] = v;
    prefs.putUShort(idleKey(curFolder).c_str(), v);
    msg = "Idle set";
    Serial.printf("Folder %d RX idle = %d ms\n", curFolder + 1, v);
  } else if (cmd == "verify" && arg.length()) {
    arg.toLowerCase();
    folderVerify[curFolder] = (arg == "on" || arg == "1");
    prefs.putBool(verifyKey(curFolder).c_str(), folderVerify[curFolder]);
    msg = folderVerify[curFolder] ? "Verify on" : "Verify off";
  } else if (cmd == "hold" && arg.length()) {
    cancelLearning();
    arg.toLowerCase();
    folderHold[curFolder] = (arg == "on" || arg == "1");
    prefs.putBool(holdKey(curFolder).c_str(), folderHold[curFolder]);
    msg = folderHold[curFolder] ? "Hold on" : "Hold off";
    Serial.printf("Folder %d hold mode %s\n", curFolder + 1, folderHold[curFolder] ? "on" : "off");
  } else if (cmd == "del") {
    cancelLearning();
    deleteSlot(curFolder, curSlot);
    msg = "Deleted";
  } else if (cmd == "delall") {
    cancelLearning();
    for (int s = 0; s < SLOTS_PER_FOLDER; s++) deleteSlot(curFolder, s);
    msg = "Folder cleared";
  } else if (cmd == "list") {
    Serial.printf("Folder %d: %s (idle %u ms, verify %s, hold %s)\n", curFolder + 1, folderName[curFolder].c_str(),
                  (unsigned)folderIdleMs[curFolder], folderVerify[curFolder] ? "on" : "off",
                  folderHold[curFolder] ? "on" : "off");
    for (int s = 0; s < SLOTS_PER_FOLDER; s++) {
      if (slotLen[curFolder][s])
        Serial.printf("  %2d: %s (%u pulses)\n", s + 1, getLabel(curFolder, s).c_str(), slotLen[curFolder][s]);
    }
  } else {
    printHelp();
  }
  drawUI();
}

// ---------------- Arduino ----------------
static void fatal(const char *text) {
  M5.Display.fillScreen(TFT_BLACK);
  M5.Display.setTextColor(TFT_RED);
  M5.Display.setTextSize(2);
  M5.Display.setCursor(4, 20);
  M5.Display.print(text);
  while (true) delay(1000);
}

void setup() {
  auto cfg = M5.config();
  cfg.internal_spk = false;  // speaker amp must be OFF for IR RX
  cfg.internal_mic = false;
  M5.begin(cfg);
  Serial.begin(115200);

  M5.Display.setRotation(3);
  M5.Display.setFont(&fonts::Font0);

  M5.Power.setExtOutput(true, m5::ext_none);
  delay(100);

  if (!LittleFS.begin(true)) fatal("No filesystem!\nUse a partition\nscheme w/ SPIFFS");

  prefs.begin("irl", false);
  for (int f = 0; f < NUM_FOLDERS; f++) {
    folderName[f] = prefs.getString(folderKey(f).c_str(), DEFAULT_FOLDERS[f]);
    folderIdleMs[f] = prefs.getUShort(idleKey(f).c_str(), DEFAULT_IDLE_MS[f]);
    if (folderIdleMs[f] > MAX_IDLE_MS) folderIdleMs[f] = MAX_IDLE_MS;
    if (folderIdleMs[f] < 10) folderIdleMs[f] = 10;
    folderVerify[f] = prefs.getBool(verifyKey(f).c_str(), DEFAULT_VERIFY[f]);
    folderHold[f] = prefs.getBool(holdKey(f).c_str(), true);
  }

  scanSlots();

  if (!setupTx()) fatal("TX init failed");
  if (!setupRx()) fatal("RX init failed");

  msg = "Ready v6";
  drawUI();
  printHelp();
}

void loop() {
  M5.update();
  bool redraw = false;

  // Button B: next slot / next folder
  if (M5.BtnB.wasHold()) {
    cancelLearning();
    curFolder = (curFolder + 1) % NUM_FOLDERS;
    curSlot = 0;
    msg = "";
    redraw = true;
  } else if (M5.BtnB.wasClicked()) {
    cancelLearning();
    curSlot = (curSlot + 1) % SLOTS_PER_FOLDER;
    msg = "";
    redraw = true;
  }

  // Button A: action / toggle mode
  if (M5.BtnA.wasHold()) {
    cancelLearning();
    learnMode = !learnMode;
    msg = "";
    redraw = true;
  } else if (M5.BtnA.wasClicked()) {
    if (learnMode) {
      if (learning) {
        if (folderHold[curFolder] && holdCount > 0) holdFinish();   // finish hold capture now
        else { cancelLearning(); msg = "Cancelled"; }
      } else {
        startLearning();
      }
    } else {
      sendSlot(curFolder, curSlot);
    }
    redraw = true;
  }

  // captured something?
  if (learning) {
    rmt_rx_done_event_data_t ev;
    if (xQueueReceive(rx_queue, &ev, 0) == pdTRUE) {
      handleCapture(ev);
      redraw = true;
    }
  }

  // hold mode: finish when enough frames, IR went quiet, or the time limit is hit
  if (learning && folderHold[curFolder] && holdCount > 0) {
    uint32_t now = millis();
    if (holdCount >= HOLD_MAX_FRAMES || now - holdLastMs > HOLD_QUIET_MS || now - holdFirstMs > HOLD_MAX_MS) {
      holdFinish();
      redraw = true;
    }
  }

  // serial commands
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (serialBuf.length()) { handleSerialLine(serialBuf); serialBuf = ""; }
    } else if (serialBuf.length() < 64) {
      serialBuf += c;
    }
  }

  if (redraw) drawUI();
  delay(5);
}
