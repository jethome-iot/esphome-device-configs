# WiFi Setup

Out of the box the network mode is `Ethernet` and WiFi is never started. The firmware
ships no WiFi credentials: once WiFi is switched on and it has no network to join, or
cannot reach the one it knows, it raises its own access point and serves a captive
portal, and you hand it the real network from there.

A device on Ethernet needs none of this, and a device you can already reach is
configured over the device API instead of through the portal. Note that in `Auto`
mode the access point comes up and stays up until WiFi is provisioned, cable or not.

## Initial setup

1. **Flash the firmware** over USB.

2. **Switch WiFi on.** **Settings → Network** on the display, or the `Network mode`
   entity: `WiFi` for WiFi only, `Auto` for Ethernet with WiFi as the fallback.

3. **Wait for the access point.** It comes up at once when no network is stored,
   and 90 seconds after the device fails to reach a stored one. With no Ethernet
   link the main page shows the WiFi icon and the access point's address.

4. **Join it** from a phone or laptop. The SSID is the device name from the top of
   the main page, `JXD-R6-E1ETH-LCD-XXXX`; the password is under **Info → AP
   password**.

5. **Configure the network.** The captive portal usually opens on its own; otherwise
   browse to `http://192.168.4.1`. Pick your network from the list, enter its password
   and save.

6. **The device reconnects** to your network and becomes reachable over the API and
   web interface. The main page shows the address of the link carrying the traffic,
   so with the Ethernet cable in it is the Ethernet one; both addresses are under
   **Info**.

The network you hand the portal joins the device's stored network list at the next
boot, and the device API answers it from then on.

## Over the device API

A device that is reachable — on Ethernet, or over WiFi on its old network — takes its
whole network configuration over HTTP: the mode, up to five WiFi networks and each
interface's address. The routes want the web server's credentials, Digest and
`admin` / `admin` out of the factory:

```bash
A='--digest -u admin:admin'

# what is stored now; a password is only ever reported as has_password
curl -s $A http://<device>/api/device/network/config

# the first network in the list is tried first; the mode is auto, wifi or ethernet
curl -sX POST $A -H 'Content-Type: application/json' \
  -d '{"network_type": "auto",
       "wifi_networks": [{"ssid": "home", "password": "s3cret"}],
       "wifi": {"use_dhcp": true}}' \
  http://<device>/api/device/network/config
```

The update is partial and all-or-nothing: what you leave out keeps its stored value,
and nothing is written unless every field passes. An entry without a `password` keeps
the one stored for that SSID, so the body `GET` answers goes straight back into `POST`.
A fixed address is the whole `manual_ip` object next to `use_dhcp: false`:

```json
{"ethernet": {"use_dhcp": false,
              "manual_ip": {"static_ip": "10.0.0.7", "gateway": "10.0.0.1",
                            "subnet": "255.255.255.0", "dns1": "10.0.0.1", "dns2": ""}}}
```

The mode and the network list take effect at once. An address change does not, and the
answer says so with `reboot_required: true`; the reboot is yours to send, so that the
client chooses when to lose the device:

```bash
curl -sX POST $A -H 'Content-Type: application/json' \
  -d '{"confirm": true, "confirm_token": "DD:EE:FF"}' \
  http://<device>/api/device/system/reboot
```

`confirm_token` is the last three octets of `base_mac_address` from
`/api/device/info`. A device that comes back on an address nobody can reach is
recovered by the display's factory reset.

The settings are stored in `/littlefs/config/network.json` on the user partition,
**passwords in the clear**, and the file API serves that directory like any other:
whoever can reach `/files` with the web server's credentials can read them.

## Access point details

Both values are derived from the WiFi MAC (**Info → MAC(WiFi)**), so nothing is
configured or stored:

- **SSID** — `JXD-R6-E1ETH-LCD-XXXX`, `XXXX` being the last two bytes of the MAC in
  upper case; the same name the main page shows
- **Password** — the last four bytes of the MAC in lower-case hex without colons, eight
  characters; also shown under **Info → AP password**
- **Timeout** — 90 seconds without a WiFi connection (ESPHome's default); none when
  no network is stored
- **Address** — `192.168.4.1` while in AP mode

## Changing the network later

**From the display menu.** **Settings → Reset WiFi creds → Confirm** clears the stored
credentials and restarts the radio, without rebooting. With no credentials compiled into the
firmware nothing is left to join, so in `WiFi` or `Auto` mode the access point is back within a
second or two and you reconfigure through the captive portal as above; in `Ethernet` mode WiFi
stays off until you switch the mode.

**By factory reset.** **Settings → Factory reset → Confirm** clears the stored preferences,
credentials included, formats the user partition, and reboots into AP mode.
`POST /api/device/system/factory-reset` does the same over the network, while the device is
still reachable on the old ones.

**Over the device API.** `POST /api/device/network/config`, as above; this is the way to
move a device to another network without going near it.

Credentials cannot be compiled in: the stored list is what the device joins, and a
`networks:` (or `ssid:`) in the `wifi:` block of
`devices/JXD/packages/features/network.yaml` is refused at validation. That block
configures the fallback AP and nothing else.
