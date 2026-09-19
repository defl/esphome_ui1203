#include "badger_meter.h"
#include "esphome/core/application.h"
#include "esphome/core/log.h"

#include <cinttypes>
#include <cstdlib>
#include <string>

namespace esphome {
namespace badger_meter {

static const char *const TAG = "badger_meter";

// The tested register decodes identically at 417, 833 and 1000 us per bit; 417 is the fastest and
// keeps the blocking part of a read to ~420 ms.
static const uint32_t BIT_PERIOD_US = 417;
static const uint32_t CLOCK_LOW_US = 100;
// The data line takes ~100 us to recover after the clock rises; sample well clear of that.
static const uint32_t SAMPLE_AFTER_US = 220;

// Sensus framing, 7E1: a start bit (0), seven data bits LSB first, even parity, a stop bit (1).
static const int FRAME_BITS = 10;

void BadgerMeterComponent::setup() {
  ESP_LOGCONFIG(TAG, "Setting up Badger Meter (Sensus UI-1203)...");
  this->data_pin_->setup();
  this->clock_pin_->setup();
  // Held high between reads: the register stays powered and the data line stays defined.
  this->clock_pin_->digital_write(true);
}

void BadgerMeterComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "Badger Meter (Sensus UI-1203):");
  LOG_PIN("  Clock/power pin: ", this->clock_pin_);
  LOG_PIN("  Data pin: ", this->data_pin_);
  ESP_LOGCONFIG(TAG, "  Reset hold: %" PRIu32 " ms, power-up: %" PRIu32 " ms",
                this->reset_hold_ms_, this->power_up_time_ms_);
  ESP_LOGCONFIG(TAG, "  Read interval: %" PRIu32 " ms", this->read_interval_ms_);
  LOG_SENSOR("  ", "Meter Reading", this->meter_reading_sensor_);
  LOG_SENSOR("  ", "Raw Value", this->raw_value_sensor_);
  LOG_SENSOR("  ", "Flow Rate", this->flow_rate_sensor_);
  LOG_TEXT_SENSOR("  ", "Raw String", this->raw_string_sensor_);
  LOG_TEXT_SENSOR("  ", "Meter ID", this->meter_id_sensor_);
}

void BadgerMeterComponent::set_state_(ReadState state) {
  this->state_ = state;
  this->state_start_ms_ = millis();
}

void BadgerMeterComponent::loop() {
  const uint32_t now = millis();

  switch (this->state_) {
    case ReadState::IDLE:
      if (this->read_requested_ || now - this->last_read_ms_ >= this->read_interval_ms_) {
        this->read_requested_ = false;
        // Taking power away restarts the register's message, so the read begins at its start.
        this->clock_pin_->digital_write(false);
        this->set_state_(ReadState::RESET);
      }
      break;

    case ReadState::RESET:
      if (now - this->state_start_ms_ >= this->reset_hold_ms_) {
        this->clock_pin_->digital_write(true);
        this->set_state_(ReadState::POWER_UP);
      }
      break;

    case ReadState::POWER_UP:
      if (now - this->state_start_ms_ >= this->power_up_time_ms_) {
        this->clock_bits_();
        std::string message;
        if (this->decode_(message)) {
          this->parse_(message);
        } else {
          this->report_stuck_line_();
        }
        this->last_read_ms_ = millis();
        this->set_state_(ReadState::IDLE);
      }
      break;
  }
}

void BadgerMeterComponent::clock_bits_() {
  // The clock line is also the register's power: each bit is a short drop and a rise, and the
  // register presents the bit on the data line once power is back.
  uint32_t fed_at = micros();
  for (int i = 0; i < MAX_BITS; i++) {
    this->clock_pin_->digital_write(false);
    delayMicroseconds(CLOCK_LOW_US);
    this->clock_pin_->digital_write(true);
    delayMicroseconds(SAMPLE_AFTER_US);
    this->bits_[i] = this->data_pin_->digital_read() ? 1 : 0;
    delayMicroseconds(BIT_PERIOD_US - CLOCK_LOW_US - SAMPLE_AFTER_US);
    if (micros() - fed_at > 100000UL) {
      App.feed_wdt();
      fed_at = micros();
    }
  }
  this->num_bits_ = MAX_BITS;
}

bool BadgerMeterComponent::decode_(std::string &message) const {
  message.clear();
  int i = 0;
  while (i + FRAME_BITS <= this->num_bits_) {
    if (this->bits_[i] != 0) {  // idle between characters
      i++;
      continue;
    }
    uint8_t value = 0;
    int ones = this->bits_[i + 8];  // the parity bit
    for (int b = 0; b < 7; b++) {
      if (this->bits_[i + 1 + b] != 0) {
        value |= (uint8_t) (1U << b);
        ones++;
      }
    }
    // One bad frame fails the whole read. Dropping a character and carrying on could shift a
    // digit out of the reading, and a line nothing drives — 60 Hz pickup included — never gets
    // past its first long low run.
    if ((ones % 2) != 0 || this->bits_[i + FRAME_BITS - 1] == 0) {
      ESP_LOGW(TAG, "Framing error at bit %d, after '%s' — read discarded", i, message.c_str());
      return false;
    }
    i += FRAME_BITS;
    if (value == '\r') {
      if (!message.empty())
        return true;
      continue;
    }
    if (value < 0x20 || value >= 0x7f) {
      ESP_LOGW(TAG, "Non-printable character 0x%02X after '%s' — read discarded", value,
               message.c_str());
      return false;
    }
    message += (char) value;
  }
  ESP_LOGW(TAG, "No complete message in %d bits (got '%s') — read discarded", this->num_bits_,
           message.c_str());
  return false;
}

void BadgerMeterComponent::report_stuck_line_() const {
  int ones = 0;
  for (int i = 0; i < this->num_bits_; i++)
    ones += this->bits_[i];
  if (ones == 0 || ones == this->num_bits_) {
    ESP_LOGW(TAG, "The data line sat at %d for all %d clocks: the meter is not answering. Check "
                  "which wire is clock and which is data, and the pull-up",
             ones != 0 ? 1 : 0, this->num_bits_);
  }
}

// The value of field `tag` in a `V;RB…;IB…;…` message: everything after `;<tag>` up to the next
// ';'. False when the field is absent.
static bool find_field(const std::string &message, const char *tag, std::string &value) {
  const std::string key = std::string(";") + tag;
  const size_t start = message.find(key);
  if (start == std::string::npos)
    return false;
  const size_t from = start + key.length();
  const size_t end = message.find(';', from);
  value = message.substr(from, end == std::string::npos ? std::string::npos : end - from);
  return true;
}

// Up to nine decimal digits, optionally followed by ',' and whatever comes after it — the Sensus
// multiplier/units suffix on `RB` (`RB123456789,-1,04`), which is ignored: the scale comes from the
// sensor's own filters.
static bool parse_decimal(const std::string &text, long &value) {
  const size_t stop = text.find_first_not_of("0123456789");
  const size_t digits = stop == std::string::npos ? text.length() : stop;
  if (digits == 0 || digits > 9)
    return false;
  if (stop != std::string::npos && text[stop] != ',')
    return false;
  value = strtol(text.substr(0, digits).c_str(), nullptr, 10);
  return true;
}

void BadgerMeterComponent::parse_(const std::string &message) {
  ESP_LOGI(TAG, "Meter message: '%s'", message.c_str());
  if (this->raw_string_sensor_ != nullptr)
    this->raw_string_sensor_->publish_state(message);

  std::string value;
  long reading;
  if (!find_field(message, "RB", value) || !parse_decimal(value, reading)) {
    ESP_LOGW(TAG, "No register reading (RB) in the message — nothing published");
    return;
  }
  if (this->meter_reading_sensor_ != nullptr)
    this->meter_reading_sensor_->publish_state(static_cast<float>(reading));
  if (this->raw_value_sensor_ != nullptr)
    this->raw_value_sensor_->publish_state(static_cast<float>(reading));

  if (this->meter_id_sensor_ != nullptr && find_field(message, "IB", value))
    this->meter_id_sensor_->publish_state(value);

  // Badger E-Series `GC`: instantaneous flow. Measured against successive RB readings it reads
  // 00 idle, 01 at ~0.44 gpm and 02 at ~1.16 gpm — whole gallons per minute, rounded up, on two
  // points. Only ever seen as two decimal digits so far; if a letter ever appears the field is
  // hex and the scale above 9 is unknown, so the value is logged rather than published wrong.
  if (this->flow_rate_sensor_ != nullptr && find_field(message, "GC", value)) {
    long flow;
    if (parse_decimal(value, flow) && value.find(',') == std::string::npos) {
      this->flow_rate_sensor_->publish_state(static_cast<float>(flow));
    } else {
      ESP_LOGW(TAG, "GC field '%s' is not decimal — not published", value.c_str());
    }
  }
}

}  // namespace badger_meter
}  // namespace esphome
