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
        ESP_LOGD(TAG, "Power-up complete, reading data...");
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
  // Apply power via the clock line
  this->clock_pin_->digital_write(true);
}

void BadgerMeterComponent::power_down_() {
  // Remove power - meter resets after ~1 second without power
  this->clock_pin_->digital_write(false);
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
  // Total: 10 bits per character
  ok = true;

  // Read start bit - should be 0 (LOW)
  bool start = this->read_bit_();
  if (start) {
    ESP_LOGW(TAG, "Expected start bit (0), got 1");
    ok = false;
  }

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

  // Wait for start of message: clock bits until we get a valid start bit.
  // The meter may send idle (high) bits before the first character.
  // We try up to MAX_READ_BYTES characters.
  for (int i = 0; i < MAX_READ_BYTES; i++) {
    bool ok;
    uint8_t byte = this->read_byte_(ok);

    if (!ok) {
      // Parity or framing error — discard entire read (per kmeter behavior)
      ESP_LOGW(TAG, "Frame error at position %d, discarding read", i);
      this->read_buffer_.clear();
      return;
    }

    if (byte == '\r' || byte == '\n') {
      break;  // End of message
    }

    if (byte >= 0x20 && byte <= 0x7E) {
      this->read_buffer_ += static_cast<char>(byte);
    } else {
      ESP_LOGW(TAG, "Non-printable byte at position %d: 0x%02X", i, byte);
    }
  }
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
