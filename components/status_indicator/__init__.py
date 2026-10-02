"""Drives one LED on a GPIO output: on, off, slow or fast blinking, N blinks and a pause, a pulse.

Each state is an action; the slow and fast blink times and the pulse length are configured on
the instance, the blink_n timing on the action.
"""

from esphome import automation, pins
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import (
    CONF_COUNT,
    CONF_DURATION,
    CONF_ID,
    CONF_ON_TIME,
    CONF_PIN,
    CONF_STATE,
)

CODEOWNERS = ["@jethome-iot"]

CONF_SLOW_ON_TIME = "slow_on_time"
CONF_SLOW_OFF_TIME = "slow_off_time"
CONF_FAST_ON_TIME = "fast_on_time"
CONF_FAST_OFF_TIME = "fast_off_time"
CONF_OFF_TIME = "off_time"
CONF_PAUSE_TIME = "pause_time"
CONF_PULSE_DURATION = "pulse_duration"

status_indicator_ns = cg.esphome_ns.namespace("status_indicator")
StatusIndicator = status_indicator_ns.class_("StatusIndicator", cg.Component)
IndicatorState = status_indicator_ns.enum("IndicatorState", is_class=True)

INDICATOR_STATES = {
    "OFF": IndicatorState.OFF,
    "ON": IndicatorState.ON,
    "BLINK_SLOW": IndicatorState.BLINK_SLOW,
    "BLINK_FAST": IndicatorState.BLINK_FAST,
    "BLINK_N": IndicatorState.BLINK_N,
    "PULSE": IndicatorState.PULSE,
}

TurnOnAction = status_indicator_ns.class_("TurnOnAction", automation.Action)
TurnOffAction = status_indicator_ns.class_("TurnOffAction", automation.Action)
BlinkSlowAction = status_indicator_ns.class_("BlinkSlowAction", automation.Action)
BlinkFastAction = status_indicator_ns.class_("BlinkFastAction", automation.Action)
BlinkNAction = status_indicator_ns.class_("BlinkNAction", automation.Action)
PulseAction = status_indicator_ns.class_("PulseAction", automation.Action)
SetStateAction = status_indicator_ns.class_("SetStateAction", automation.Action)

# A zero phase would reschedule on every loop pass.
time_ms = cv.All(cv.positive_not_null_time_period, cv.positive_time_period_milliseconds)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(StatusIndicator),
        cv.Required(CONF_PIN): pins.gpio_output_pin_schema,
        cv.Optional(CONF_SLOW_ON_TIME, default="500ms"): time_ms,
        cv.Optional(CONF_SLOW_OFF_TIME, default="500ms"): time_ms,
        cv.Optional(CONF_FAST_ON_TIME, default="200ms"): time_ms,
        cv.Optional(CONF_FAST_OFF_TIME, default="200ms"): time_ms,
        cv.Optional(CONF_PULSE_DURATION, default="200ms"): time_ms,
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    pin = await cg.gpio_pin_expression(config[CONF_PIN])
    cg.add(var.set_pin(pin))

    cg.add(var.set_slow_on_time(config[CONF_SLOW_ON_TIME]))
    cg.add(var.set_slow_off_time(config[CONF_SLOW_OFF_TIME]))
    cg.add(var.set_fast_on_time(config[CONF_FAST_ON_TIME]))
    cg.add(var.set_fast_off_time(config[CONF_FAST_OFF_TIME]))
    cg.add(var.set_pulse_duration(config[CONF_PULSE_DURATION]))


ID_SCHEMA = automation.maybe_simple_id({cv.GenerateID(): cv.use_id(StatusIndicator)})

BLINK_N_SCHEMA = automation.maybe_simple_id(
    {
        cv.GenerateID(): cv.use_id(StatusIndicator),
        cv.Optional(CONF_COUNT, default=3): cv.templatable(
            cv.int_range(min=1, max=255)
        ),
        cv.Optional(CONF_ON_TIME, default="200ms"): cv.templatable(time_ms),
        cv.Optional(CONF_OFF_TIME, default="200ms"): cv.templatable(time_ms),
        cv.Optional(CONF_PAUSE_TIME, default="1500ms"): cv.templatable(time_ms),
    }
)

# No default duration: the action then takes the instance's pulse_duration.
PULSE_SCHEMA = automation.maybe_simple_id(
    {
        cv.GenerateID(): cv.use_id(StatusIndicator),
        cv.Optional(CONF_DURATION): cv.templatable(time_ms),
    }
)


def indicator_state(value):
    # YAML reads a bare ON or OFF as a boolean.
    if isinstance(value, bool):
        value = "ON" if value else "OFF"
    return cv.enum(INDICATOR_STATES, upper=True)(value)


SET_STATE_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.use_id(StatusIndicator),
        cv.Required(CONF_STATE): cv.templatable(indicator_state),
    }
)


@automation.register_action(
    "status_indicator.turn_on", TurnOnAction, ID_SCHEMA, synchronous=True
)
@automation.register_action(
    "status_indicator.turn_off", TurnOffAction, ID_SCHEMA, synchronous=True
)
@automation.register_action(
    "status_indicator.blink_slow", BlinkSlowAction, ID_SCHEMA, synchronous=True
)
@automation.register_action(
    "status_indicator.blink_fast", BlinkFastAction, ID_SCHEMA, synchronous=True
)
async def simple_action_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    return var


@automation.register_action(
    "status_indicator.blink_n", BlinkNAction, BLINK_N_SCHEMA, synchronous=True
)
async def blink_n_action_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    cg.add(var.set_count(await cg.templatable(config[CONF_COUNT], args, cg.uint8)))
    for key, setter in (
        (CONF_ON_TIME, var.set_on_time),
        (CONF_OFF_TIME, var.set_off_time),
        (CONF_PAUSE_TIME, var.set_pause_time),
    ):
        cg.add(setter(await cg.templatable(config[key], args, cg.uint32)))
    return var


@automation.register_action(
    "status_indicator.pulse", PulseAction, PULSE_SCHEMA, synchronous=True
)
async def pulse_action_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    if CONF_DURATION in config:
        template_ = await cg.templatable(config[CONF_DURATION], args, cg.uint32)
        cg.add(var.set_duration(template_))
    return var


@automation.register_action(
    "status_indicator.set_state", SetStateAction, SET_STATE_SCHEMA, synchronous=True
)
async def set_state_action_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    template_ = await cg.templatable(config[CONF_STATE], args, IndicatorState)
    cg.add(var.set_state(template_))
    return var
