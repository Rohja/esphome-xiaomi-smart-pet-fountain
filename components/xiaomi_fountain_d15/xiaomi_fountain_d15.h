#pragma once
// Xiaomi smart pet fountain XWWF01MG-EU (board EP-1158-EMC-D15) - control logic for ESPHome.
// Behaviour reverse-engineered from the stock firmware and validated against real logs:
//  - pump on GPIO13 (inverted: 0 = on), the same pin is read as an input to sense the pump state;
//  - after the drive phase GPIO13 is released and sampled every 10 ms; falling edges are counted per window;
//  - the last sampled level is kept across windows (like the stock firmware): this is what makes a lifted
//    lid read as exactly one falling edge.
#include <Arduino.h>
#include <cstdio>
#include "esphome/core/log.h"

namespace xiaomi_fountain_d15 {

static const char *const TAG = "fountain";

static const int PIN_PUMP = 13;     // pump, inverted logic: 0 = on, 1 = off. Also the sensing input.
static const int PIN_SIDE_L = 4;    // "+" commons of the left / right LED
static const int PIN_SIDE_R = 5;
// Colour channels (measured on this board): GPIO14 = white, GPIO15 = violet, GPIO16 = cold white
static const int PIN_LED14 = 14;
static const int PIN_LED15 = 15;
static const int PIN_LED16 = 16;
static const int PIN_CENTER = 3;    // central red "problem" LED, inverted (0 = on)

enum Mode : uint8_t { MODE_NORMAL = 0, MODE_SMART = 1, MODE_OFF = 2, MODE_UNSET = 255 };
enum Water : uint8_t { WATER_OK = 0, NO_WATER = 1, PUMP_BLOCK = 2, LID_UP = 3 };
enum Status : uint8_t { ST_OFF = 0, ST_MEASURING, ST_RUNNING, ST_PAUSED, ST_NO_WATER, ST_BLOCKED, ST_LID };
enum LedMode : uint8_t { LED_ON = 0, LED_OFF = 1, LED_TIMED = 2 };

inline const char *water_text(Water w) {
  switch (w) {
    case WATER_OK: return "water ok";
    case NO_WATER: return "no water";
    case PUMP_BLOCK: return "pump blocked";
    default: return "lid open";
  }
}

class Controller {
 public:
  // Diagnostic: read GPIO13 with the internal pull-up (the stock firmware does not use one)
  static constexpr bool SENSE_PULLUP = false;

  // ---- pump / sensing cadence (cadence "A" of the stock firmware, in 10 ms ticks)
  uint16_t drive_ticks = 100;          // 1 s of pump when water is OK
  uint16_t sense_ticks = 50;           // 0.5 s of sensing
  uint16_t probe_drive_ticks = 100;    // unknown state (start-up): 1 s of pump as well
  uint16_t spinup_ticks = 300;         // first drive after a cold start: 3 s, the pump needs time to spin up
  uint8_t warmup_windows = 3;          // windows ignored after a cold start (a stopped pump shows spin-up pulses)
  uint16_t gap_start = 50;             // pause between cycles while the state is unknown
  uint16_t lid_recheck_ticks = 1000;   // lid open: re-check with normal cycles every 10 s
  uint16_t fault_recheck_ticks = 6000; // no water: every 60 s (limits dry running)
  uint16_t block_recheck_ticks = 6000; // pump blocked: every 60 s
  uint16_t block_max_windows = 8;      // pump blocked: a re-check lasts at most 8 windows (~12 s)
  uint16_t block_bad_windows = 4;      // ... and stops when 4 windows in a row still show the blockage

  // ---- inputs, set by the YAML
  bool want_pump = false;

  // ---- outputs, read by the YAML
  Water water = NO_WATER;
  bool water_valid = false;
  bool pump_on = false;
  uint32_t windows = 0;

  // ---- smart mode (day 08:00-21:59: 10 min on / 10 min off; night: 10 min on / 50 min off)
  uint16_t smart_sec = 0;
  bool smart_on = true;

  void begin() {
    pinMode(PIN_PUMP, OUTPUT);
    digitalWrite(PIN_PUMP, HIGH);  // pump OFF
    pinMode(PIN_SIDE_L, OUTPUT); pinMode(PIN_SIDE_R, OUTPUT);
    pinMode(PIN_LED14, OUTPUT); pinMode(PIN_LED15, OUTPUT); pinMode(PIN_LED16, OUTPUT);
    pinMode(PIN_CENTER, OUTPUT);
    digitalWrite(PIN_CENTER, HIGH);
    leds_off();
  }

  void smart_tick(bool day) {
    smart_sec++;
    const uint16_t cycle = day ? 1200 : 3600;
    if (smart_sec >= cycle) smart_sec = 0;
    smart_on = smart_sec < 600;
  }
  void smart_reset() { smart_sec = 0; smart_on = true; }
  // Called continuously while the mode is Normal: switching to Smart then starts directly in the "pump off"
  // phase (rest of the cycle: 10 min by day, 50 min by night), then alternates normally.
  void smart_start_off() { smart_sec = 600; smart_on = false; }

  // ---- user mode (select "Mode": Normal / Smart / Off)
  uint8_t mode = MODE_UNSET;
  // Called continuously by the YAML. On a change:
  //  - to Off: pump stopped (want_pump is computed by the YAML)
  //  - Off -> Normal / Smart: pump restarted and the water state is measured again from scratch
  //  - Normal -> Smart: the cycle starts in the "pump off" phase
  void set_mode(uint8_t m) {
    if (m == mode) return;
    const uint8_t old = mode;
    mode = m;
    if (old == MODE_UNSET) return;              // start-up: keep the restored mode
    wake_leds();
    if (m == MODE_OFF) { smart_reset(); }
    else if (old == MODE_OFF) { restart_(); smart_reset(); }
    else if (m == MODE_SMART) { smart_start_off(); }
  }

  // Single status for Home Assistant
  // Order: Off, then a known fault, then the smart-mode pause, then "checking" while unknown, then running.
  uint8_t status() const {
    if (mode == MODE_OFF) return ST_OFF;
    if (water_valid) {
      switch (water) {
        case NO_WATER: return ST_NO_WATER;
        case PUMP_BLOCK: return ST_BLOCKED;
        case LID_UP: return ST_LID;
        default: break;
      }
    }
    if (mode == MODE_SMART && !smart_on) return ST_PAUSED;
    return water_valid ? ST_RUNNING : ST_MEASURING;
  }

  // Called every 10 ms. Returns true when the water state just changed.
  //  - water OK: 1 s of pump + 0.5 s of sensing, in a loop
  //  - unknown (start-up): same cycle with a short pause, until a state is confirmed
  //  - fault (lid open / no water / pump blocked): the pump is no longer driven, only the line level is
  //    watched (passive_()); now and then a few normal cycles re-check the state (recheck_)
  //  - cold start (every time the pump starts again after being stopped: mode change, smart ON phase,
  //    end of a fault, re-check): 3 s first drive, and the first windows are ignored
  bool tick() {
    if (!want_pump) {
      idle_();
      was_wanted_ = false;
      return false;
    }
    if (!was_wanted_) {
      was_wanted_ = true;
      cnt_ = 0; n_ = 0;
      spin_first_ = true;
      warmup_ = warmup_windows;
    }
    if (water_valid && water != WATER_OK && !recheck_) {
      spin_first_ = false; warmup_ = 0;
      return passive_();
    }
    const bool ok = (water_valid && water == WATER_OK) || recheck_;
    const uint16_t drive = spin_first_ ? spinup_ticks : (ok ? drive_ticks : probe_drive_ticks);
    const uint16_t gap = ok ? 0 : gap_start;   // unknown state (start-up) only
    const uint16_t sense_end = drive + sense_ticks;
    if (cnt_ >= sense_end + gap) { cnt_ = 0; n_ = 0; }

    if (cnt_ < drive) {
      pinMode(PIN_PUMP, OUTPUT);
      digitalWrite(PIN_PUMP, LOW);  // pump ON
      pump_on = true;
    } else if (cnt_ < sense_end) {
      pump_on = false;
      pinMode(PIN_PUMP, SENSE_PULLUP ? INPUT_PULLUP : INPUT);
      // The first sample is read right after releasing the pin: do nothing before it (a lifted lid shows up
      // as a single falling edge only if this first sample is still 0).
      const int lvl = digitalRead(PIN_PUMP) ? 1 : 0;
      if (n_ == 0) { falls_ = 0; rises_ = 0; }
      if (last_ == 1 && lvl == 0) falls_++;
      if (last_ == 0 && lvl == 1) rises_++;
      last_ = lvl;
      if (n_ < sizeof(bits_) - 1) bits_[n_] = lvl ? '1' : '0';
#ifdef FOUNTAIN_PROBE
      // The fast probe takes 2.4 ms: it runs AFTER the first sample.
      if (n_ == 0 || n_ == 25) probe_(n_ == 0 ? 0 : 1);
#endif
      n_++;
    } else {
      // pause between cycles: pump not driven, GPIO13 as an input (like the stock firmware)
      pinMode(PIN_PUMP, INPUT);
      pump_on = false;
    }
    cnt_++;

    bool changed = false;
    if (cnt_ == sense_end) {
      bits_[n_ < sizeof(bits_) - 1 ? n_ : sizeof(bits_) - 1] = 0;
      windows++;
      spin_first_ = false;
      const bool warm = warmup_ > 0;
      if (warm) {
        warmup_--;   // cold start: this window is ignored
      } else {
        hit_ = false;
        changed = classify_(falls_);
        if (recheck_) {
          recheck_windows_++;
          // No water: 2 windows in a row with >= 2 edges = the fault persists, stop driving the pump.
          // Pump blocked: more time is given (3 s spin-up, 4 windows) to judge the real state.
          bad_run_ = (falls_ >= 2) ? bad_run_ + 1 : 0;
          const bool blocked = (water == PUMP_BLOCK);
          const uint16_t max_w = blocked ? block_max_windows : 6;
          const uint16_t max_bad = blocked ? block_bad_windows : 2;
          if (hit_ || recheck_windows_ >= max_w || (water != LID_UP && bad_run_ >= max_bad)) recheck_ = false;
        }
      }
      ESP_LOGD(TAG, "win=%u falls=%d rises=%d state=%s%s [%s]", (unsigned) windows, falls_, rises_,
               water_valid ? water_text(water) : "unknown", warm ? " (warm-up, ignored)" : "", bits_);
#ifdef FOUNTAIN_PROBE
      ESP_LOGD(TAG, "  probe start: %s | middle: %s", probe_txt_[0], probe_txt_[1]);
#endif
      n_ = 0;
    }
    return changed;
  }

  // ---- LEDs (called every 100 ms)
  // Left LED = Normal mode, right LED = Smart mode, white. LED mode: On = always lit, Off = never lit,
  // Timed = lit for led_timeout_ds after wake_leds() (mode change, state change, button).
  // Central red LED = problem (no water, pump blocked, lid open): blinks as long as the problem lasts,
  // independently of the LED mode.
  // Assumed polarity: "+" side high, colour pin low = lit (flip LED_COLOR_ACTIVE_LOW otherwise).
  static constexpr bool LED_COLOR_ACTIVE_LOW = true;
  static constexpr bool LEFT_IS_PIN4 = true;       // flip if left / right are swapped
  // Colour mask: bit 0 = GPIO14 (white), bit 1 = GPIO15 (violet), bit 2 = GPIO16 (cold white)
  uint8_t white_mask = 0b001;
  // Bottom button held 1.5-4 s: try the next colour combination (1-7) and log it (tuning aid).
  void cycle_color() {
    white_mask = white_mask % 7 + 1;
    wake_leds();
    ESP_LOGI(TAG, "colour test: mask=%d (bit0=GPIO14, bit1=GPIO15, bit2=GPIO16)", white_mask);
  }

  uint16_t led_timeout_ds = 100;   // Timed mode, in 100 ms units (set by the YAML)
  void wake_leds() { led_timer_ = led_timeout_ds; }

  void update_leds(uint8_t led_mode) {
    const bool smart = (mode == MODE_SMART);
    const bool problem = water_valid && water != WATER_OK && mode != MODE_OFF;
    const uint8_t wk = water_valid ? (uint8_t) water : 255;
    if (wk != last_water_) { last_water_ = wk; wake_leds(); }
    if (led_timer_ > 0) led_timer_--;
    led_phase_++;
    const bool blink = (led_phase_ % 10) < 5;   // 1 Hz
    digitalWrite(PIN_CENTER, (problem && blink) ? LOW : HIGH);
    const bool lit_now = led_mode == LED_ON || (led_mode == LED_TIMED && led_timer_ > 0);
    if (!lit_now || mode == MODE_OFF) { leds_off(); return; }
    const bool pin4_lit = (smart != LEFT_IS_PIN4);
    const uint8_t lit = LED_COLOR_ACTIVE_LOW ? LOW : HIGH;
    const uint8_t unlit = LED_COLOR_ACTIVE_LOW ? HIGH : LOW;
    digitalWrite(PIN_SIDE_L, pin4_lit ? HIGH : LOW);
    digitalWrite(PIN_SIDE_R, pin4_lit ? LOW : HIGH);
    digitalWrite(PIN_LED14, (white_mask & 1) ? lit : unlit);
    digitalWrite(PIN_LED15, (white_mask & 2) ? lit : unlit);
    digitalWrite(PIN_LED16, (white_mask & 4) ? lit : unlit);
  }

 private:
  uint16_t high_run_ = 0;
  bool lid_seen_ = false;
  uint16_t cnt_ = 0, n_ = 0, low_run_ = 0, passive_ticks_ = 0, recheck_windows_ = 0, bad_run_ = 0;
  bool recheck_ = false, hit_ = false, was_wanted_ = false, spin_first_ = false;
  uint8_t warmup_ = 0;
  int last_ = 0, falls_ = 0, rises_ = 0;   // last_: last sampled level, kept across windows
  uint8_t ok_ = 0, lid_ = 0, nw_ = 0, blk_ = 0;
  uint16_t led_timer_ = 100;               // x 100 ms: lit at start-up
  uint8_t last_water_ = 255;
  uint16_t led_phase_ = 0;
  char bits_[64] = {0};

#ifdef FOUNTAIN_PROBE
  // Diagnostic probe (DEBUG log, build with -DFOUNTAIN_PROBE): 3000 fast reads of GPIO13
  // (transitions, % high, duration). Off by default.
  char probe_txt_[2][48] = {"-", "-"};
  void probe_(int k) {
    const uint32_t t0 = micros();
    int last = digitalRead(PIN_PUMP) ? 1 : 0, ones = last, trans = 0;
    for (int i = 1; i < 3000; i++) {
      const int v = digitalRead(PIN_PUMP) ? 1 : 0;
      ones += v;
      if (v != last) trans++;
      last = v;
    }
    const uint32_t dt = micros() - t0;
    snprintf(probe_txt_[k], sizeof(probe_txt_[k]), "trans=%d high=%d%% %uus", trans, ones / 30, (unsigned) dt);
  }
#endif

  // Fault state (lid open, no water, pump blocked): the pump is no longer driven, only the level of GPIO13
  // is watched (like the stock firmware). Isolated probes gave false "pump blocked" results: a pump that
  // is completely stopped shows pulses during its first second.
  //  - lid open only: line low for 1 s (lid put back, pump plugged in again) = back to normal cycles right
  //    away, with a cold start (a stopped pump also leaves the line low, so this is not used for no water /
  //    pump blocked: those only leave through the periodic re-check)
  //  - otherwise, now and then a few normal cycles re-check the state (the displayed state only changes
  //    if they conclude something else)
  bool passive_() {
    pinMode(PIN_PUMP, INPUT);
    pump_on = false;
    cnt_ = 0; n_ = 0;
    const int lvl = digitalRead(PIN_PUMP) ? 1 : 0;
    last_ = lvl;
    low_run_ = lvl == 0 ? (low_run_ < 65535 ? low_run_ + 1 : low_run_) : 0;
    if (water == LID_UP && low_run_ >= 100) {
      ESP_LOGI(TAG, "lid put back: restarting the pump");
      low_run_ = 0; passive_ticks_ = 0;
      ok_ = lid_ = nw_ = blk_ = 0;
      water = WATER_OK;
      spin_first_ = true; warmup_ = warmup_windows;   // the pump was stopped: cold start
      return true;
    }
    if (water == PUMP_BLOCK) {
      // Pump blocked: the lid is watched too. Line high for 0.3 s = pump unplugged (lid lifted); when it
      // goes low again for 0.5 s (lid put back), test the pump right away.
      high_run_ = lvl == 1 ? (high_run_ < 65535 ? high_run_ + 1 : high_run_) : 0;
      if (high_run_ >= 30) lid_seen_ = true;
      if (lid_seen_ && low_run_ >= 50) {
        ESP_LOGI(TAG, "lid put back: testing the pump again");
        start_recheck_();
        return false;
      }
    }
    if (++passive_ticks_ >= (water == LID_UP ? lid_recheck_ticks : (water == PUMP_BLOCK ? block_recheck_ticks : fault_recheck_ticks))) {
      start_recheck_();
    }
    return false;
  }

  // Reset of the detection (Off -> running): the water state is measured again like at start-up.
  void restart_() {
    water_valid = false; water = NO_WATER;
    ok_ = lid_ = nw_ = blk_ = 0;
    recheck_ = false; lid_seen_ = false;
    cnt_ = 0; n_ = 0; low_run_ = 0; passive_ticks_ = 0; high_run_ = 0; bad_run_ = 0;
  }

  // Starts the re-check with normal cycles (the displayed state only changes if it concludes otherwise).
  void start_recheck_() {
    passive_ticks_ = 0; recheck_ = true; recheck_windows_ = 0; bad_run_ = 0;
    lid_seen_ = false; high_run_ = 0;
    ok_ = lid_ = nw_ = blk_ = 0;
    cnt_ = 0; n_ = 0;
    spin_first_ = true; warmup_ = warmup_windows;   // the pump was stopped: cold start
  }

  void idle_() {
    pinMode(PIN_PUMP, OUTPUT);
    digitalWrite(PIN_PUMP, HIGH);
    pump_on = false;
    cnt_ = 0; n_ = 0; low_run_ = 0; passive_ticks_ = 0; recheck_ = false; high_run_ = 0; lid_seen_ = false;
  }

  // Rules of the stock firmware: 0 edges x3 = water OK; 1 x3 = lid open; 2 x4 = no water;
  // 3 to 5 x4 = pump blocked; >= 6: counters reset, state unchanged.
  bool classify_(int falls) {
    Water target = water;
    bool hit = false;
    if (falls == 0) {
      ok_ = ok_ < 255 ? ok_ + 1 : ok_; lid_ = nw_ = blk_ = 0;
      if (ok_ >= 3) { target = WATER_OK; hit = true; }
    } else if (falls == 1) {
      lid_ = lid_ < 255 ? lid_ + 1 : lid_; ok_ = nw_ = blk_ = 0;
      if (lid_ >= 3) { target = LID_UP; hit = true; }
    } else if (falls == 2) {
      nw_ = nw_ < 255 ? nw_ + 1 : nw_; ok_ = lid_ = blk_ = 0;
      if (nw_ >= 4) { target = NO_WATER; hit = true; }
    } else if (falls <= 5) {
      blk_ = blk_ < 255 ? blk_ + 1 : blk_; ok_ = lid_ = nw_ = 0;
      if (blk_ >= 4) { target = PUMP_BLOCK; hit = true; }
    } else {
      ok_ = lid_ = nw_ = blk_ = 0;
    }
    if (!hit) return false;
    hit_ = true;
    const bool changed = !water_valid || target != water;
    water = target;
    water_valid = true;
    return changed;
  }

  void leds_off() {
    digitalWrite(PIN_SIDE_L, LOW); digitalWrite(PIN_SIDE_R, LOW);
    digitalWrite(PIN_LED14, LOW); digitalWrite(PIN_LED15, LOW); digitalWrite(PIN_LED16, LOW);
  }
};

// Single instance used by the YAML lambdas
inline Controller ctl;

}  // namespace xiaomi_fountain_d15
