"""`switch: platform: irrigation_core` - a relay output slot (`slot: 1..16 | master`).

The slot is driven through the core's runtime output map, so the pin behind it
can be changed from Home Assistant without reflashing.
"""

import esphome.codegen as cg
from esphome.components import switch
import esphome.config_validation as cv
from esphome.const import CONF_RESTORE_MODE

from .. import CONF_IRRIGATION_CORE_ID, CONF_SLOT, IrrigationCore, irrigation_core_ns, validate_slot

DEPENDENCIES = ["irrigation_core"]

IrrigationOutputSwitch = irrigation_core_ns.class_(
    "IrrigationOutputSwitch", switch.Switch, cg.Component
)


def _always_off(config):
    # Relays must never come back on after a reboot (global autonomy rule).
    if str(config[CONF_RESTORE_MODE]).upper() != "ALWAYS_OFF":
        raise cv.Invalid(
            f"restore_mode must be ALWAYS_OFF for irrigation_core output switches, got {config[CONF_RESTORE_MODE]}"
        )
    return config


CONFIG_SCHEMA = cv.All(
    switch.switch_schema(
        IrrigationOutputSwitch,
        block_inverted=True,
        default_restore_mode="ALWAYS_OFF",
    )
    .extend(
        {
            cv.GenerateID(CONF_IRRIGATION_CORE_ID): cv.use_id(IrrigationCore),
            cv.Required(CONF_SLOT): validate_slot,
        }
    )
    .extend(cv.COMPONENT_SCHEMA),
    _always_off,
)


async def to_code(config):
    var = await switch.new_switch(config)
    await cg.register_component(var, config)
    parent = await cg.get_variable(config[CONF_IRRIGATION_CORE_ID])
    cg.add(var.set_parent(parent))
    cg.add(var.set_slot(config[CONF_SLOT]))
