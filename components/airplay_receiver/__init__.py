import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import pins
from esphome.components import esp32 as esp32_platform
# The component is itself the media_player (it inherits media_player::MediaPlayer
# and registers via register_media_player). It is the sole entry point — there is
# no separate `media_player: - platform: airplay_receiver` platform, which would
# otherwise build a second instance (two RTSP servers on :7000, two I2S claims).
from esphome.components import media_player as media_player_mod
from esphome.components.esp32 import add_idf_component, include_builtin_idf_component
from esphome.components.esp32.const import VARIANT_ESP32, VARIANT_ESP32S3
from esphome.const import CONF_ID

# The AirPlay receiver wants the I2S bus. ESPHome's I2S driver is excluded by
# default (DEFAULT_EXCLUDED_IDF_COMPONENTS), so we must re-enable it exactly as
# components/i2s_audio/__init__.py does, or audio/audio_output.cpp fails at the
# #include <driver/i2s_std.h>.
DEPENDENCIES = ["network"]
AUTO_LOAD = ["media_player", "mdns"]

# AirPlay and the vendor Sendspin media firmware both want I2S_NUM_0 on this
# class of board; an i2s_audio speaker likewise claims I2S_NUM_0. For a config
# that would run any of them alongside this receiver, fail clearly at validation
# (ESPHome enforces CONFLICTS_WITH while building the component graph) rather
# than at runtime with only an ESP_LOGE.
CONFLICTS_WITH = ["sendspin", "i2s_audio"]

airplay_receiver_ns = cg.esphome_ns.namespace("airplay_receiver")
AirPlayReceiver = airplay_receiver_ns.class_("AirPlayReceiver", cg.Component, media_player_mod.MediaPlayer)

_CONFIG_AUDIO_CHANNEL_MODE = "audio_channel_mode"
_CONFIG_BUFFER_SIZE = "buffer_size"
_CONFIG_I2S_BCLK = "i2s_bclk_pin"
_CONFIG_I2S_LRCLK = "i2s_lrclk_pin"
_CONFIG_I2S_DOUT = "i2s_dout_pin"
_CONFIG_I2S_MCLK = "i2s_mclk_pin"
_CONFIG_AMP_ENABLE = "amp_enable_pin"
_CONFIG_SAMPLE_RATE = "sample_rate"
_CONFIG_AMP_INVERTED = "amp_enable_inverted"
_CONFIG_AMP_IDLE_TIMEOUT = "amp_idle_timeout"
_CONFIG_DSP = "dsp"
_CONFIG_DSP_ENABLED = "enabled"
_CONFIG_DSP_PREAMP = "preamp"
_CONFIG_DSP_FILTERS = "filters"
_CONFIG_FILTER_TYPE = "type"
_CONFIG_FILTER_FREQUENCY = "frequency"
_CONFIG_FILTER_Q = "q"
_CONFIG_FILTER_GAIN = "gain"

# Output channel mode -> audio_channel_mode_t (audio/audio_output.h): 0=STEREO
# 1=LEFT 2=RIGHT 3=MONO. Applied after audio_output_init() so the AuMONO
# channel modes are reachable from YAML / Home Assistant.
AUDIO_CHANNEL_MODE_ENUM = {
    "stereo": 0,
    "left": 1,
    "right": 2,
    "mono": 3,
}


# Output DSP biquad shapes -> airplay_dsp_filter_type_t (audio/audio_dsp.h).
# Keep the two in step; the integer is what reaches add_dsp_filter().
DSP_FILTER_TYPE_ENUM = {
    "low_shelf": 0,
    "high_shelf": 1,
    "high_pass": 2,
    "low_pass": 3,
    "peaking": 4,
    "notch": 5,
}

# Filter shapes with no gain term. Passing one a `gain:` means the config does
# not do what it reads like, so reject it rather than silently ignoring it.
_GAINLESS_FILTER_TYPES = ("high_pass", "low_pass", "notch")

# Mirrors AIRPLAY_DSP_MAX_FILTERS in audio/audio_dsp.h.
_DSP_MAX_FILTERS = 8

# Butterworth. The usual choice for a protective high-pass, and what Music
# Assistant's DSP defaults to.
_DSP_DEFAULT_Q = 0.7071067811865476

# A corner above this fraction of the sample rate has no usable response left,
# and the biquad design degenerates near Nyquist. Mirrors the runtime guard.
_DSP_MAX_FREQUENCY_FRACTION = 0.49


def _validate_dsp_filter(config):
    """Reject a `gain:` on a filter shape that has no gain term."""
    if config[_CONFIG_FILTER_TYPE] in _GAINLESS_FILTER_TYPES and config[_CONFIG_FILTER_GAIN] != 0.0:
        raise cv.Invalid(
            f"a {config[_CONFIG_FILTER_TYPE]} filter has no gain term; "
            f"remove '{_CONFIG_FILTER_GAIN}:' or use a shelf/peaking filter",
            path=[_CONFIG_FILTER_GAIN],
        )
    return config


_DSP_FILTER_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.Required(_CONFIG_FILTER_TYPE): cv.enum(DSP_FILTER_TYPE_ENUM, lower=True),
            cv.Required(_CONFIG_FILTER_FREQUENCY): cv.frequency,
            cv.Optional(_CONFIG_FILTER_Q, default=_DSP_DEFAULT_Q): cv.float_range(min=0.05, max=20.0),
            cv.Optional(_CONFIG_FILTER_GAIN, default="0dB"): cv.decibel,
        }
    ),
    _validate_dsp_filter,
)

# Output DSP. Direct AirPlay never passes through Music Assistant, so the
# speaker correction MA would have applied has to run on the board instead.
_DSP_SCHEMA = cv.Schema(
    {
        cv.Optional(_CONFIG_DSP_ENABLED, default=True): cv.boolean,
        # Negative preamp buys back the headroom a positive shelf spends. The
        # component warns at boot if the configured boost exceeds it.
        cv.Optional(_CONFIG_DSP_PREAMP, default="0dB"): cv.decibel,
        cv.Optional(_CONFIG_DSP_FILTERS, default=[]): cv.All(
            cv.ensure_list(_DSP_FILTER_SCHEMA), cv.Length(max=_DSP_MAX_FILTERS)
        ),
    }
)


def _validate_dsp_against_sample_rate(config):
    """Catch a filter corner too close to Nyquist at validation rather than
    letting the runtime drop the section with only a log line."""
    dsp = config.get(_CONFIG_DSP)
    if not dsp:
        return config
    limit = config[_CONFIG_SAMPLE_RATE] * _DSP_MAX_FREQUENCY_FRACTION
    for index, filter_config in enumerate(dsp[_CONFIG_DSP_FILTERS]):
        if filter_config[_CONFIG_FILTER_FREQUENCY] > limit:
            raise cv.Invalid(
                f"{filter_config[_CONFIG_FILTER_FREQUENCY]:.0f} Hz is above the usable limit "
                f"({limit:.0f} Hz) for a {config[_CONFIG_SAMPLE_RATE]} Hz output",
                path=[_CONFIG_DSP, _CONFIG_DSP_FILTERS, index, _CONFIG_FILTER_FREQUENCY],
            )
    return config


def _optional_output_pin(value):
    """Validate a GPIO pin as an internal output pin, but keep the -1 'unused'
    sentinel for optional pins (defaults in the YAML schema)."""
    if value == -1:
        return -1
    return pins.internal_gpio_output_pin_number(value)


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.Optional(_CONFIG_BUFFER_SIZE, default=1000000): cv.int_,
            # I2S external DAC (PCM5100) + amp-enable wiring. Defaults (-1) leave
            # the output stage unconfigured until the YAML supplies the pins. Pins
            # go through the pin registry so range/type + cross-component GPIO
            # conflicts are caught at validation.
            cv.Optional(_CONFIG_I2S_BCLK, default=-1): _optional_output_pin,
            cv.Optional(_CONFIG_I2S_LRCLK, default=-1): _optional_output_pin,
            cv.Optional(_CONFIG_I2S_DOUT, default=-1): _optional_output_pin,
            # Optional MCLK/SCK for PCM5100 boards that don't self-strap (-1 = unused).
            cv.Optional(_CONFIG_I2S_MCLK, default=-1): _optional_output_pin,
            cv.Optional(_CONFIG_AMP_ENABLE, default=-1): _optional_output_pin,
            cv.Optional(_CONFIG_SAMPLE_RATE, default=44100): cv.int_,
            cv.Optional(_CONFIG_AMP_INVERTED, default=False): cv.boolean,
            # Seconds of no audio (pause/idle) before the amp-enable line is
            # de-asserted to mute the amplifier. 0 disables the power-down
            # watchdog (amp stays on for the whole session). Default 60.
            cv.Optional(_CONFIG_AMP_IDLE_TIMEOUT, default=60): cv.int_,
            cv.Optional(_CONFIG_AUDIO_CHANNEL_MODE, default="stereo"): cv.enum(AUDIO_CHANNEL_MODE_ENUM, lower=True),
            cv.Optional(_CONFIG_DSP): _DSP_SCHEMA,
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
    .extend(media_player_mod.media_player_schema(AirPlayReceiver)),
    _validate_dsp_against_sample_rate,
    cv.only_with_framework("esp-idf"),
    cv.only_on_esp32,
)


async def to_code(config):
    await _build_airplay_receiver(config)


async def _build_airplay_receiver(config):
    """Build the AirPlayReceiver variable, wire the hub, and register its
    embedded media_player. The component is the single entry point — both the
    `airplay_receiver:` config and (historically) a media_player platform built
    identical instances, so only `airplay_receiver:` is supported now."""
    _register_recursive_sources()
    _add_memory_policy_flags()
    _add_lwip_requirements()
    # Re-enable ESP-IDF's I2S driver (excluded by default to save compile time).
    include_builtin_idf_component("esp_driver_i2s")
    # The HAP pairing / ChaCha20-Poly1305 audio crypto needs libsodium (Ed25519,
    # X25519, ChaCha20-Poly1305, SHA-512/HMAC) pulled in as a managed component.
    # mbedtls (SRP bignum + raw-signature AES-CTR) is a built-in IDF component.
    # Caret ranges over published Espressif Component Registry versions.
    add_idf_component(name="espressif/libsodium", ref="^1.0.21")
    # ALAC/AAC decoding (audio engine) comes from the ESP-ADF codec library.
    add_idf_component(name="espressif/esp_audio_codec", ref="^2.5.0")

    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    # Also register as a media_player entity so Home Assistant gets volume,
    # play/pause/stop/transport state. (Entity name comes from the schema.)
    await media_player_mod.register_media_player(var, config)

    cg.add(var.set_buffer_size(config[_CONFIG_BUFFER_SIZE]))
    cg.add(var.set_audio_config(config[_CONFIG_I2S_BCLK], config[_CONFIG_I2S_LRCLK], config[_CONFIG_I2S_DOUT],
                                config[_CONFIG_AMP_ENABLE], config[_CONFIG_SAMPLE_RATE],
                                config[_CONFIG_AMP_INVERTED]))
    cg.add(var.set_amp_idle_timeout(config[_CONFIG_AMP_IDLE_TIMEOUT]))
    cg.add(var.set_i2s_mclk(config[_CONFIG_I2S_MCLK]))
    # cv.enum returns the string key ("stereo"/"mono"...), so map to the
    # audio_channel_mode_t integer before the C++ setter.
    cg.add(var.set_audio_channel_mode(AUDIO_CHANNEL_MODE_ENUM[config[_CONFIG_AUDIO_CHANNEL_MODE]]))
    _add_dsp(var, config.get(_CONFIG_DSP))
    return var


def _add_dsp(var, dsp_config) -> None:
    """Emit the output DSP cascade in YAML order.

    Filters are only collected here; the component publishes the whole cascade
    in setup(), once audio_output_init() has told the DSP stage the real output
    sample rate. Designing coefficients before that would use a guessed rate.
    """
    if dsp_config is None:
        return
    cg.add(var.set_dsp_enabled(dsp_config[_CONFIG_DSP_ENABLED]))
    cg.add(var.set_dsp_preamp(dsp_config[_CONFIG_DSP_PREAMP]))
    for filter_config in dsp_config[_CONFIG_DSP_FILTERS]:
        cg.add(
            var.add_dsp_filter(
                DSP_FILTER_TYPE_ENUM[filter_config[_CONFIG_FILTER_TYPE]],
                filter_config[_CONFIG_FILTER_FREQUENCY],
                filter_config[_CONFIG_FILTER_Q],
                filter_config[_CONFIG_FILTER_GAIN],
            )
        )


# The IDF defaults for both of these are too small for a realtime AirPlay
# receiver, and both fail *silently* -- the receiver keeps its RTSP socket, so
# metadata and transport still work while the audio is wrong or absent. Set them
# here rather than documenting them, so the component works when dropped into a
# config that knows nothing about either.
_LWIP_MAX_SOCKETS = 24
_LWIP_UDP_RECVMBOX_SIZE = 32


def _add_lwip_requirements() -> None:
    # ~15 sockets are held during playback: RTSP listener + client, event port
    # listener + client, PTP on 319/320, RTP data/control/timing, plus the
    # ESPHome API listener, its HA client, any `esphome logs` client, OTA and
    # mDNS. At the IDF default of 10 the control and timing ports fail to bind
    # and SETUP goes out with controlPort=0.
    esp32_platform.add_idf_sdkconfig_option("CONFIG_LWIP_MAX_SOCKETS", _LWIP_MAX_SOCKETS)

    # This is a slot count, not a byte budget. The IDF default of 6 is ~48ms at
    # the 125 packet/s ALAC rate (352 frames per packet), and a burst deeper
    # than that is discarded by sys_mbox_trypost() in recv_udp() with no counter
    # anywhere -- the packet was received off the air and then dropped inside
    # the stack, surfacing only as an RTP sequence gap that is indistinguishable
    # from loss on air. Measured on an Amped-ESP32-S3: 931 concealment events in
    # 75s at the default, zero at 32. 32 matches ESP_WIFI_DYNAMIC_RX_BUFFER_NUM,
    # which caps what can be in flight anyway.
    #
    # Note SO_RCVBUF cannot substitute for this: CONFIG_LWIP_SO_RCVBUF is off by
    # default, so lwIP compiles the option out and setsockopt() silently fails.
    esp32_platform.add_idf_sdkconfig_option("CONFIG_LWIP_UDP_RECVMBOX_SIZE", _LWIP_UDP_RECVMBOX_SIZE)


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
