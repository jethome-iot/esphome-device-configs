# firmware_rollback

Boots the firmware in the other app slot: says whether there is one worth going back to, and
makes it the next boot. Declare it with an `id` and drive it from YAML with the actions below;
[`web_device_dashboard`](../web_device_dashboard/README.md) auto-loads it for
`/api/device/system/rollback`, so the dashboard and a display menu give the same answer. ESP32
only.

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/jethome-iot/esphome-device-configs
      ref: master
      path: components
    components: [firmware_rollback]

firmware_rollback:
  id: rollback
```

## Actions

```yaml
# Read the other slot again.
- firmware_rollback.refresh: rollback

# True when the last read found a firmware to go back to.
- if:
    condition:
      firmware_rollback.is_available: rollback
    then:
      # Select the other slot and reboot into it.
      - firmware_rollback.rollback:
          id: rollback
          on_error:
            - logger.log:
                format: "Rollback failed: %s"
                args: [x.c_str()]
```

The instance holds what the last read found; `firmware_rollback.is_available` and
`available()` answer from it without touching the flash. Boot does the first read, and a
`firmware_rollback.refresh` keeps it current: run one before showing the answer, as a menu
does when it opens.

`firmware_rollback.rollback` checks the slot again and, on success, reboots into it; the actions
after it do not run. On failure nothing is selected, the instance reads the slot again, and
`on_error` runs with the reason in `x`, a `std::string`; the actions after it run too.

The `id` may be left out of all three: there is only one instance.

## C++

For lambdas, the instance gives the same answers: `id(rollback).available()`, and
`id(rollback).target()` with the slot's `partition`, `version` and `project_name`. The free
functions read and select without going through it:

```cpp
// Anywhere, on any task: reads the slot's header, never the image.
auto target = firmware_rollback::rollback_target();

// On the loop task; nullptr once the next boot is the other slot, else why not.
if (firmware_rollback::select_rollback(target) == nullptr)
  App.safe_reboot();
```

## When a rollback is possible

After an update over the air, the other slot holds the firmware the update replaced, and a
rollback boots it. After a rollback it holds the newer firmware, so a second rollback goes
forward again.

There is nothing to roll back to:

- after a serial flash, which leaves the other slot unselected;
- after an update that failed or was interrupted;
- after the bootloader itself rolled back from a firmware that crashed before it confirmed
  itself;
- while a switch is already waiting for its reboot — an update just written, or a rollback
  just selected.

This, and the monitored boot below, take the bootloader's app rollback, which ESPHome builds in
whenever `ota:` is configured, unless `esp32: advanced: enable_ota_rollback` is off.

## Selecting

A rollback asks again, and refuses when the slot has gone or is no longer the one the last read
found. It reads the whole image back and checks it before it writes the boot selection,
so a broken slot is an error at the call rather than a device that reboots and comes back
unchanged. The firmware it selects gets one monitored boot: if it fails before it marks itself
good, the bootloader returns to the running one. A running firmware that has not yet confirmed
its own first boot is confirmed first, once the target has checked out, so it stays there to
fall back to. `select_rollback()` leaves the reboot to its caller.

The target is the slot's partition label and, from the image's header, its ESPHome `version`
and `project_name`. Off ESP32 there is none, and a rollback fails with `Rollback needs an
ESP32`.

## Tests

`tests/components/firmware_rollback/` covers the configuration refusals of the component, its
actions and its condition and, on the host, the rule that decides: every slot state the
bootloader would and would not boot, a switch already pending, a slot with no header or no
second slot at all, and a header field that fills its 32 bytes with no terminator, read under
AddressSanitizer. The instance and its automations run over the host build's answers: a read
that finds nothing, a refused rollback that fires `on_error` with its reason and reads again,
and the condition following the last read. Out of reach there are the `esp_ota_*` and
`esp_image_verify` calls that read the slot and write the boot selection, and so a rollback that
succeeds and reboots, which exist only on ESP32.
