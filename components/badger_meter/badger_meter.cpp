#include "badger_meter.h"
#include "esphome/core/application.h"
#include "esphome/core/log.h"

#ifdef USE_ESP_IDF
#include "driver/gpio.h"
#endif

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <string>

namespace esphome {
namespace badger_meter {

static const char *const TAG = "badger_meter";

// How long to watch a quiet line before giving up on a burst. Non-blocking — the pin is sampled
// once per loop() — so this costs nothing and can be generous.
static const uint32_t ARM_TIMEOUT_MS = 8000;
// Transition dumps are for a human; a whole burst is hundreds of lines and the histogram carries
// the same information.
static const int MAX_LOGGED_TRANSITIONS = 24;
static const uint32_t HISTOGRAM_BUCKET_US = 50;
static const int HISTOGRAM_BUCKETS = 100;

// Bit periods worth trying against a capture: whatever the capture itself suggests, then the two
// rates the reference implementations use (1200 baud, kmeter's ~1 kHz) and the common faster ones.
static const uint32_t CANDIDATE_BIT_US[] = {833, 1000, 416, 208, 104, 2083};

static const int SCAN_PINS[] = {4, 16, 17, 25, 26, 34, 35, 36, 39};

void BadgerMeterComponent::scan_pins_() {
#ifdef USE_ESP_IDF
  // Runs before any pin here is configured. Reads each candidate with the internal pull-up and
  // then the pull-down: a pin that reads 1 both ways is being held high by something external,
  // 0 both ways is held low, and 1-then-0 is simply floating. That distinguishes "the data wire
  // is not on the pin we think" from "the pin is being held low", which a DMM on the wire and a
  // digital_read() disagreeing cannot.
  for (int index = 0; index < SCAN_COUNT; index++) {
    const int number = SCAN_PINS[index];
    const gpio_num_t pin = static_cast<gpio_num_t>(number);
    gpio_set_direction(pin, GPIO_MODE_INPUT);
    // 34..39 are input-only and carry no internal pull resistors; the calls fail harmlessly and
    // both readings are then the bare pin.
    gpio_set_pull_mode(pin, GPIO_PULLUP_ONLY);
    delay(3);
    const int with_pullup = gpio_get_level(pin);
    gpio_set_pull_mode(pin, GPIO_PULLDOWN_ONLY);
    delay(3);
    const int with_pulldown = gpio_get_level(pin);
    gpio_set_pull_mode(pin, GPIO_FLOATING);
    this->scan_pullup_[index] = with_pullup;
    this->scan_pulldown_[index] = with_pulldown;
  }
  this->scanned_ = true;
#endif
}

void BadgerMeterComponent::setup() {
  ESP_LOGCONFIG(TAG, "Setting up Badger Meter (Sensus UI-1203)...");
  this->scan_pins_();
  this->data_pin_->setup();
  if (this->clock_pin_ != nullptr) {
    // Configured only when the ESP feeds the meter. Held HIGH so the register runs continuously
    // and transmits on its own; this component never clocks it.
    this->clock_pin_->setup();
    this->clock_pin_->digital_write(true);
  }
  this->state_ = ReadState::IDLE;
  this->last_read_ms_ = 0;
}

void BadgerMeterComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "Badger Meter (Sensus UI-1203):");
  if (this->clock_pin_ != nullptr) {
    LOG_PIN("  Clock/power pin (held high): ", this->clock_pin_);
  } else {
    ESP_LOGCONFIG(TAG, "  Clock/power pin: not set — meter is externally powered");
  }
  LOG_PIN("  Data Pin: ", this->data_pin_);
  if (this->scanned_) {
    for (int index = 0; index < SCAN_COUNT; index++) {
      const int up = this->scan_pullup_[index], down = this->scan_pulldown_[index];
      const char *verdict = "floating / unconnected";
      if (up == 1 && down == 1)
        verdict = "HELD HIGH externally";
      else if (up == 0 && down == 0)
        verdict = "HELD LOW externally";
      ESP_LOGCONFIG(TAG, "  boot pin scan GPIO%-2d: pullup=%d pulldown=%d -> %s",
                    SCAN_PINS[index], up, down, verdict);
    }
  }
  ESP_LOGCONFIG(TAG, "  Mode: %s, bit period: %u us, reset hold: %u ms",
                this->mode_ == ReadMode::CLOCKED ? "clocked" : "passive", this->bit_period_us_,
                this->reset_hold_ms_);
  ESP_LOGCONFIG(TAG, "  Capture window: %u ms, idle gap: %u ms, interval: %u ms",
                this->capture_window_ms_, this->idle_gap_ms_, this->update_interval_ms_);
  if (this->meter_reading_sensor_)
    LOG_SENSOR("  ", "Meter Reading", this->meter_reading_sensor_);
  if (this->raw_value_sensor_)
    LOG_SENSOR("  ", "Raw Value", this->raw_value_sensor_);
  if (this->raw_string_sensor_)
    LOG_TEXT_SENSOR("  ", "Raw String", this->raw_string_sensor_);
  if (this->meter_id_sensor_)
    LOG_TEXT_SENSOR("  ", "Meter ID", this->meter_id_sensor_);
}

void BadgerMeterComponent::set_state_(ReadState new_state) {
  this->state_ = new_state;
  this->state_start_ms_ = millis();
}

void BadgerMeterComponent::loop() {
  const uint32_t now = millis();

  switch (this->state_) {
    case ReadState::IDLE: {
      const bool time_to_read = (now - this->last_read_ms_) >= this->update_interval_ms_;
      if (time_to_read || this->read_requested_) {
        this->read_requested_ = false;
        if (this->mode_ == ReadMode::CLOCKED) {
          if (this->clock_pin_ == nullptr) {
            ESP_LOGE(TAG, "clocked mode needs a clock_pin");
            this->last_read_ms_ = now;
            break;
          }
          this->clock_pin_->digital_write(false);
          ESP_LOGD(TAG, "Holding the meter unpowered for %u ms to reset its send buffer",
                   this->reset_hold_ms_);
          this->set_state_(ReadState::RESET);
          break;
        }
        this->armed_level_ = this->data_pin_->digital_read();
        ESP_LOGD(TAG, "Armed; data pin idles at %d, waiting for an edge", this->armed_level_);
        this->set_state_(ReadState::ARMED);
      }
      break;
    }

    case ReadState::ARMED: {
      // One sample per loop. A burst is milliseconds of continuous toggling, so a ~16 ms loop
      // cannot miss one — it lands inside the burst rather than on its first edge, which async
      // framing makes harmless: every character carries its own start and stop bits.
      if (this->data_pin_->digital_read() != this->armed_level_) {
        this->capture_();
        this->set_state_(ReadState::PARSE);
        break;
      }
      if ((now - this->state_start_ms_) >= ARM_TIMEOUT_MS) {
        ESP_LOGW(TAG, "No edge in %u ms — line stuck at %d. Meter unpowered, wrong pin, or "
                      "nothing driving the data wire.",
                 ARM_TIMEOUT_MS, this->armed_level_);
        this->num_transitions_ = 0;
        this->last_read_ms_ = now;
        this->set_state_(ReadState::IDLE);
      }
      break;
    }

    case ReadState::RESET: {
      if ((now - this->state_start_ms_) >= this->reset_hold_ms_) {
        this->clock_pin_->digital_write(true);
        ESP_LOGD(TAG, "Powering the meter for %u ms before clocking", this->power_up_time_ms_);
        this->set_state_(ReadState::POWER_UP);
      }
      break;
    }

    case ReadState::POWER_UP: {
      // Non-blocking: the register wants seconds of settled power, and this board runs a 1 s
      // pressure check that must not wait for it.
      if ((now - this->state_start_ms_) >= this->power_up_time_ms_) {
        this->clock_bits_();
        this->set_state_(ReadState::PARSE);
      }
      break;
    }

    case ReadState::PARSE: {
      if (this->mode_ == ReadMode::CLOCKED) {
        const DecodeResult best = this->decode_bits_best_();
        if (best.score() > 0) {
          ESP_LOGI(TAG, "Clocked decode: %d chars, %d errors, %s, %d%s1 -> '%s'", best.chars,
                   best.errors, best.inverted ? "inverted" : "non-inverted", best.data_bits,
                   best.parity ? "E" : "N", best.text.c_str());
          if (best.chars >= 4) {
            this->read_buffer_ = best.text;
            this->parse_data_(this->read_buffer_);
          }
        } else {
          ESP_LOGW(TAG, "Clocked read decoded nothing — see the bit dump");
        }
        this->report_bits_();
        this->last_read_ms_ = millis();
        this->set_state_(ReadState::IDLE);
        break;
      }
      // Verdict first, dump second. The API log ring drops messages under a flood, and a few
      // hundred transition lines were silently eating the histogram and the decode line.
      const DecodeResult best = this->decode_best_();
      if (best.score() > 0) {
        ESP_LOGI(TAG, "Best decode: %d chars, %d errors @ %u us/bit (%u baud), %s, %d%s1 -> '%s'",
                 best.chars, best.errors, best.bit_us,
                 best.bit_us ? 1000000U / best.bit_us : 0,
                 best.inverted ? "inverted" : "non-inverted", best.data_bits,
                 best.parity ? "E" : "N", best.text.c_str());
      } else {
        ESP_LOGW(TAG, "No framing candidate decoded anything — see the histogram below");
      }
      if (best.chars >= 4) {
        this->read_buffer_ = best.text;
        this->parse_data_(this->read_buffer_);
      }
      this->report_();
      this->last_read_ms_ = millis();
      this->set_state_(ReadState::IDLE);
      break;
    }

    default:
      this->set_state_(ReadState::IDLE);
      break;
  }
}

void BadgerMeterComponent::capture_() {
  this->num_transitions_ = 0;

  bool last_level = this->data_pin_->digital_read();
  const uint32_t start_us = micros();
  this->transitions_[0].offset_us = 0;
  this->transitions_[0].level = last_level;
  this->num_transitions_ = 1;

  const uint32_t window_us = this->capture_window_ms_ * 1000UL;
  const uint32_t idle_us = this->idle_gap_ms_ * 1000UL;
  uint32_t last_edge_us = 0;
  uint32_t fed_at_us = 0;

  while (true) {
    const uint32_t elapsed = micros() - start_us;
    if (elapsed >= window_us || this->num_transitions_ >= MAX_TRANSITIONS)
      break;
    // A burst that has ended is the natural place to stop: it keeps the blocking section far
    // shorter than the window, which matters because this board also runs a 1 s pressure check.
    if (this->num_transitions_ > 20 && (elapsed - last_edge_us) > idle_us)
      break;
    if (elapsed - fed_at_us > 100000UL) {
      App.feed_wdt();
      fed_at_us = elapsed;
    }

    const bool current = this->data_pin_->digital_read();
    if (current != last_level) {
      last_edge_us = micros() - start_us;
      this->transitions_[this->num_transitions_].offset_us = last_edge_us;
      this->transitions_[this->num_transitions_].level = current;
      this->num_transitions_++;
      last_level = current;
    }
  }
  App.feed_wdt();
}

void BadgerMeterComponent::report_() {
  if (this->num_transitions_ <= 1) {
    ESP_LOGW(TAG, "Capture holds no edges (pin %d)", this->transitions_[0].level);
    return;
  }

  const uint32_t span = this->transitions_[this->num_transitions_ - 1].offset_us;
  ESP_LOGI(TAG, "=== CAPTURE: %d transitions over %u us ===", this->num_transitions_, span);

  uint32_t min_delta = UINT32_MAX, max_delta = 0;
  int low_buckets[HISTOGRAM_BUCKETS] = {};
  int high_buckets[HISTOGRAM_BUCKETS] = {};
  for (int i = 1; i < this->num_transitions_; i++) {
    const uint32_t delta = this->transitions_[i].offset_us - this->transitions_[i - 1].offset_us;
    if (delta < min_delta)
      min_delta = delta;
    if (delta > max_delta)
      max_delta = delta;
    int bucket = (int) (delta / HISTOGRAM_BUCKET_US);
    if (bucket >= HISTOGRAM_BUCKETS)
      bucket = HISTOGRAM_BUCKETS - 1;
    if (this->transitions_[i - 1].level)
      high_buckets[bucket]++;
    else
      low_buckets[bucket]++;
  }

  ESP_LOGI(TAG, "--- pulse widths: min %u us, max %u us ---", min_delta, max_delta);
  ESP_LOGI(TAG, "--- histogram (%u us buckets) ---", HISTOGRAM_BUCKET_US);
  for (int b = 0; b < HISTOGRAM_BUCKETS; b++) {
    if (low_buckets[b] == 0 && high_buckets[b] == 0)
      continue;
    ESP_LOGI(TAG, "  %5u-%5u us: LOW=%d HIGH=%d", b * HISTOGRAM_BUCKET_US,
             (b + 1) * HISTOGRAM_BUCKET_US - 1, low_buckets[b], high_buckets[b]);
  }
  if (min_delta > 0)
    ESP_LOGI(TAG, "Narrowest pulse %u us => %u baud if that is one bit", min_delta,
             1000000U / min_delta);

  // Mains coupling on a line nothing is driving has a signature: a long HIGH, a shorter LOW and
  // a 16.67 ms repeat, with microsecond chatter at each threshold crossing. Worth naming,
  // because it looks like a signal and frames cleanly as one repeated character.
  int long_high = 0, mid_low = 0;
  for (int i = 1; i < this->num_transitions_; i++) {
    const uint32_t delta = this->transitions_[i].offset_us - this->transitions_[i - 1].offset_us;
    if (this->transitions_[i - 1].level && delta >= 12500 && delta <= 15000)
      long_high++;
    else if (!this->transitions_[i - 1].level && delta >= 2400 && delta <= 3100)
      mid_low++;
  }
  if (long_high >= 3 && mid_low >= 3)
    ESP_LOGW(TAG, "%dx long HIGH + %dx ~2.7ms LOW at a ~16.7 ms repeat: this is 60 Hz mains "
                  "coupling on an undriven line, not meter data",
             long_high, mid_low);

  const int logged = this->num_transitions_ < MAX_LOGGED_TRANSITIONS ? this->num_transitions_
                                                                     : MAX_LOGGED_TRANSITIONS;
  for (int i = 1; i < logged; i++) {
    const uint32_t delta = this->transitions_[i].offset_us - this->transitions_[i - 1].offset_us;
    ESP_LOGI(TAG, "  T%3d @%7u us: %s for %6u us", i, this->transitions_[i - 1].offset_us,
             this->transitions_[i - 1].level ? "HIGH" : "LOW ", delta);
  }
  if (this->num_transitions_ > logged)
    ESP_LOGI(TAG, "  ... %d more transitions not listed", this->num_transitions_ - logged);
}

// (bit period, clock-low time) pairs tried one per read. kmeter's 1000/500 first, then slower
// rates, then short power interruptions — a 500 us outage every millisecond may simply brown the
// register out, and "briefly remove power" is all the reference implementations actually claim.
struct ClockProfile {
  uint32_t period_us;
  uint32_t low_us;
};
static const ClockProfile CLOCK_SWEEP[] = {
    {1000, 500}, {2000, 500}, {5000, 500}, {1000, 100}, {2000, 200}, {10000, 1000},
};
static const int SWEEP_LEN = sizeof(CLOCK_SWEEP) / sizeof(CLOCK_SWEEP[0]);
// Half the old count: the blocking phase is the cost, and 200 bits is 20 characters.
static const int CLOCK_BITS_PER_READ = 200;
// The clock phase blocks the loop, and this board runs a 1 s pressure check, so a slow profile
// gets fewer bits rather than a longer block. 10 ms/bit x 200 would have been two seconds.
static const uint32_t CLOCK_BUDGET_US = 500000;

void BadgerMeterComponent::clock_bits_() {
  // kmeter's clocking: the power line IS the clock. Drop it, raise it, let the register settle,
  // then sample. One bit per cycle.
  const ClockProfile profile = CLOCK_SWEEP[this->sweep_index_ % SWEEP_LEN];
  this->sweep_index_++;
  const uint32_t low_us = profile.low_us;
  const uint32_t high_us = profile.period_us > low_us ? profile.period_us - low_us : 500;
  const uint32_t settle = high_us < 140 ? high_us / 2 : 70;
  this->last_period_us_ = profile.period_us;
  this->last_low_us_ = low_us;

  int budget_bits = (int) (CLOCK_BUDGET_US / profile.period_us);
  if (budget_bits > CLOCK_BITS_PER_READ)
    budget_bits = CLOCK_BITS_PER_READ;
  if (budget_bits < 40)
    budget_bits = 40;

  this->num_bits_ = 0;
  uint32_t fed_at = micros();

  for (int i = 0; i < budget_bits; i++) {
    this->clock_pin_->digital_write(false);
    delayMicroseconds(low_us > 40 ? low_us - 30 : low_us);
    // Sampled with the meter unpowered, for comparison with the powered sample below.
    this->low_phase_[i] = this->data_pin_->digital_read() ? 1 : 0;
    delayMicroseconds(30);
    this->clock_pin_->digital_write(true);
    delayMicroseconds(settle);
    this->bits_[this->num_bits_++] = this->data_pin_->digital_read() ? 1 : 0;
    delayMicroseconds(high_us - settle);
    if (micros() - fed_at > 100000UL) {
      App.feed_wdt();
      fed_at = micros();
    }
  }
  // Leave the meter powered; it costs nothing and keeps the line defined between reads.
  this->clock_pin_->digital_write(true);
  App.feed_wdt();
}

void BadgerMeterComponent::report_bits_() {
  int ones = 0, low_ones = 0, differ = 0;
  for (int i = 0; i < this->num_bits_; i++) {
    ones += this->bits_[i];
    low_ones += this->low_phase_[i];
    if (this->bits_[i] != this->low_phase_[i])
      differ++;
  }
  ESP_LOGI(TAG, "=== CLOCKED: %d bits, %u us period / %u us low ===", this->num_bits_,
           this->last_period_us_, this->last_low_us_);
  ESP_LOGI(TAG, "  powered sample: %d ones / %d zeros | unpowered sample: %d ones / %d zeros | "
                "%d cycles differ",
           ones, this->num_bits_ - ones, low_ones, this->num_bits_ - low_ones, differ);
  if (ones == 0 || ones == this->num_bits_) {
    if (differ == 0) {
      ESP_LOGW(TAG, "Line sat at %d throughout and did not react to the clock at all — the meter "
                    "is not driving this pin, or is not being powered by the clock pin",
               ones ? 1 : 0);
    } else {
      ESP_LOGW(TAG, "Powered sample never moved, but %d cycles differ from the unpowered sample "
                    "— the clock IS reaching the line, the sampling point is wrong",
               differ);
    }
    return;
  }
  std::string row;
  for (int i = 0; i < this->num_bits_; i++) {
    row += this->bits_[i] ? '1' : '0';
    if (row.length() == 60 || i == this->num_bits_ - 1) {
      ESP_LOGI(TAG, "  %3d: %s", i - (int) row.length() + 1, row.c_str());
      row.clear();
    }
  }
}

DecodeResult BadgerMeterComponent::decode_bits_once_(bool inverted, int data_bits,
                                                     bool parity) const {
  DecodeResult out;
  out.inverted = inverted;
  out.data_bits = data_bits;
  out.parity = parity;
  out.bit_us = this->bit_period_us_;

  const int frame = 1 + data_bits + (parity ? 1 : 0) + 1;
  int i = 0;
  while (i + frame <= this->num_bits_) {
    const bool start = inverted ? !this->bits_[i] : (bool) this->bits_[i];
    if (start) {  // idle, not a start bit
      i++;
      continue;
    }
    uint8_t value = 0;
    int ones = 0;
    for (int b = 0; b < data_bits; b++) {
      bool bit = this->bits_[i + 1 + b] != 0;
      if (inverted)
        bit = !bit;
      if (bit) {
        value |= (uint8_t) (1 << b);
        ones++;
      }
    }
    bool ok = true;
    if (parity) {
      bool bit = this->bits_[i + 1 + data_bits] != 0;
      if (inverted)
        bit = !bit;
      if (bit)
        ones++;
      if ((ones % 2) != 0)
        ok = false;
    }
    bool stop = this->bits_[i + frame - 1] != 0;
    if (inverted)
      stop = !stop;
    if (!stop)
      ok = false;

    if (!ok) {
      out.errors++;
      i++;  // resync one bit at a time rather than trusting a bad frame
      continue;
    }
    if (value >= 0x20 && value < 0x7f) {
      out.text += (char) value;
      out.chars++;
      out.seen[value >> 5] |= (1U << (value & 31U));
    } else if (value == '\r' || value == '\n') {
      out.chars++;
    } else {
      out.errors++;
    }
    i += frame;
  }
  return out;
}

DecodeResult BadgerMeterComponent::decode_bits_best_() const {
  DecodeResult best;
  const int data_bits[4] = {7, 8, 7, 8};
  const bool parity[4] = {true, false, false, true};
  for (int inverted = 0; inverted < 2; inverted++) {
    for (int f = 0; f < 4; f++) {
      const DecodeResult candidate =
          this->decode_bits_once_(inverted != 0, data_bits[f], parity[f]);
      if (candidate.score() > best.score())
        best = candidate;
    }
  }
  return best;
}

bool BadgerMeterComponent::level_at_(uint32_t offset_us) const {
  bool level = this->transitions_[0].level;
  for (int i = 1; i < this->num_transitions_; i++) {
    if (this->transitions_[i].offset_us > offset_us)
      break;
    level = this->transitions_[i].level;
  }
  return level;
}

DecodeResult BadgerMeterComponent::decode_once_(uint32_t bit_us, bool inverted, int data_bits,
                                                bool parity) const {
  DecodeResult out;
  out.bit_us = bit_us;
  out.inverted = inverted;
  out.data_bits = data_bits;
  out.parity = parity;
  if (bit_us < 40 || this->num_transitions_ < 4)
    return out;

  const uint32_t last = this->transitions_[this->num_transitions_ - 1].offset_us;
  const uint32_t frame_bits = 1 + (uint32_t) data_bits + (parity ? 1 : 0) + 1;
  uint32_t pos = 0;

  while (pos + frame_bits * bit_us <= last) {
    // Next logical falling edge at or after `pos` — the start bit.
    int idx = -1;
    for (int i = 1; i < this->num_transitions_; i++) {
      if (this->transitions_[i].offset_us < pos)
        continue;
      const bool level = inverted ? !this->transitions_[i].level : this->transitions_[i].level;
      const bool prev = inverted ? !this->transitions_[i - 1].level : this->transitions_[i - 1].level;
      if (prev && !level) {
        idx = i;
        break;
      }
    }
    if (idx < 0)
      break;

    const uint32_t start = this->transitions_[idx].offset_us;
    if (start + frame_bits * bit_us > last)
      break;

    // Sample each bit at its centre: start edge + (n + 0.5) bit periods.
    uint8_t value = 0;
    int ones = 0;
    for (int b = 0; b < data_bits; b++) {
      const uint32_t at = start + bit_us * (uint32_t) (2 * b + 3) / 2;
      bool bit = this->level_at_(at);
      if (inverted)
        bit = !bit;
      if (bit) {
        value |= (uint8_t) (1 << b);
        ones++;
      }
    }

    bool ok = true;
    if (parity) {
      const uint32_t at = start + bit_us * (uint32_t) (2 * data_bits + 3) / 2;
      bool bit = this->level_at_(at);
      if (inverted)
        bit = !bit;
      if (bit)
        ones++;
      if ((ones % 2) != 0)
        ok = false;  // even parity
    }
    const uint32_t stop_at = start + bit_us * (2 * (frame_bits - 1) + 1) / 2;
    bool stop = this->level_at_(stop_at);
    if (inverted)
      stop = !stop;
    if (!stop)
      ok = false;

    if (!ok) {
      out.errors++;
    } else if (value >= 0x20 && value < 0x7f) {
      out.text += (char) value;
      out.chars++;
      out.seen[value >> 5] |= (1U << (value & 31U));
    } else if (value == '\r' || value == '\n') {
      out.chars++;  // a real terminator, but not worth printing into the log line
    } else {
      out.errors++;
    }

    pos = start + frame_bits * bit_us;
  }
  return out;
}

DecodeResult BadgerMeterComponent::decode_best_() {
  DecodeResult best;
  if (this->num_transitions_ < 4)
    return best;

  uint32_t min_delta = UINT32_MAX;
  for (int i = 1; i < this->num_transitions_; i++) {
    const uint32_t delta = this->transitions_[i].offset_us - this->transitions_[i - 1].offset_us;
    if (delta < min_delta)
      min_delta = delta;
  }

  const size_t fixed = sizeof(CANDIDATE_BIT_US) / sizeof(CANDIDATE_BIT_US[0]);
  for (size_t c = 0; c <= fixed; c++) {
    const uint32_t bit_us = (c == 0) ? min_delta : CANDIDATE_BIT_US[c - 1];
    for (int inverted = 0; inverted < 2; inverted++) {
      // 7E1 and 8N1 first — the two the references and the vendor statement point at — then the
      // other two combinations, which cost nothing to try.
      const int data_bits[4] = {7, 8, 7, 8};
      const bool parity[4] = {true, false, false, true};
      for (int f = 0; f < 4; f++) {
        const DecodeResult candidate =
            this->decode_once_(bit_us, inverted != 0, data_bits[f], parity[f]);
        if (candidate.score() > best.score())
          best = candidate;
      }
    }
  }
  return best;
}

void BadgerMeterComponent::parse_data_(const std::string &data) {
  if (data.empty()) {
    ESP_LOGW(TAG, "Empty data received from meter");
    return;
  }

  if (this->raw_string_sensor_ != nullptr)
    this->raw_string_sensor_->publish_state(data);

  ESP_LOGI(TAG, "Meter response: '%s'", data.c_str());

  // Two payload shapes are known: a bare `R<digits>` string, and the field form
  // `V;RB<reading>;IB<id>;K<check>`. Which this meter emits is exactly what the capture is
  // meant to settle, so both are handled and anything else is published as best effort.
  if (data[0] == 'R' && data.length() >= 5) {
    const std::string digits = data.substr(1);

    if (this->raw_value_sensor_ != nullptr) {
      char *end;
      const long long raw = strtoll(digits.c_str(), &end, 10);
      if (end != digits.c_str())
        this->raw_value_sensor_->publish_state(static_cast<float>(raw));
    }

    if (this->meter_reading_sensor_ != nullptr) {
      // The reading/ID split is meter-model specific and unverified for this unit.
      const int reading_digits = std::min(static_cast<int>(digits.length()), 7);
      const std::string reading_str = digits.substr(0, reading_digits);
      char *end;
      const long reading = strtol(reading_str.c_str(), &end, 10);
      if (end != reading_str.c_str()) {
        this->meter_reading_sensor_->publish_state(static_cast<float>(reading));
        ESP_LOGI(TAG, "Meter reading: %ld", reading);
      }
    }

    if (this->meter_id_sensor_ != nullptr && digits.length() > 7)
      this->meter_id_sensor_->publish_state(digits.substr(7));

  } else if (data.find(";RB") != std::string::npos) {
    const size_t rb_pos = data.find(";RB");
    const size_t ib_pos = data.find(";IB");
    const size_t k_pos = data.find(";K");

    const size_t end_pos = (ib_pos != std::string::npos) ? ib_pos : data.length();
    const std::string reading_str = data.substr(rb_pos + 3, end_pos - rb_pos - 3);
    char *end;
    const long reading = strtol(reading_str.c_str(), &end, 10);
    if (end != reading_str.c_str()) {
      if (this->meter_reading_sensor_ != nullptr)
        this->meter_reading_sensor_->publish_state(static_cast<float>(reading));
      if (this->raw_value_sensor_ != nullptr)
        this->raw_value_sensor_->publish_state(static_cast<float>(reading));
      ESP_LOGI(TAG, "Meter reading (RB format): %ld", reading);
    }

    if (ib_pos != std::string::npos && this->meter_id_sensor_ != nullptr) {
      const size_t id_end = (k_pos != std::string::npos) ? k_pos : data.length();
      this->meter_id_sensor_->publish_state(data.substr(ib_pos + 3, id_end - ib_pos - 3));
    }

  } else {
    ESP_LOGW(TAG, "Unknown data format: '%s'", data.c_str());
    std::string digits;
    for (char c : data) {
      if (c >= '0' && c <= '9')
        digits += c;
    }
    if (!digits.empty() && this->meter_reading_sensor_ != nullptr) {
      char *end;
      const long reading = strtol(digits.c_str(), &end, 10);
      if (end != digits.c_str())
        this->meter_reading_sensor_->publish_state(static_cast<float>(reading));
    }
  }
}

}  // namespace badger_meter
}  // namespace esphome
