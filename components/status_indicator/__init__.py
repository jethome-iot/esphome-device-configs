"""Drives one LED on a GPIO output: on, off, slow or fast blinking, N blinks and a pause, a pulse.

Each state is an action; the slow and fast blink times and the pulse length are configured on
the instance, the blink_n timing on the action. Signals pair a condition with a state or a
blink_n; the first that holds is shown.
"""

from esphome import automation, pins
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import (
    CONF_CONDITION,
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
CONF_SIGNALS = "signals"

status_indicator_ns = cg.esphome_ns.namespace("status_indicator")
StatusIndicator = status_indicator_ns.class_("StatusIndicator", cg.PollingComponent)
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

# The blink_n action's, for a signal too.
BLINK_N_TIMES = {
    CONF_ON_TIME: "200ms",
    CONF_OFF_TIME: "200ms",
    CONF_PAUSE_TIME: "1500ms",
}


def indicator_state(value):
    # YAML reads a bare ON or OFF as a boolean.
    if isinstance(value, bool):
        value = "ON" if value else "OFF"
    return cv.enum(INDICATOR_STATES, upper=True)(value)


# OFF is what no signal shows; BLINK_N is count; a PULSE would end on its own.
def signal_state(value):
    value = indicator_state(value)
    if value not in ("ON", "BLINK_SLOW", "BLINK_FAST"):
        raise cv.Invalid(
            f"A signal shows ON, BLINK_SLOW or BLINK_FAST, or a count, not {value}"
        )
    return value


def blink_times_with_count(config):
    if CONF_COUNT in config:
        for key, default in BLINK_N_TIMES.items():
            config.setdefault(key, time_ms(default))
        return config
    for key in BLINK_N_TIMES:
        if key in config:
            raise cv.Invalid(f"{key} goes with count, not with state", path=[key])
    return config


SIGNAL_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.Required(CONF_CONDITION): automation.validate_potentially_and_condition,
            cv.Optional(CONF_STATE): signal_state,
            cv.Optional(CONF_COUNT): cv.int_range(min=1, max=255),
            **{cv.Optional(key): time_ms for key in BLINK_N_TIMES},
        }
    ),
    cv.has_exactly_one_key(CONF_STATE, CONF_COUNT),
    blink_times_with_count,
)

# Signals are checked every update_interval; with none there is no polling.
CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(StatusIndicator),
        cv.Required(CONF_PIN): pins.gpio_output_pin_schema,
        cv.Optional(CONF_SLOW_ON_TIME, default="500ms"): time_ms,
        cv.Optional(CONF_SLOW_OFF_TIME, default="500ms"): time_ms,
        cv.Optional(CONF_FAST_ON_TIME, default="200ms"): time_ms,
        cv.Optional(CONF_FAST_OFF_TIME, default="200ms"): time_ms,
        cv.Optional(CONF_PULSE_DURATION, default="200ms"): time_ms,
        cv.Optional(CONF_SIGNALS, default=[]): cv.ensure_list(SIGNAL_SCHEMA),
    }
).extend(cv.polling_component_schema("500ms"))


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

    for signal in config[CONF_SIGNALS]:
        condition = await automation.build_condition(
            signal[CONF_CONDITION], cg.TemplateArguments(), []
        )
        if CONF_COUNT in signal:
            cg.add(
                var.add_signal(
                    condition,
                    IndicatorState.BLINK_N,
                    signal[CONF_COUNT],
                    signal[CONF_ON_TIME],
                    signal[CONF_OFF_TIME],
                    signal[CONF_PAUSE_TIME],
                )
            )
        else:
            cg.add(var.add_signal(condition, signal[CONF_STATE]))


ID_SCHEMA = automation.maybe_simple_id({cv.GenerateID(): cv.use_id(StatusIndicator)})

BLINK_N_SCHEMA = automation.maybe_simple_id(
    {
        cv.GenerateID(): cv.use_id(StatusIndicator),
        cv.Optional(CONF_COUNT, default=3): cv.templatable(
            cv.int_range(min=1, max=255)
        ),
        **{
            cv.Optional(key, default=default): cv.templatable(time_ms)
            for key, default in BLINK_N_TIMES.items()
        },
    }
)

# No default duration: the action then takes the instance's pulse_duration.
PULSE_SCHEMA = automation.maybe_simple_id(
    {
        cv.GenerateID(): cv.use_id(StatusIndicator),
        cv.Optional(CONF_DURATION): cv.templatable(time_ms),
    }
)


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
