# Home Assistant

## Adding the device

Home Assistant discovers the device and adds it through its ESPHome integration, which sets the
device's API encryption key; from then on only that key connects.

That has to happen within 15 minutes of power-on or a reboot. After that, a device not yet added
turns every API connection away, and its WiFi setup access point goes down, until the next reboot.
To add it later, reboot it first: **Settings → Reboot device** on the display, **Settings → System**
on the dashboard, or by cutting the power. A device already added connects whenever its network
comes up.

A key cleared later (by Forget HA below, or from Home Assistant's side, as deleting the device
there may do) leaves the device to be added again within 15 minutes of a boot.

## Moving to another Home Assistant

**Settings → Forget HA → Confirm** on the display, or **Forget Home Assistant key** under
**Settings → Security** on the dashboard, clears the key and reboots; the settings and files stay.
Add the device again within 15 minutes, to the same Home Assistant or another. The Home Assistant
that held the key asks to re-authenticate the device: do that, or delete the device there and add
it again, within 15 minutes of the reboot.

Over the network it is `POST /api/device/system/forget-api-key`
([details](../components/web_device_dashboard/README.md#system-actions)). A factory reset clears the
key as well.

## Uploads from ESPHome

The ESPHome Builder and `esphome run --device` install only while **Allow uploads from ESPHome**
is on: in Home Assistant, under **Settings → Security** on the dashboard, or **Settings → ESPHome
OTA** on the display. The switch turns itself off:

- 30 minutes after it was turned on: on a slow build, turn it on while the build runs
- at every reboot, except into safe mode, where the port stays open as the way back without USB
- whenever the API key is set or cleared
- on a device without an API key, when the 15 minutes after a boot run out; until then anyone on
  the network can turn it on over the API

Firmware updates from JetHome, an upload encrypted with the API key, and an upload through the
dashboard behind the web password do not need it.
