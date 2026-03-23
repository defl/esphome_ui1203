#include "badger_meter.h"
#include "esphome/core/log.h"
#include "esphome/core/application.h"

namespace esphome {
namespace badger_meter {

static const char *const TAG = "badger_meter";

void BadgerMeterComponent::setup() {
  ESP_LOGCONFIG(TAG, "Setting up Badger Meter (Sensus UI-1203)...");
  this->clock_pin_->setup();
  this->data_pin_->setup();
  // Start with meter powered down
  this->clock_pin_->digital_write(false);
  this->state_ = ReadState::IDLE;
  this->last_read_ms_ = 0;
}

void BadgerMeterComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "Badger Meter (Sensus UI-1203):");
  LOG_PIN("  Clock Pin: ", this->clock_pin_);
  LOG_PIN("  Data Pin: ", this->data_pin_);
  ESP_LOGCONFIG(TAG, "  Power-up time: %u ms", this->power_up_time_ms_);
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
  uint32_t now = millis();

  switch (this->state_) {
    case ReadState::IDLE: {
      // Check if it's time to read, or if a manual read was requested
      bool time_to_read = (now - this->last_read_ms_) >= this->update_interval_ms_;
      if (time_to_read || this->read_requested_) {
        this->read_requested_ = false;
        ESP_LOGD(TAG, "Starting meter read...");
        this->power_up_();
        this->set_state_(ReadState::POWER_UP);
      }
      break;
    }

    case ReadState::POWER_UP: {
      // Wait for meter to power up (typically 3 seconds)
      if ((now - this->state_start_ms_) >= this->power_up_time_ms_) {
        // Log pin states before reading for diagnostics
        bool clock_state = this->clock_pin_->digital_read();
        bool data_state = this->data_pin_->digital_read();
        ESP_LOGD(TAG, "Power-up complete. Clock pin: %d, Data pin: %d (before read)",
                 clock_state, data_state);
        this->set_state_(ReadState::READING);
      }
      break;
    }

    case ReadState::READING: {
      // Read the data synchronously - this blocks for ~100ms
      // which is acceptable for a water meter reading
      this->read_data_();
      this->set_state_(ReadState::POWER_DOWN);
      break;
    }

    case ReadState::POWER_DOWN: {
      this->power_down_();
      ESP_LOGD(TAG, "Read complete, raw data: '%s'", this->read_buffer_.c_str());
      this->set_state_(ReadState::PARSE);
      break;
    }

    case ReadState::PARSE: {
      this->parse_data_(this->read_buffer_);
      this->last_read_ms_ = now;
      this->set_state_(ReadState::IDLE);
      break;
    }
  }
}

void BadgerMeterComponent::power_up_() {
  // In passive capture mode, meter is powered externally (5V on RED, GND on BLACK).
  // No clock toggling needed. This is a no-op but kept for state machine compatibility.
  ESP_LOGD(TAG, "Passive mode: meter should be externally powered (5V on RED, GND on BLACK)");
}

void BadgerMeterComponent::power_down_() {
  // In passive capture mode, we don't control power.
  ESP_LOGD(TAG, "Passive mode: power-down is a no-op");
}

bool BadgerMeterComponent::read_bit_() {
  // Sensus UI-1203 clocking sequence (per kmeter reference implementation):
  // 1. Clock LOW - 500us
  // 2. Clock HIGH - wait 70us settle, then sample data
  // 3. Hold HIGH for remaining ~430us
  // Data pin: open-collector, pulled high at idle. Meter pulls LOW to signal.
  // Inverted: LOW = 1, HIGH = 0 (per sensus_protocol_lib)
  static const uint32_t HALF_CYCLE_US = 500;
  static const uint32_t SETTLE_US = 70;

  this->clock_pin_->digital_write(false);
  delayMicroseconds(HALF_CYCLE_US);
  this->clock_pin_->digital_write(true);
  delayMicroseconds(SETTLE_US);
  bool value = !this->data_pin_->digital_read();  // Inverted
  delayMicroseconds(HALF_CYCLE_US - SETTLE_US);
  return value;
}

uint8_t BadgerMeterComponent::read_byte_(bool &ok) {
  // Frame format: start(0), 7 data bits (LSB first), parity, stop(1)
  // Assumes start bit has already been consumed by the caller.
  ok = true;

  // Read 7 data bits, LSB first
  uint8_t value = 0;
  uint8_t parity = 0;
  for (int i = 0; i < 7; i++) {
    bool bit = this->read_bit_();
    if (bit) {
      value |= (1 << i);
      parity ^= 1;
    }
  }

  // Read and verify even parity bit
  bool parity_bit = this->read_bit_();
  if (parity_bit != (bool)parity) {
    ESP_LOGW(TAG, "Parity error on byte 0x%02X ('%c')", value, value);
    ok = false;
  }

  // Read stop bit - should be 1 (HIGH)
  bool stop = this->read_bit_();
  if (!stop) {
    ESP_LOGW(TAG, "Expected stop bit (1), got 0");
    ok = false;
  }

  return value;
}

void BadgerMeterComponent::read_data_() {
  this->read_buffer_.clear();

  // HIGH-RESOLUTION PASSIVE SIGNAL CAPTURE
  // No clock toggling - just observe the data pin and record every transition
  // with microsecond timestamps. This tells us exactly what the meter is sending.
  //
  // With 5V on RED and GND on BLACK, the meter free-runs.
  // We capture transitions on WHITE (data pin) to determine the actual protocol.

  static const int MAX_TRANSITIONS = 200;
  struct Transition {
    uint32_t timestamp_us;  // micros() value
    bool level;             // pin state after transition
  };
  Transition transitions[MAX_TRANSITIONS];
  int num_transitions = 0;

  // Capture duration: 200ms should cover several frames at any reasonable baud rate
  static const uint32_t CAPTURE_DURATION_US = 200000;

  // Record initial state
  bool last_level = this->data_pin_->digital_read();
  uint32_t start_us = micros();
  transitions[0].timestamp_us = start_us;
  transitions[0].level = last_level;
  num_transitions = 1;

  ESP_LOGI(TAG, "=== SIGNAL CAPTURE START (200ms, max %d transitions) ===", MAX_TRANSITIONS);
  ESP_LOGI(TAG, "Initial pin state: %d", last_level);

  // Tight polling loop - sample as fast as possible
  while ((micros() - start_us) < CAPTURE_DURATION_US && num_transitions < MAX_TRANSITIONS) {
    bool current = this->data_pin_->digital_read();
    if (current != last_level) {
      transitions[num_transitions].timestamp_us = micros();
      transitions[num_transitions].level = current;
      num_transitions++;
      last_level = current;
    }
  }

  uint32_t elapsed_us = micros() - start_us;
  ESP_LOGI(TAG, "Captured %d transitions in %u us", num_transitions, elapsed_us);

  if (num_transitions <= 1) {
    ESP_LOGW(TAG, "No transitions detected! Pin stuck at %d", transitions[0].level);
    ESP_LOGW(TAG, "Check wiring: WHITE wire to data pin, 5V to RED, GND to BLACK");
    return;
  }

  // Log all transitions with delta times
  ESP_LOGI(TAG, "--- Transition log (delta_us, level) ---");
  for (int i = 1; i < num_transitions; i++) {
    uint32_t delta = transitions[i].timestamp_us - transitions[i-1].timestamp_us;
    ESP_LOGI(TAG, "  T%3d: +%6u us -> %d  (%s for %u us)",
             i, delta, transitions[i].level,
             transitions[i-1].level ? "HIGH" : "LOW",
             delta);
  }

  // Compute statistics on HIGH and LOW pulse durations
  uint32_t min_high = UINT32_MAX, max_high = 0, sum_high = 0, count_high = 0;
  uint32_t min_low = UINT32_MAX, max_low = 0, sum_low = 0, count_low = 0;

  for (int i = 1; i < num_transitions; i++) {
    uint32_t delta = transitions[i].timestamp_us - transitions[i-1].timestamp_us;
    if (transitions[i-1].level) {
      // Was HIGH for this duration
      if (delta < min_high) min_high = delta;
      if (delta > max_high) max_high = delta;
      sum_high += delta;
      count_high++;
    } else {
      // Was LOW for this duration
      if (delta < min_low) min_low = delta;
      if (delta > max_low) max_low = delta;
      sum_low += delta;
      count_low++;
    }
  }

  ESP_LOGI(TAG, "--- Pulse statistics ---");
  if (count_high > 0) {
    ESP_LOGI(TAG, "HIGH pulses: count=%u, min=%u us, max=%u us, avg=%u us",
             count_high, min_high, max_high, sum_high / count_high);
  }
  if (count_low > 0) {
    ESP_LOGI(TAG, "LOW pulses:  count=%u, min=%u us, max=%u us, avg=%u us",
             count_low, min_low, max_low, sum_low / count_low);
  }

  // Estimate frequency
  if (num_transitions >= 3) {
    uint32_t total_us = transitions[num_transitions-1].timestamp_us - transitions[0].timestamp_us;
    float cycles = (num_transitions - 1) / 2.0f;
    float freq = cycles / (total_us / 1000000.0f);
    ESP_LOGI(TAG, "Estimated frequency: %.1f Hz", freq);

    // Check for multiple distinct pulse widths (suggests data encoding)
    // Bucket pulse durations and look for clusters
    ESP_LOGI(TAG, "--- Pulse width histogram (LOW pulses) ---");
    // Simple histogram: count pulses in 100us buckets
    static const int NUM_BUCKETS = 20;
    static const uint32_t BUCKET_SIZE = 500;  // 500us per bucket
    int low_buckets[NUM_BUCKETS] = {};
    int high_buckets[NUM_BUCKETS] = {};

    for (int i = 1; i < num_transitions; i++) {
      uint32_t delta = transitions[i].timestamp_us - transitions[i-1].timestamp_us;
      int bucket = delta / BUCKET_SIZE;
      if (bucket >= NUM_BUCKETS) bucket = NUM_BUCKETS - 1;
      if (transitions[i-1].level)
        high_buckets[bucket]++;
      else
        low_buckets[bucket]++;
    }

    for (int b = 0; b < NUM_BUCKETS; b++) {
      if (low_buckets[b] > 0 || high_buckets[b] > 0) {
        ESP_LOGI(TAG, "  %5u-%5u us: LOW=%d, HIGH=%d",
                 b * BUCKET_SIZE, (b + 1) * BUCKET_SIZE,
                 low_buckets[b], high_buckets[b]);
      }
    }
  }

  ESP_LOGI(TAG, "=== SIGNAL CAPTURE END ===");
}

void BadgerMeterComponent::parse_data_(const std::string &data) {
  if (data.empty()) {
    ESP_LOGW(TAG, "Empty data received from meter");
    return;
  }

  // Publish raw string
  if (this->raw_string_sensor_) {
    this->raw_string_sensor_->publish_state(data);
  }

  ESP_LOGI(TAG, "Meter response: '%s'", data.c_str());

  // Common Sensus/Badger format: R<digits><CR>
  // First character is typically 'R' (for Reading)
  // Digits after R: first N digits = reading, rest = meter ID or static
  //
  // Example: "R226107229550"
  //   Reading: 2261 (could be in gallons, cubic feet, or cubic meters
  //            depending on meter configuration)
  //   Remaining: 07229550 (possibly meter serial/ID)
  //
  // Alternate format from some meters: "V;RBxxxxxxx;IByyyyy;Kmmmmm"
  //   RB = reading, IB = meter ID, K = check

  if (data[0] == 'R' && data.length() >= 5) {
    // Simple R-prefix format
    // The reading length varies by meter model. Common configs:
    //   - 4 digits reading + 8 digits ID (12 char total after R)
    //   - 7 digits reading + 5 digits ID
    // We'll try to extract a numeric reading from the first portion.

    std::string digits = data.substr(1);  // Everything after 'R'

    if (this->raw_value_sensor_) {
      // Publish the full numeric value
      char *end;
      long long raw = strtoll(digits.c_str(), &end, 10);
      if (end != digits.c_str()) {
        this->raw_value_sensor_->publish_state(static_cast<float>(raw));
      }
    }

    if (this->meter_reading_sensor_) {
      // Extract the meter reading
      // Default: first 7 digits are the reading in the meter's native unit
      // Adjust reading_digits based on your meter's configuration
      int reading_digits = std::min(static_cast<int>(digits.length()), 7);
      std::string reading_str = digits.substr(0, reading_digits);

      char *end;
      long reading = strtol(reading_str.c_str(), &end, 10);
      if (end != reading_str.c_str()) {
        // Convert to cubic meters (adjust divisor based on your meter)
        // Common: reading in gallons -> divide by 264.172 for m3
        // Or reading may already be in the desired unit
        // Default: publish as-is, user can apply filters in YAML
        float value = static_cast<float>(reading);
        this->meter_reading_sensor_->publish_state(value);
        ESP_LOGI(TAG, "Meter reading: %.0f", value);
      }
    }

    // Extract meter ID from remaining digits
    if (this->meter_id_sensor_ && digits.length() > 7) {
      std::string meter_id = digits.substr(7);
      this->meter_id_sensor_->publish_state(meter_id);
    }

  } else if (data.find(";RB") != std::string::npos) {
    // Alternate format: "V;RBxxxxxxx;IByyyyy;Kmmmmm"
    size_t rb_pos = data.find(";RB");
    size_t ib_pos = data.find(";IB");
    size_t k_pos = data.find(";K");

    if (rb_pos != std::string::npos) {
      size_t end_pos = (ib_pos != std::string::npos) ? ib_pos : data.length();
      std::string reading_str = data.substr(rb_pos + 3, end_pos - rb_pos - 3);

      if (this->meter_reading_sensor_) {
        char *end;
        long reading = strtol(reading_str.c_str(), &end, 10);
        if (end != reading_str.c_str()) {
          this->meter_reading_sensor_->publish_state(static_cast<float>(reading));
          ESP_LOGI(TAG, "Meter reading (RB format): %ld", reading);
        }
      }

      if (this->raw_value_sensor_) {
        char *end;
        long reading = strtol(reading_str.c_str(), &end, 10);
        if (end != reading_str.c_str()) {
          this->raw_value_sensor_->publish_state(static_cast<float>(reading));
        }
      }
    }

    if (ib_pos != std::string::npos && this->meter_id_sensor_) {
      size_t end_pos = (k_pos != std::string::npos) ? k_pos : data.length();
      std::string meter_id = data.substr(ib_pos + 3, end_pos - ib_pos - 3);
      this->meter_id_sensor_->publish_state(meter_id);
    }

  } else {
    ESP_LOGW(TAG, "Unknown data format: '%s'", data.c_str());
    // Still publish raw reading as best effort
    if (this->meter_reading_sensor_) {
      // Try to extract any numeric value
      std::string digits;
      for (char c : data) {
        if (c >= '0' && c <= '9') digits += c;
      }
      if (!digits.empty()) {
        char *end;
        long reading = strtol(digits.c_str(), &end, 10);
        if (end != digits.c_str()) {
          this->meter_reading_sensor_->publish_state(static_cast<float>(reading));
        }
      }
    }
  }
}

}  // namespace badger_meter
}  // namespace esphome
