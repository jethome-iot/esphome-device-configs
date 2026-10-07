# timezone_settings

Decides which timezone the device runs on: Home Assistant's, the one compiled into the
firmware, or one chosen at run time — over
[`web_device_dashboard`](../web_device_dashboard/README.md)'s `/api/device/time`, which the
dashboard's **Settings → Date & Time** uses. It also tells which clock synced the time last.
ESP-IDF only.

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/jethome-iot/esphome-device-configs
      ref: master
      path: components
    components: [timezone_settings]

globals:
  - id: stored_timezone
    type: esphome::time::ParsedTimezone
    restore_value: true

timezone_settings:
  id: device_timezone
  timezone: Europe/Berlin            # the compiled zone
  homeassistant_zone: stored_timezone
  clocks:
    homeassistant: homeassistant_time
    sntp: sntp_time
    rtc: pcf8563_time

time:
  - platform: homeassistant          # no timezone: here, so Home Assistant sends its own
    id: homeassistant_time
  - platform: sntp
    id: sntp_time
    timezone: Europe/Berlin
  - platform: pcf8563
    id: pcf8563_time
    timezone: Europe/Berlin
```

| Option | | |
|---|---|---|
| `timezone` | required | the zone compiled in: an IANA name or a POSIX TZ string, as `time:` takes it |
| `homeassistant_zone` | optional | a restoring global of type `esphome::time::ParsedTimezone` that keeps Home Assistant's zone across reboots; needs `clocks: homeassistant` |
| `clocks` | optional | the `homeassistant`, `sntp` and `rtc` time components whose syncs are reported; the `homeassistant` one also hands over Home Assistant's zone, and must have no `timezone:` |

## Which zone is in effect

By default the device follows Home Assistant: the zone Home Assistant sent, once it has sent
one, and the compiled zone until then. Home Assistant sends its zone each time it sets the
clock, and the global keeps it, so a reboot comes back on it before Home Assistant has
answered. A stored UTC reads as nothing stored, so a device whose Home Assistant runs on UTC
comes back on the compiled zone after a reboot, until Home Assistant answers again.

A zone chosen by hand — a POSIX string, with the IANA name it was picked as — replaces that
when the device is told to stop following Home Assistant, and applies at once. Home
Assistant's zone is still taken and kept while it does not apply, so following it again
needs no answer from Home Assistant. A zone chosen while the device follows Home Assistant
is kept for later and not applied. The mode and the chosen zone are kept in flash and
survive reboots and OTA updates; a factory reset clears them, and the device follows Home
Assistant again.

The zone is applied as the component sets up, right after the globals are restored and
before anything that reads the local time sets itself up.

A chosen zone's name is at most 63 bytes with no control characters, and may be empty; its
POSIX string at most 63 bytes, printable ASCII, parsed the way ESPHome parses one at build
time. Anything else is refused and changes nothing. A change is stored before it is applied,
so one that does not reach flash leaves the device as it was and says so.

The panel's **Info → Timezone** row shows the chosen zone's city while it is in effect, and
the offset from UTC of the zone in effect otherwise.

## Tests

`tests/components/timezone_settings/` holds the device's POSIX parser to ESPHome's own Python
one over every zone in tzdata and a list of odd and malformed strings
(`posix_tz_cases.tsv`, written by `generate_fixture.py`; `test_fixture.py` says when it is
stale), and covers the mode and its storage, the upgrade from a firmware that kept only Home
Assistant's zone, Home Assistant's zone arriving in either mode, the offset and DST it
reports, the panel's text, and the configuration refusals.
