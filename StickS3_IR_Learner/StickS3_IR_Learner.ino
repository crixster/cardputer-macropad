/*
 * StickS3 IR Learner / Replayer
 * -----------------------------------------------------------
 * Board   : M5Stack StickS3 (M5StickS3)  - ESP32-S3
 * IR TX   : GPIO46   IR RX : GPIO42   (per M5Stack docs)
 * Needs   : M5Stack board package >= 3.2.5, M5Unified >= 0.2.12, M5GFX >= 0.2.18
 * Partition scheme: any 8MB scheme that includes SPIFFS (e.g. "Default 8MB with spiffs")
 *
 * Controls
 *   Button A (front)  click : LEARN mode -> start/cancel listening
 *                             SEND  mode -> transmit stored signal of current slot
 *   Button A          hold  : delete current slot
 *   Button B (side)   click : next slot
 *   Button B          hold  : toggle LEARN / SEND mode
 *
 * Signals are stored as raw mark/space timings (works with almost any
 * protocol: NEC, Sony, RC5, Samsung, many TV/AC single-frame commands...)
 * and are persisted to LittleFS so they survive power-off.
 */

#include <M5Unified.h>
#include <LittleFS.h>
#include "driver/rmt_tx.h"
#include "driver/rmt_rx.h"
#include "driver/rmt_encoder.h"

// ---------------- configuration ----------------
#define IR_TX_PIN        46
#define IR_RX_PIN        42
#define NUM_SLOTS        10
#define MAX_DURS         1200        // max mark/space entries stored per slot
#define CARRIER_HZ       38000       // change to 36000/40000/56000 if your device needs it
#define CARRIER_DUTY     0.33f
#define MIN_VALID_DURS   6           // ignore captures shorter than this (noise)
#define RX_IDLE_NS       30000000    // 30 ms gap = end of frame

// ---------------- globals ----------------
static rmt_channel_handle_t tx_chan = nullptr;
static rmt_channel_handle_t rx_chan = nullptr;
static rmt_encoder_handle_t copy_enc = nullptr;
static QueueHandle_t rx_queue = nullptr;

#define RX_BUF_SYMBOLS_MAX 1024
static rmt_symbol_word_t rx_buf[RX_BUF_SYMBOLS_MAX];
static size_t rx_buf_syms = RX_BUF_SYMBOLS_MAX;

static rmt_symbol_word_t tx_syms[MAX_DURS / 2 + 2];

static uint16_t slots[NUM_SLOTS][MAX_DURS];
static uint16_t slotLen[NUM_SLOTS];

static int  curSlot = 0;
static bool learnMode = true;
static bool learning = false;
static bool fsOK = false;
static String msg = "";

// ---------------- storage ----------------
static String slotPath(int i) { return "/slot" + String(i) + ".bin"; }

static void saveSlot(int i) {
  if (!fsOK) return;
  File f = LittleFS.open(slotPath(i), "w");
  if (!f) return;
  f.write((uint8_t *)&slotLen[i], 2);
  f.write((uint8_t *)slots[i], slotLen[i] * 2);
  f.close();
}

static void deleteSlot(int i) {
  slotLen[i] = 0;
  if (fsOK) LittleFS.remove(slotPath(i));
}

static void loadAllSlots() {
  for (int i = 0; i < NUM_SLOTS; i++) {
    slotLen[i] = 0;
    if (!fsOK) continue;
    File f = LittleFS.open(slotPath(i), "r");
    if (!f) continue;
    uint16_t n = 0;
    if (f.read((uint8_t *)&n, 2) == 2 && n > 0 && n <= MAX_DURS) {
      if (f.read((uint8_t *)slots[i], n * 2) == (size_t)(n * 2)) slotLen[i] = n;
    }
    f.close();
  }
}

// ---------------- UI ----------------
static void drawUI() {
  auto &d = M5.Display;
  d.fillScreen(TFT_BLACK);

  d.setTextSize(2);
  d.setTextColor(learnMode ? TFT_ORANGE : TFT_GREEN);
  d.setCursor(4, 6);
  d.print(learnMode ? "LEARN" : "SEND");

  d.setTextColor(TFT_WHITE);
  d.setCursor(110, 6);
  d.printf("Slot %d/%d", curSlot + 1, NUM_SLOTS);

  d.setCursor(4, 38);
  if (slotLen[curSlot]) {
    d.setTextColor(TFT_CYAN);
    d.printf("%u pulses", (unsigned)slotLen[curSlot]);
  } else {
    d.setTextColor(TFT_DARKGREY);
    d.print("(empty)");
  }

  d.setCursor(4, 68);
  d.setTextColor(learning ? TFT_RED : TFT_YELLOW);
  d.print(learning ? "Listening..." : msg);

  d.setTextSize(1);
  d.setTextColor(TFT_DARKGREY);
  d.setCursor(4, 102);
  d.print(learnMode ? "A: listen/cancel   B: next slot" : "A: send   B: next slot");
  d.setCursor(4, 116);
  d.print("hold A: delete   hold B: mode");
  if (!fsOK) {
    d.setTextColor(TFT_RED);
    d.setCursor(4, 90);
    d.print("No FS: signals not persistent!");
  }
}

// ---------------- RMT TX ----------------
static bool setupTx() {
  rmt_tx_channel_config_t t = {};
  t.gpio_num = (gpio_num_t)IR_TX_PIN;
  t.clk_src = RMT_CLK_SRC_DEFAULT;
  t.resolution_hz = 1000000;  // 1 tick = 1 us
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

static void sendSlot(int i) {
  uint16_t n = slotLen[i];
  if (!n) { msg = "Slot empty"; return; }

  size_t k = 0;
  for (uint16_t j = 0; j < n; j += 2) {
    uint32_t mark = slots[i][j];
    uint32_t space = (j + 1 < n) ? slots[i][j + 1] : 1000;
    if (mark < 1) mark = 1;
    if (mark > 32767) mark = 32767;
    if (space < 1) space = 1;
    if (space > 32767) space = 32767;

    rmt_symbol_word_t s;
    s.level0 = 1;               // carrier ON  (mark)
    s.duration0 = mark;
    s.level1 = 0;               // carrier OFF (space)
    s.duration1 = space;
    tx_syms[k++] = s;
  }

  rmt_transmit_config_t tc = {};
  tc.loop_count = 0;
  esp_err_t e = rmt_transmit(tx_chan, copy_enc, tx_syms, k * sizeof(rmt_symbol_word_t), &tc);
  if (e != ESP_OK) { msg = "TX error"; return; }
  rmt_tx_wait_all_done(tx_chan, 2000);
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
  c.resolution_hz = 1000000;
  c.mem_block_symbols = 1024;
  c.flags.with_dma = 1;
  rx_buf_syms = RX_BUF_SYMBOLS_MAX;

  esp_err_t e = rmt_new_rx_channel(&c, &rx_chan);
  if (e != ESP_OK) {  // fall back to non-DMA (shorter captures)
    c.mem_block_symbols = 128;
    c.flags.with_dma = 0;
    rx_buf_syms = 128;
    e = rmt_new_rx_channel(&c, &rx_chan);
  }
  if (e != ESP_OK) return false;

  rmt_rx_event_callbacks_t cbs = {};
  cbs.on_recv_done = rxDoneCb;
  if (rmt_rx_register_event_callbacks(rx_chan, &cbs, rx_queue) != ESP_OK) return false;
  return rmt_enable(rx_chan) == ESP_OK;
}

static bool armReceive() {
  rmt_receive_config_t rc = {};
  rc.signal_range_min_ns = 1250;        // glitch filter
  rc.signal_range_max_ns = RX_IDLE_NS;  // frame-end gap
  return rmt_receive(rx_chan, rx_buf, rx_buf_syms * sizeof(rmt_symbol_word_t), &rc) == ESP_OK;
}

static void startLearning() {
  xQueueReset(rx_queue);
  if (armReceive()) {
    learning = true;
    msg = "";
  } else {
    msg = "RX arm failed";
  }
}

static void cancelLearning() {
  if (!learning) return;
  rmt_disable(rx_chan);   // aborts pending receive
  rmt_enable(rx_chan);
  xQueueReset(rx_queue);
  learning = false;
}

// Convert RMT symbols to alternating mark/space durations (mark first).
// IR receiver modules output active-LOW, so level 0 == mark.
static uint16_t symbolsToDurations(const rmt_symbol_word_t *s, size_t count, uint16_t *out) {
  uint16_t n = 0;
  int lastLevel = -1;
  uint32_t acc = 0;
  bool started = false;

  for (size_t i = 0; i < count; i++) {
    for (int half = 0; half < 2; half++) {
      int lvl = half ? s[i].level1 : s[i].level0;
      uint32_t dur = half ? s[i].duration1 : s[i].duration0;
      if (dur == 0) continue;
      if (!started) {
        if (lvl != 0) continue;  // skip leading idle
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

static void handleCapture(const rmt_rx_done_event_data_t &ev) {
  static uint16_t tmp[MAX_DURS];
  uint16_t n = symbolsToDurations(ev.received_symbols, ev.num_symbols, tmp);

  if (n < MIN_VALID_DURS) {
    msg = "Noise, retry";
    startLearning();
    return;
  }

  memcpy(slots[curSlot], tmp, n * 2);
  slotLen[curSlot] = n;
  saveSlot(curSlot);
  learning = false;
  msg = "Saved!";

  Serial.printf("Slot %d captured, %u entries:\n", curSlot, n);
  for (uint16_t i = 0; i < n; i++) Serial.printf("%u%s", tmp[i], (i + 1 < n) ? "," : "\n");
}

// ---------------- Arduino ----------------
void setup() {
  auto cfg = M5.config();
  cfg.internal_spk = false;  // speaker amp must be OFF for IR RX to work
  cfg.internal_mic = false;
  M5.begin(cfg);
  Serial.begin(115200);

  M5.Display.setRotation(3);
  M5.Display.setFont(&fonts::Font0);

  // Keep power to IR TX/RX enabled
  M5.Power.setExtOutput(true, m5::ext_none);
  delay(100);

  fsOK = LittleFS.begin(true);
  loadAllSlots();

  if (!setupTx() || !setupRx()) {
    M5.Display.fillScreen(TFT_BLACK);
    M5.Display.setTextColor(TFT_RED);
    M5.Display.setTextSize(2);
    M5.Display.setCursor(4, 40);
    M5.Display.print("RMT init failed");
    while (true) delay(1000);
  }

  msg = "Ready";
  drawUI();
}

void loop() {
  M5.update();
  bool redraw = false;

  // Button B: next slot / toggle mode
  if (M5.BtnB.wasHold()) {
    cancelLearning();
    learnMode = !learnMode;
    msg = "";
    redraw = true;
  } else if (M5.BtnB.wasClicked()) {
    cancelLearning();
    curSlot = (curSlot + 1) % NUM_SLOTS;
    msg = "";
    redraw = true;
  }

  // Button A: action / delete
  if (M5.BtnA.wasHold()) {
    cancelLearning();
    deleteSlot(curSlot);
    msg = "Deleted";
    redraw = true;
  } else if (M5.BtnA.wasClicked()) {
    if (learnMode) {
      if (learning) { cancelLearning(); msg = "Cancelled"; }
      else startLearning();
    } else {
      sendSlot(curSlot);
    }
    redraw = true;
  }

  // Captured something?
  if (learning) {
    rmt_rx_done_event_data_t ev;
    if (xQueueReceive(rx_queue, &ev, 0) == pdTRUE) {
      handleCapture(ev);
      redraw = true;
    }
  }

  if (redraw) drawUI();
  delay(5);
}
