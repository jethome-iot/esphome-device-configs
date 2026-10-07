# status_indicator

Drives one LED on a GPIO output: on, off, slow or fast blinking, a number of blinks and a
pause, or a single pulse. Each is an action, and the LED stays in a state until the next action
changes it. It starts off.

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/jethome-iot/esphome-device-configs
      ref: master
      path: components
    components: [status_indicator]

status_indicator:
  id: red_led
  pin: GPIO2
```

## Options

| Option           | Default | Meaning |
| ---------------- | ------- | ------- |
| `pin`            |         | The output; high lights the LED. On ESP32 a strapping pin takes `ignore_strapping_warning` |
| `slow_on_time`   | `500ms` | Lit part of a slow blink |
| `slow_off_time`  | `500ms` | Dark part of a slow blink |
| `fast_on_time`   | `200ms` | Lit part of a fast blink |
| `fast_off_time`  | `200ms` | Dark part of a fast blink |
| `pulse_duration` | `200ms` | A pulse's length when the action gives none |

Every time is above zero, in whole milliseconds.

## Actions

The `id` may be left out when there is one instance. Every option but `id` takes a lambda.

```yaml
- status_indicator.turn_on: red_led
- status_indicator.turn_off: red_led
- status_indicator.blink_slow: red_led
- status_indicator.blink_fast: red_led

# count blinks, then a pause, over and over.
- status_indicator.blink_n:
    id: red_led
    count: 3            # 1 to 255
    on_time: 200ms
    off_time: 200ms
    pause_time: 1500ms  # dark after the last blink, in place of its off_time

# Lit for duration, then back to what it was doing.
- status_indicator.pulse:
    id: red_led
    duration: 100ms     # pulse_duration when left out

# OFF, ON, BLINK_SLOW, BLINK_FAST, BLINK_N or PULSE, in any case.
- status_indicator.set_state:
    id: red_led
    state: BLINK_FAST
```

- Asking for the state it is already in changes nothing: a blink keeps its rhythm.
- `blink_n` always starts over with its own options. `set_state: BLINK_N` repeats the last
  `blink_n`, or 3 blinks of 200 ms with a 1.5 s pause before there was one. A count of 0 from
  a lambda turns the LED off; a time of 0 is refused, and the LED goes on as it was.
- `pulse` goes back to the state it interrupted; a blink starts over from its lit part. A pulse
  during a pulse runs for its own duration and still goes back to the state before the first.
  `set_state: PULSE` is a pulse of `pulse_duration`. Any other action ends a pulse early. Over
  `ON`, a pulse does not show.

## From lambdas

`id(red_led)` has the same calls: `turn_on()`, `turn_off()`, `blink_slow()`, `blink_fast()`,
`blink_n(count, on_ms, off_ms, pause_ms)`, `pulse(duration_ms)` (0 for `pulse_duration`),
`set_state(status_indicator::IndicatorState::BLINK_FAST)` and `get_state()`.
