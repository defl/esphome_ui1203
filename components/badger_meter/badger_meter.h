#pragma once

#include "esphome/core/component.h"
#include "esphome/core/hal.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/text_sensor/text_sensor.h"

namespace esphome {
namespace badger_meter {

// Sensus UI-1203 protocol states
enum class ReadState : uint8_t {
  IDLE,
  POWER_UP,
  READING,
  POWER_DOWN,
  PARSE,
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

  void set_meter_reading_sensor(sensor::Sensor *sensor) { this->meter_reading_sensor_ = sensor; }
  void set_raw_value_sensor(sensor::Sensor *sensor) { this->raw_value_sensor_ = sensor; }
  void set_raw_string_sensor(text_sensor::TextSensor *sensor) { this->raw_string_sensor_ = sensor; }
  void set_meter_id_sensor(text_sensor::TextSensor *sensor) { this->meter_id_sensor_ = sensor; }

  // Trigger a read manually (e.g., from a lambda or button)
  void request_read() { this->read_requested_ = true; }

 protected:
  // Pin configuration
  GPIOPin *clock_pin_{nullptr};
  GPIOPin *data_pin_{nullptr};
  uint32_t power_up_time_ms_{3000};

  // State machine
  ReadState state_{ReadState::IDLE};
  uint32_t state_start_ms_{0};
  bool read_requested_{false};
  uint32_t last_read_ms_{0};
  uint32_t update_interval_ms_{60000};  // Default: read every 60s

  // Read buffer
  std::string read_buffer_;
  static const int MAX_READ_BYTES = 50;

  // Sensors
  sensor::Sensor *meter_reading_sensor_{nullptr};
  sensor::Sensor *raw_value_sensor_{nullptr};
  text_sensor::TextSensor *raw_string_sensor_{nullptr};
  text_sensor::TextSensor *meter_id_sensor_{nullptr};

  // Protocol methods
  void power_up_();
  void power_down_();
  bool read_bit_();
  uint8_t read_byte_(bool &ok);
  void read_data_();
  void parse_data_(const std::string &data);

  // Transition helper
  void set_state_(ReadState new_state);
};

}  // namespace badger_meter
}  // namespace esphome
