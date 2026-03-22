import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import text_sensor

from . import BadgerMeterComponent

DEPENDENCIES = ["badger_meter"]

CONF_BADGER_METER_ID = "badger_meter_id"
CONF_RAW_STRING = "raw_string"
CONF_METER_ID = "meter_id"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_BADGER_METER_ID): cv.use_id(BadgerMeterComponent),
        cv.Optional(CONF_RAW_STRING): text_sensor.text_sensor_schema(),
        cv.Optional(CONF_METER_ID): text_sensor.text_sensor_schema(),
    }
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_BADGER_METER_ID])

    if raw_string_config := config.get(CONF_RAW_STRING):
        sens = await text_sensor.new_text_sensor(raw_string_config)
        cg.add(parent.set_raw_string_sensor(sens))

    if meter_id_config := config.get(CONF_METER_ID):
        sens = await text_sensor.new_text_sensor(meter_id_config)
        cg.add(parent.set_meter_id_sensor(sens))
