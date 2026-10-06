import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID

DEPENDENCIES = ["airplay_receiver"]

fdk_bench_ns = cg.esphome_ns.namespace("fdk_bench")
FdkBench = fdk_bench_ns.class_("FdkBench", cg.Component)

CONFIG_SCHEMA = cv.Schema({cv.GenerateID(): cv.declare_id(FdkBench)}).extend(cv.COMPONENT_SCHEMA)

WRAPPED = [
    "_Z20CChannelElement_ReadP13FDK_BITSTREAMPP22CAacDecoderChannelInfoPP28CAacDecoderStaticChannelInfo17AUDIO_OBJECT_TYPEP16SamplingRateInfojjjhaP12TRANSPORTDEC",
    "_Z22CChannelElement_DecodePP22CAacDecoderChannelInfoPP28CAacDecoderStaticChannelInfoP16SamplingRateInfojji",
    "_Z22CBlock_FrequencyToTimeP28CAacDecoderStaticChannelInfoP22CAacDecoderChannelInfoPlsiS3_iji",
    "pcmDmx_ApplyFrame",
]


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    for symbol in WRAPPED:
        cg.add_build_flag(f"-Wl,--wrap={symbol}")
