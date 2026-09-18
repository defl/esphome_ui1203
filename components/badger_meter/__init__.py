import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import pins
from esphome.const import CONF_ID

CODEOWNERS = ["@defl"]
MULTI_CONF = False

CONF_CLOCK_PIN = "clock_pin"
CONF_DATA_PIN = "data_pin"
CONF_POWER_UP_TIME = "power_up_time"
CONF_CAPTURE_WINDOW = "capture_window"
CONF_IDLE_GAP = "idle_gap"
CONF_READ_INTERVAL = "read_interval"
CONF_MODE = "mode"
CONF_BIT_PERIOD = "bit_period"

badger_meter_ns = cg.esphome_ns.namespace("badger_meter")
BadgerMeterComponent = badger_meter_ns.class_("BadgerMeterComponent", cg.Component)
ReadMode = badger_meter_ns.enum("ReadMode", is_class=True)
MODES = {
    # Hold the meter powered and watch the data line.
    "passive": ReadMode.PASSIVE,
    # Toggle power on the RED wire, one bit per cycle, and sample after each rising edge.
    "clocked": ReadMode.CLOCKED,
}

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(BadgerMeterComponent),
        # Optional on purpose. Configured, the pin is driven HIGH to power the meter; omitted,
        # it is never touched — which is what a meter on its own supply needs, since an output
        # pin defaults LOW and would short that supply to ground.
        cv.Optional(CONF_CLOCK_PIN): pins.gpio_output_pin_schema,
        cv.Required(CONF_DATA_PIN): pins.gpio_input_pin_schema,
        cv.Optional(CONF_POWER_UP_TIME, default="3s"): cv.positive_time_period_milliseconds,
        cv.Optional(CONF_CAPTURE_WINDOW, default="1200ms"): cv.positive_time_period_milliseconds,
        cv.Optional(CONF_IDLE_GAP, default="250ms"): cv.positive_time_period_milliseconds,
        cv.Optional(CONF_READ_INTERVAL, default="60s"): cv.positive_time_period_milliseconds,
        cv.Optional(CONF_MODE, default="passive"): cv.enum(MODES, lower=True),
        cv.Optional(CONF_BIT_PERIOD, default="1000us"): cv.positive_time_period_microseconds,
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    if CONF_CLOCK_PIN in config:
        clock_pin = await cg.gpio_pin_expression(config[CONF_CLOCK_PIN])
        cg.add(var.set_clock_pin(clock_pin))

    data_pin = await cg.gpio_pin_expression(config[CONF_DATA_PIN])
    cg.add(var.set_data_pin(data_pin))

    cg.add(var.set_power_up_time(config[CONF_POWER_UP_TIME]))
    cg.add(var.set_capture_window(config[CONF_CAPTURE_WINDOW]))
    cg.add(var.set_idle_gap(config[CONF_IDLE_GAP]))
    cg.add(var.set_read_interval(config[CONF_READ_INTERVAL]))
    cg.add(var.set_mode(config[CONF_MODE]))
    cg.add(var.set_bit_period(config[CONF_BIT_PERIOD]))
