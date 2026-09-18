# network_config

The network settings a device keeps across reboots: which interfaces it runs, the WiFi networks
it may join, and whether each interface takes DHCP or a fixed address. The record is one JSON
file kept by `config_json`, and the device API edits it.

## Configuration

```yaml
network_config:
  id: network_config_id
  settings_id: network_settings
  network_mode_select_id: network_mode
  wifi_id: wifi_id
  ethernet_id: ethernet_id
```

- **id** (optional): the component; the device API is wired to it.
- **config_json_id** (optional): the keeper that holds the file. Defaults to the only one.
- **settings_id** (optional): the record itself, for lambdas.
- **network_mode_select_id** (optional): a `select` with the options `Auto`, `Ethernet`, `WiFi`
  in that order. The component publishes the stored mode into it at boot; the select's
  `on_value` calls `set_network_type_from_select()`.
- **wifi_id** / **ethernet_id** (optional): the interfaces to configure. Required when the
  configuration has that section.

`wifi:` must carry `enable_on_boot: false`, must not list `networks:`, and neither interface may
carry a `manual_ip:` — this component sets both at run time.

## The file

`<config_dir>/network.json`, next to the other `config_json` records:

```json
{
  "version": 1,
  "network_type": "auto",
  "wifi": { "use_dhcp": true,
            "manual_ip": {"static_ip": "", "gateway": "", "subnet": "", "dns1": "", "dns2": ""} },
  "ethernet": { "use_dhcp": false,
                "manual_ip": {"static_ip": "10.0.0.7", "gateway": "10.0.0.1",
                              "subnet": "255.255.255.0", "dns1": "10.0.0.1", "dns2": ""} },
  "wifi_networks": [ {"ssid": "home", "password": "s3cret"} ]
}
```

- `network_type` is `auto`, `ethernet` or `wifi`; with no file it is `ethernet`.
- `wifi_networks` holds at most five entries and the order is the priority: the first is tried
  first. **Passwords are stored in the clear**, and `config/` stays readable through
  `web_file_browser`.
- Credentials entered through the captive portal are taken over into the list within seconds,
  so an update installed before the next boot cannot lose them.
- A file that cannot be read leaves the compiled defaults in force and is not written over until
  something replaces it.

## Applying

The mode and the network list take effect as soon as they change. **An address change needs a
reboot**: ESPHome applies a manual IP when an interface connects and cannot be moved back to
DHCP while it runs. `reboot_required()` says whether the stored settings are still what the
interfaces are running, and the device API reports it.

A device that ends up on an unreachable address is recovered through the display's factory
reset, which clears the record with the rest of the settings.

## Boot order

The component sets up at priority 260, after the keeper has loaded the file and before either
interface. The boot script applies the mode at `on_boot` priority 200, once both interfaces
exist.
