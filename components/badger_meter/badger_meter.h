#pragma once

#include "esphome/core/component.h"
#include "esphome/core/hal.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/text_sensor/text_sensor.h"

#include <string>

namespace esphome {
namespace badger_meter {

// A read: hold the clock low to reset the register (RESET), hold it high so the register powers up
// (POWER_UP), then clock the message out and parse it in one step. RESET and POWER_UP wait
// without blocking; only the clocking itself blocks.
enum class ReadState : uint8_t {
  IDLE,
  RESET,
  POWER_UP,
};

class BadgerMeterComponent : public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  void set_clock_pin(GPIOPin *pin) { this->clock_pin_ = pin; }
  void set_data_pin(GPIOPin *pin) { this->data_pin_ = pin; }
  void set_power_up_time(uint32_t ms) { this->power_up_time_ms_ = ms; }
  void set_reset_hold(uint32_t ms) { this->reset_hold_ms_ = ms; }
  void set_read_interval(uint32_t ms) { this->read_interval_ms_ = ms; }

  void set_meter_reading_sensor(sensor::Sensor *sensor) { this->meter_reading_sensor_ = sensor; }
  void set_raw_value_sensor(sensor::Sensor *sensor) { this->raw_value_sensor_ = sensor; }
  void set_flow_rate_sensor(sensor::Sensor *sensor) { this->flow_rate_sensor_ = sensor; }
  void set_raw_string_sensor(text_sensor::TextSensor *sensor) { this->raw_string_sensor_ = sensor; }
  void set_meter_id_sensor(text_sensor::TextSensor *sensor) { this->meter_id_sensor_ = sensor; }

  // Read on the next loop instead of waiting for the interval, e.g. from a button.
  void request_read() { this->read_requested_ = true; }

 protected:
  GPIOPin *clock_pin_{nullptr};
  GPIOPin *data_pin_{nullptr};
  uint32_t power_up_time_ms_{3000};
  uint32_t reset_hold_ms_{1200};
  uint32_t read_interval_ms_{60000};

  ReadState state_{ReadState::IDLE};
  uint32_t state_start_ms_{0};
  uint32_t last_read_ms_{0};
  bool read_requested_{false};

  static const int MAX_BITS = 1000;
  uint8_t bits_[MAX_BITS]{};
  int num_bits_{0};

  sensor::Sensor *meter_reading_sensor_{nullptr};
  sensor::Sensor *raw_value_sensor_{nullptr};
  sensor::Sensor *flow_rate_sensor_{nullptr};
  text_sensor::TextSensor *raw_string_sensor_{nullptr};
  text_sensor::TextSensor *meter_id_sensor_{nullptr};

  void set_state_(ReadState state);
  void clock_bits_();
  bool decode_(std::string &message) const;
  void report_stuck_line_() const;
  void parse_(const std::string &message);
};

}  // namespace badger_meter
}  // namespace esphome
