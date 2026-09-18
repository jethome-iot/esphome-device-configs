# WiFi Setup

Out of the box the network mode is `Ethernet` and WiFi is never started. The firmware
ships no WiFi credentials: once WiFi is switched on and it has no network to join, or
cannot reach the one it knows, it raises its own access point and serves a captive
portal, and you hand it the real network from there.

A device on Ethernet needs none of this. Note that in `Auto` mode the access point
comes up and stays up until WiFi is provisioned, cable or not.

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
credentials and reboots; the access point is back within 90 seconds, and you
reconfigure through the captive portal as above.

**By factory reset.** **Settings → Factory reset → Confirm** clears the stored preferences,
credentials included, formats the user partition, and reboots into AP mode.

**By baking credentials into the firmware.** `devices/JXD/packages/features/network.yaml` configures
only the fallback AP; add `ssid` and `password` to its `wifi:` block, keeping them out
of the repository, then recompile and upload:

```bash
esphome run devices/JXD/jxd-r6-e1eth-lcd.yaml --device <IP_ADDRESS>
```
