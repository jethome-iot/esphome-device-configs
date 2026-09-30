# firmware_rollback

Boots the firmware in the other app slot: says whether there is one worth going back to, and
makes it the next boot. A library with nothing to configure —
[`web_device_dashboard`](../web_device_dashboard/README.md) auto-loads it for
`/api/device/system/rollback`, and a display menu declares it to offer the same rollback from
its own rows, so both give the same answer. ESP32 only.

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/jethome-iot/esphome-device-configs
      ref: master
      path: components
    components: [firmware_rollback]

firmware_rollback:
```

```cpp
// Anywhere, on any task: reads the slot's header, never the image.
auto target = firmware_rollback::rollback_target();
if (target.available())
  ESP_LOGI("main", "Can roll back to %s (%s)", target.version.c_str(), target.partition.c_str());

// On the loop task; nullptr once the next boot is the other slot, else why not.
if (firmware_rollback::select_rollback(target) == nullptr)
  App.safe_reboot();
```

## When a rollback is possible

After an update over the air, the other slot holds the firmware the update replaced, and a
rollback boots it. After a rollback it holds the newer firmware, so a second rollback goes
forward again; `version` says which one it is.

There is nothing to roll back to:

- after a serial flash, which leaves the other slot unselected;
- after an update that failed or was interrupted;
- after the bootloader itself rolled back from a firmware that crashed before it confirmed
  itself;
- while a switch is already waiting for its reboot — an update just written, or a rollback
  just selected.

## Selecting

`select_rollback()` asks again, and refuses when the slot has gone or is no longer the one it
was handed. It reads the whole image back and checks it before it writes the boot selection,
so a broken slot is an error at the call rather than a device that reboots and comes back
unchanged. The firmware it selects gets one monitored boot: if it fails before it marks itself
good, the bootloader returns to the running one. A running firmware that has not yet confirmed
its own first boot is confirmed first, once the target has checked out, so it stays there to
fall back to. The caller does the reboot.

`rollback_target()` reports the slot's partition label and, from the image's header, its
ESPHome `version` and `project_name`. Off ESP32 it reports nothing, and `select_rollback()`
answers `Rollback needs an ESP32`.

## Tests

`tests/components/firmware_rollback/` covers the configuration refusals and, on the host, the
rule that decides: every slot state the bootloader would and would not boot, a switch already
pending, a slot with no header or no second slot at all, and a header field that fills its 32
bytes with no terminator, read under AddressSanitizer. Out of reach there are the `esp_ota_*`
and `esp_image_verify` calls that read the slot and write the boot selection, which exist only
on ESP32; the host build's own answers are checked instead.
