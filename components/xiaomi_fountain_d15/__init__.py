"""Header-only helper for the Xiaomi Smart Pet Fountain, board EP-1158-EMC-D15.

Adding `xiaomi_fountain_d15:` to a configuration makes ESPHome copy xiaomi_fountain_d15.h into the build;
the YAML package (esphome/EP-1158-EMC-D15.yaml) drives it from lambdas.
"""

import esphome.codegen as cg
import esphome.config_validation as cv

CODEOWNERS = ["@deceju"]

CONFIG_SCHEMA = cv.Schema({})


async def to_code(config):
    cg.add_global(cg.RawStatement('#include "esphome/components/xiaomi_fountain_d15/xiaomi_fountain_d15.h"'))
