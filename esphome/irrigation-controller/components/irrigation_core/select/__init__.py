"""`select: platform: irrigation_core` - picks the output behind a slot (`slot: 1..16 | master`).

Options come from the core at runtime (`output_options(candidates, has_expander)`),
which a template select cannot do because its options are static.
"""

import esphome.codegen as cg
from esphome.components import select
import esphome.config_validation as cv

from .. import CONF_IRRIGATION_CORE_ID, CONF_SLOT, IrrigationCore, irrigation_core_ns, validate_slot

DEPENDENCIES = ["irrigation_core"]

IrrigationOutputSelect = irrigation_core_ns.class_(
    "IrrigationOutputSelect", select.Select, cg.Component
)

CONFIG_SCHEMA = (
    select.select_schema(IrrigationOutputSelect, entity_category="config")
    .extend(
        {
            cv.GenerateID(CONF_IRRIGATION_CORE_ID): cv.use_id(IrrigationCore),
            cv.Required(CONF_SLOT): validate_slot,
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
)


async def to_code(config):
    var = await select.new_select(config, options=[])
    await cg.register_component(var, config)
    parent = await cg.get_variable(config[CONF_IRRIGATION_CORE_ID])
    cg.add(var.set_parent(parent))
    cg.add(var.set_slot(config[CONF_SLOT]))
