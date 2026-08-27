import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import esp32 as esp32_platform
from esphome.components.esp32 import add_idf_component
from esphome.components.esp32.const import VARIANT_ESP32, VARIANT_ESP32S3
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
    _register_recursive_sources()
    _add_memory_policy_flags()
    # The HAP pairing / ChaCha20-Poly1305 audio crypto needs libsodium (Ed25519,
    # X25519, ChaCha20-Poly1305, SHA-512/HMAC) pulled in as a managed component.
    # mbedtls (SRP bignum + raw-signature AES-CTR) is a built-in IDF component.
    add_idf_component(name="espressif/libsodium", ref="1.0.21")
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_name(config[_CONF_NAME]))
    cg.add(var.set_buffer_size(config[_CONF_BUFFER_SIZE]))


def _register_recursive_sources() -> None:
    """Let ESPHome collect this component's one-level subdir sources.

    ESPHome's external_components file collection only copies a component's
    top-level source files by default (recursive_sources=False). The
    airplay_receiver modules live under transport/, crypto/, decoder/, timing/
    and audio/, so we opt this component into one-level recursive collection.
    This runs in to_code(), which is finished before copy_src_tree() executes,
    so the subdir .cpp files land in the generated build tree. Gracefully
    no-ops if the ESPHome manifest cache API changes.
    """
    try:
        import esphome.loader as _loader

        manifest = _loader._COMPONENT_CACHE.get("airplay_receiver")
        if manifest is not None:
            manifest.recursive_sources = True
    except Exception:  # pragma: no cover - defensive against API drift
        pass


def _add_memory_policy_flags() -> None:
    """Emit the active platform-profile build flag + heap-trace defines.

    The AIRPLAY_PLATFORM_* macro selects which memory/tuning profile
    (platform/esp32s3/config.h vs platform/esp32/config.h) the allocator and
    task-stack code use. USE_AIRPLAY_HEAP_TRACE enables the allocation
    reporters (airplay_internal_free/.../airplay_psram_free).
    """
    variant = esp32_platform.get_esp32_variant()
    if variant == VARIANT_ESP32S3:
        cg.add_build_flag("-DAIRPLAY_PLATFORM_ESP32S3")
    else:
        cg.add_build_flag("-DAIRPLAY_PLATFORM_ESP32")
    cg.add_define("USE_AIRPLAY_HEAP_TRACE")
