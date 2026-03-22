import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import pins
from esphome.const import CONF_ID

CODEOWNERS = ["@defl"]
MULTI_CONF = False

CONF_CLOCK_PIN = "clock_pin"
CONF_DATA_PIN = "data_pin"
CONF_POWER_UP_TIME = "power_up_time"

badger_meter_ns = cg.esphome_ns.namespace("badger_meter")
BadgerMeterComponent = badger_meter_ns.class_("BadgerMeterComponent", cg.Component)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(BadgerMeterComponent),
        cv.Required(CONF_CLOCK_PIN): pins.gpio_output_pin_schema,
        cv.Required(CONF_DATA_PIN): pins.gpio_input_pin_schema,
        cv.Optional(CONF_POWER_UP_TIME, default="3s"): cv.positive_time_period_milliseconds,
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    clock_pin = await cg.gpio_pin_expression(config[CONF_CLOCK_PIN])
    cg.add(var.set_clock_pin(clock_pin))

    data_pin = await cg.gpio_pin_expression(config[CONF_DATA_PIN])
    cg.add(var.set_data_pin(data_pin))

    cg.add(var.set_power_up_time(config[CONF_POWER_UP_TIME]))
