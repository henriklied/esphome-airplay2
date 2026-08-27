import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID

DEPENDENCIES = ["network"]
AUTO_LOAD = ["audio"]

airplay_receiver_ns = cg.esphome_ns.namespace("airplay_receiver")
AirPlayReceiver = airplay_receiver_ns.class_("AirPlayReceiver", cg.Component)

_CONF_NAME = "name"
_CONF_BUFFER_SIZE = "buffer_size"

CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(AirPlayReceiver),
            cv.Optional(_CONF_NAME, default="AirPlay2"): cv.string,
            cv.Optional(_CONF_BUFFER_SIZE, default=1000000): cv.int_,
        }
    ).extend(cv.COMPONENT_SCHEMA),
    cv.only_with_framework("esp-idf"),
    cv.only_on_esp32,
)


async def to_code(config):
    # TEMP EXP: enable recursive source collection so subdir .cpp compile.
    import esphome.loader as _loader
    manif = _loader._COMPONENT_CACHE.get("airplay_receiver")
    if manif is not None:
        manif.recursive_sources = True
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_name(config[_CONF_NAME]))
    cg.add(var.set_buffer_size(config[_CONF_BUFFER_SIZE]))
