"""irrigation_core: programs, zones, settings and the runtime relay map in NVS.

The pure scheduling/sequencing logic (types, civil, schedule, planner, runner,
flow, runlog, pinmap, buttons) sits flat in this directory because ESPHome only
compiles the top-level sources of an external component. irrigation_core.h/.cpp
is the ESPHome glue around it.
"""

from esphome import automation
import esphome.codegen as cg
from esphome.components import mcp23x17_base, sensor, sprinkler, text
from esphome.components import time as time_
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_ON_EVENT, CONF_TIME_ID, CONF_TRIGGER_ID

DEPENDENCIES = ["sprinkler", "time"]

CONF_IRRIGATION_CORE_ID = "irrigation_core_id"
CONF_SPRINKLER_ID = "sprinkler_id"
CONF_FLOW_SENSOR = "flow_sensor"
CONF_OUTPUT_CANDIDATES = "output_candidates"
CONF_RESERVED_PINS = "reserved_pins"
CONF_EXPANDER_ID = "expander_id"
CONF_DEFAULT_OUTPUTS = "default_outputs"
CONF_DEFAULT_MASTER_OUTPUT = "default_master_output"
CONF_PROGRAM_NAME_TEXTS = "program_name_texts"
CONF_ZONE_NAME_TEXTS = "zone_name_texts"
CONF_SLOT = "slot"

NUM_ZONES = 16
NUM_PROGRAMS = 8
MASTER_SLOT = 16  # internal slot index of the master output

irrigation_core_ns = cg.esphome_ns.namespace("irrigation_core")
IrrigationCore = irrigation_core_ns.class_("IrrigationCore", cg.Component)
EventTrigger = irrigation_core_ns.class_(
    "EventTrigger", automation.Trigger.template(cg.std_string, cg.std_string)
)

GPIO_NUM = cv.int_range(min=0, max=30)
# Output codes: -1 none, 0..30 board GPIO, 100..115 MCP23017 A0..B7.
OUTPUT_CODE = cv.Any(cv.int_range(min=-1, max=30), cv.int_range(min=100, max=115))


def validate_slot(value):
    """`slot: 1..16 | master` -> internal index 0..15, master 16."""
    if isinstance(value, str) and value.strip().lower() == "master":
        return MASTER_SLOT
    return cv.int_range(min=1, max=NUM_ZONES)(cv.int_(value)) - 1


def _validate_pins(config):
    candidates = config[CONF_OUTPUT_CANDIDATES]
    reserved = config[CONF_RESERVED_PINS]
    for pin in candidates:
        if pin in reserved:
            raise cv.Invalid(f"pin {pin} is both an output candidate and reserved")
    has_expander = CONF_EXPANDER_ID in config
    defaults = list(config[CONF_DEFAULT_OUTPUTS]) + [config[CONF_DEFAULT_MASTER_OUTPUT]]
    seen = set()
    for code in defaults:
        if code == -1:
            continue
        if code >= 100:
            if not has_expander:
                raise cv.Invalid(
                    f"default output {code} is an MCP channel but no expander_id is set"
                )
        elif code not in candidates:
            raise cv.Invalid(f"default output GPIO {code} is not an output candidate")
        if code in seen:
            raise cv.Invalid(f"default output {code} is used twice")
        seen.add(code)
    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(IrrigationCore),
            cv.Required(CONF_SPRINKLER_ID): cv.use_id(sprinkler.Sprinkler),
            cv.Required(CONF_TIME_ID): cv.use_id(time_.RealTimeClock),
            cv.Optional(CONF_FLOW_SENSOR): cv.use_id(sensor.Sensor),
            cv.Required(CONF_OUTPUT_CANDIDATES): cv.All(
                cv.ensure_list(GPIO_NUM), cv.Length(min=1)
            ),
            cv.Optional(CONF_RESERVED_PINS, default=[]): cv.ensure_list(GPIO_NUM),
            cv.Optional(CONF_EXPANDER_ID): cv.use_id(mcp23x17_base.MCP23X17Base),
            cv.Optional(CONF_DEFAULT_OUTPUTS, default=[]): cv.All(
                cv.ensure_list(OUTPUT_CODE), cv.Length(max=NUM_ZONES)
            ),
            cv.Optional(CONF_DEFAULT_MASTER_OUTPUT, default=-1): OUTPUT_CODE,
            cv.Optional(CONF_PROGRAM_NAME_TEXTS): cv.All(
                cv.ensure_list(cv.use_id(text.Text)), cv.Length(max=NUM_PROGRAMS)
            ),
            cv.Optional(CONF_ZONE_NAME_TEXTS): cv.All(
                cv.ensure_list(cv.use_id(text.Text)), cv.Length(max=NUM_ZONES)
            ),
            cv.Optional(CONF_ON_EVENT): automation.validate_automation(
                {cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(EventTrigger)}
            ),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    cv.only_on_esp32,
    _validate_pins,
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    spr = await cg.get_variable(config[CONF_SPRINKLER_ID])
    cg.add(var.set_sprinkler(spr))
    clock = await cg.get_variable(config[CONF_TIME_ID])
    cg.add(var.set_time(clock))

    if CONF_FLOW_SENSOR in config:
        cg.add_define("USE_IRRIGATION_FLOW_SENSOR")
        flow = await cg.get_variable(config[CONF_FLOW_SENSOR])
        cg.add(var.set_flow_sensor(flow))

    for pin in config[CONF_OUTPUT_CANDIDATES]:
        cg.add(var.add_output_candidate(pin))
    for pin in config[CONF_RESERVED_PINS]:
        cg.add(var.add_reserved_pin(pin))

    if CONF_EXPANDER_ID in config:
        cg.add_define("USE_IRRIGATION_EXPANDER")
        exp = await cg.get_variable(config[CONF_EXPANDER_ID])
        cg.add(var.set_expander(exp))

    for code in config[CONF_DEFAULT_OUTPUTS]:
        cg.add(var.add_default_output(code))
    cg.add(var.set_default_master_output(config[CONF_DEFAULT_MASTER_OUTPUT]))

    if CONF_PROGRAM_NAME_TEXTS in config or CONF_ZONE_NAME_TEXTS in config:
        cg.add_define("USE_IRRIGATION_NAME_TEXTS")
    for i, tid in enumerate(config.get(CONF_PROGRAM_NAME_TEXTS, [])):
        t = await cg.get_variable(tid)
        cg.add(var.set_program_name_text(i, t))
    for i, tid in enumerate(config.get(CONF_ZONE_NAME_TEXTS, [])):
        t = await cg.get_variable(tid)
        cg.add(var.set_zone_name_text(i, t))

    for conf in config.get(CONF_ON_EVENT, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID])
        cg.add(var.add_on_event_trigger(trigger))
        await automation.build_automation(
            trigger, [(cg.std_string, "type"), (cg.std_string, "json")], conf
        )
