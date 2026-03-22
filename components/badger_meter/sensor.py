import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import sensor
from esphome.const import (
    CONF_ID,
    DEVICE_CLASS_WATER,
    STATE_CLASS_TOTAL_INCREASING,
    UNIT_CUBIC_METER,
    ICON_WATER,
)

from . import BadgerMeterComponent, CONF_ID as BADGER_CONF_ID, badger_meter_ns

DEPENDENCIES = ["badger_meter"]

CONF_BADGER_METER_ID = "badger_meter_id"
CONF_METER_READING = "meter_reading"
CONF_RAW_VALUE = "raw_value"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_BADGER_METER_ID): cv.use_id(BadgerMeterComponent),
        cv.Optional(CONF_METER_READING): sensor.sensor_schema(
            unit_of_measurement=UNIT_CUBIC_METER,
            icon=ICON_WATER,
            accuracy_decimals=1,
            device_class=DEVICE_CLASS_WATER,
            state_class=STATE_CLASS_TOTAL_INCREASING,
        ),
        cv.Optional(CONF_RAW_VALUE): sensor.sensor_schema(
            accuracy_decimals=0,
            icon=ICON_WATER,
        ),
    }
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_BADGER_METER_ID])

    if meter_reading_config := config.get(CONF_METER_READING):
        sens = await sensor.new_sensor(meter_reading_config)
        cg.add(parent.set_meter_reading_sensor(sens))

    if raw_value_config := config.get(CONF_RAW_VALUE):
        sens = await sensor.new_sensor(raw_value_config)
        cg.add(parent.set_raw_value_sensor(sens))
