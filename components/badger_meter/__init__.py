import logging

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import pins
from esphome.const import CONF_ID

CODEOWNERS = ["@defl"]
MULTI_CONF = False

_LOGGER = logging.getLogger(__name__)

CONF_CLOCK_PIN = "clock_pin"
CONF_DATA_PIN = "data_pin"
CONF_POWER_UP_TIME = "power_up_time"
CONF_RESET_HOLD = "reset_hold"
CONF_READ_INTERVAL = "read_interval"

# Options from the diagnostic build that no longer do anything. Still accepted, with a warning,
# so configs copied from the earlier README keep compiling.
REMOVED_OPTIONS = ("mode", "bit_period", "capture_window", "idle_gap")

badger_meter_ns = cg.esphome_ns.namespace("badger_meter")
BadgerMeterComponent = badger_meter_ns.class_("BadgerMeterComponent", cg.Component)


def _warn_removed_options(config):
    for option in REMOVED_OPTIONS:
        if option in config:
            _LOGGER.warning(
                "badger_meter: '%s' no longer has any effect and can be removed", option
            )
    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(BadgerMeterComponent),
            # Driven directly: this line is both the register's clock and its power.
            cv.Required(CONF_CLOCK_PIN): pins.gpio_output_pin_schema,
            cv.Required(CONF_DATA_PIN): pins.gpio_input_pin_schema,
            cv.Optional(CONF_POWER_UP_TIME, default="3s"): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_RESET_HOLD, default="1200ms"): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_READ_INTERVAL, default="60s"): cv.positive_time_period_milliseconds,
            # Passive capture is gone, so the only mode left is the only one accepted.
            cv.Optional("mode"): cv.one_of("clocked", lower=True),
            cv.Optional("bit_period"): cv.valid,
            cv.Optional("capture_window"): cv.valid,
            cv.Optional("idle_gap"): cv.valid,
        }
    ).extend(cv.COMPONENT_SCHEMA),
    _warn_removed_options,
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    clock_pin = await cg.gpio_pin_expression(config[CONF_CLOCK_PIN])
    cg.add(var.set_clock_pin(clock_pin))
    data_pin = await cg.gpio_pin_expression(config[CONF_DATA_PIN])
    cg.add(var.set_data_pin(data_pin))

    cg.add(var.set_power_up_time(config[CONF_POWER_UP_TIME]))
    cg.add(var.set_reset_hold(config[CONF_RESET_HOLD]))
    cg.add(var.set_read_interval(config[CONF_READ_INTERVAL]))
