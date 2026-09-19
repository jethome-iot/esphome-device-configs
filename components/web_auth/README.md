# web_auth

Makes the `web_server` credentials changeable on a running device. The `auth:` block in the
configuration becomes the factory default; a pair set later takes effect on the next request
and is kept in flash, so it survives reboots and OTA updates. ESP-IDF only.

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/jethome-iot/esphome-device-configs
      ref: master
      path: components
    components: [web_auth]

web_server:
  port: 80
  auth:
    type: digest
    username: admin
    password: admin

web_auth:
```

`auth:` is required — without it the firmware carries no authentication at all and there is
nothing for this component to replace; the configuration is refused with that message. Off
ESP32 the block must also say `type: digest`.

A new pair applies to the next request: no restart, and the request that sets it still answers.
The browser asks for the new credentials as soon as the page makes its next call. The pair is
stored before it is applied, from the main loop task: a device that cannot write its flash keeps
serving the old pair, says so in the log and raises the component's error status, which is where
the failure shows once the request has been answered. Flash is flushed for the whole device at
once, so a failure there is not always this record's — a pair that reads back as stored is
applied. A username is up to 32 characters and a password up to 64, both printable ASCII, and a
username may not contain `:`, `"` or `\`.

Everything the shared web server carries is behind the same credentials — the dashboard, the
file manager, the automation editor and `web_server`'s own REST API and `/events`. The ESPHome
native API and OTA keep their own ports and are not affected, and neither is `captive_portal`:
upstream registers it without authentication on purpose, so a device in its WiFi setup access
point takes credentials from anyone in radio range.

The credentials live in the same flash area as the rest of the device's saved state, so a
factory reset restores the ones the firmware was built with. That is the only way back in after
a forgotten password, short of reflashing.

While the device still serves the factory password it says so in the boot log and on
`/api/device/auth`.

## Tests

`tests/components/web_auth/` covers the configuration refusals on the host and, through the
`web_server_base` stand-in, what reaches the server: the stored pair at boot, the compiled
default when nothing is stored, which credentials are refused, and what a store that fails
leaves in force.
