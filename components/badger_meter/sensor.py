import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import sensor
from esphome.const import (
    DEVICE_CLASS_WATER,
    STATE_CLASS_MEASUREMENT,
    STATE_CLASS_TOTAL_INCREASING,
    UNIT_CUBIC_METER,
    ICON_WATER,
)

from . import BadgerMeterComponent

DEPENDENCIES = ["badger_meter"]

CONF_BADGER_METER_ID = "badger_meter_id"
CONF_METER_READING = "meter_reading"
CONF_RAW_VALUE = "raw_value"
CONF_FLOW_RATE = "flow_rate"

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
        # The E-Series `GC` field: instantaneous flow, whole gallons per minute (see docs).
        cv.Optional(CONF_FLOW_RATE): sensor.sensor_schema(
            unit_of_measurement="gal/min",
            icon="mdi:water-pump",
            accuracy_decimals=0,
            device_class="volume_flow_rate",
            state_class=STATE_CLASS_MEASUREMENT,
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

    if flow_rate_config := config.get(CONF_FLOW_RATE):
        sens = await sensor.new_sensor(flow_rate_config)
        cg.add(parent.set_flow_rate_sensor(sens))
