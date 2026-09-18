#pragma once

#include "esphome/core/component.h"
#include "esphome/core/hal.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/text_sensor/text_sensor.h"

#include <string>

namespace esphome {
namespace badger_meter {

// The meter transmits on its own once powered, in bursts separated by seconds of idle. A
// fixed-window capture lands in the gap and reports silence, so the capture is edge-triggered:
// ARMED samples the data pin once per loop and hands over to a blocking CAPTURE the moment the
// line moves.
enum class ReadState : uint8_t {
  IDLE,
  ARMED,
  CAPTURE,
  PARSE,
};

// One decode attempt: a bit period, a polarity and a framing, scored by how much of the capture
// it turns into well-framed printable characters.
struct DecodeResult {
  std::string text;
  uint32_t bit_us{0};
  int chars{0};
  int errors{0};
  int data_bits{0};
  bool inverted{false};
  bool parity{false};

  int score() const { return this->chars - this->errors; }
};

class BadgerMeterComponent : public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  // Optional: only set when the ESP powers the meter from a GPIO. Left unset the pin is never
  // touched, which is what a meter fed from its own supply needs — driving a pin low against an
  // external supply would be a short.
  void set_clock_pin(GPIOPin *pin) { this->clock_pin_ = pin; }
  void set_data_pin(GPIOPin *pin) { this->data_pin_ = pin; }
  void set_power_up_time(uint32_t ms) { this->power_up_time_ms_ = ms; }
  void set_capture_window(uint32_t ms) { this->capture_window_ms_ = ms; }
  void set_idle_gap(uint32_t ms) { this->idle_gap_ms_ = ms; }
  void set_read_interval(uint32_t ms) { this->update_interval_ms_ = ms; }

  void set_meter_reading_sensor(sensor::Sensor *sensor) { this->meter_reading_sensor_ = sensor; }
  void set_raw_value_sensor(sensor::Sensor *sensor) { this->raw_value_sensor_ = sensor; }
  void set_raw_string_sensor(text_sensor::TextSensor *sensor) { this->raw_string_sensor_ = sensor; }
  void set_meter_id_sensor(text_sensor::TextSensor *sensor) { this->meter_id_sensor_ = sensor; }

  // Trigger a read manually (e.g., from a lambda or button)
  void request_read() { this->read_requested_ = true; }

 protected:
  GPIOPin *clock_pin_{nullptr};
  GPIOPin *data_pin_{nullptr};
  uint32_t power_up_time_ms_{3000};
  uint32_t capture_window_ms_{1200};
  uint32_t idle_gap_ms_{250};
  uint32_t update_interval_ms_{60000};

  ReadState state_{ReadState::IDLE};
  uint32_t state_start_ms_{0};
  bool read_requested_{false};
  uint32_t last_read_ms_{0};
  bool armed_level_{true};

  // Capture buffer. Timestamps are offsets from the first recorded edge, so micros() rolling
  // over mid-capture cannot reorder them.
  static const int MAX_TRANSITIONS = 512;
  struct Transition {
    uint32_t offset_us;
    bool level;
  };
  Transition transitions_[MAX_TRANSITIONS];
  int num_transitions_{0};

  std::string read_buffer_;

  sensor::Sensor *meter_reading_sensor_{nullptr};
  sensor::Sensor *raw_value_sensor_{nullptr};
  text_sensor::TextSensor *raw_string_sensor_{nullptr};
  text_sensor::TextSensor *meter_id_sensor_{nullptr};

  void capture_();
  void report_();
  DecodeResult decode_best_();
  bool level_at_(uint32_t offset_us) const;
  DecodeResult decode_once_(uint32_t bit_us, bool inverted, int data_bits, bool parity) const;
  void parse_data_(const std::string &data);
  void set_state_(ReadState new_state);
};

}  // namespace badger_meter
}  // namespace esphome
