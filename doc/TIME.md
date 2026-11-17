# Time

The device keeps time in its battery-backed RTC and sets it from NTP or Home Assistant, whichever
reaches it.

## Timezone

The device follows Home Assistant's timezone: it takes the zone on connect and keeps it across
reboots. Until it is added to Home Assistant it runs on the zone compiled into the firmware, `UTC`
by default.

On the dashboard, **Settings → Date & Time** switches the device from Home Assistant's zone to one
picked there, and back; the choice is kept across reboots. The display shows the zone in effect
under **Info → Timezone**. How that zone is chosen: [timezone_settings](../components/timezone_settings/README.md).

## Setting the clock by hand

A device with neither internet nor Home Assistant has nothing to set its clock from but the RTC.
The same dashboard screen sets the date and time by hand, into the RTC as well, so they survive a
reboot. NTP or Home Assistant, once either can reach the device, sets the time again as usual.

## NTP servers

A site with an NTP server of its own, such as its router or a domain controller, sets up to three
servers on the same screen. They are asked ahead of `pool.ntp.org`, which can be turned off, and
the screen shows when the time came last and from which server. See
[ntp_servers](../components/ntp_servers/README.md).

## Compiled zone

Compile the zone in for a device that runs standalone from its first boot:

```bash
esphome -s timezone Europe/Berlin run devices/JXD/jxd-r6-e1eth-lcd.yaml
```

or per device in the device config:

```yaml
substitutions:
  timezone: Europe/Berlin
```

Both IANA names (`Europe/Berlin`) and POSIX TZ strings (`CET-1CEST,M3.5.0,M10.5.0/3`) are
accepted. POSIX strings per zone: [posix_tz_db](https://github.com/nayarsystems/posix_tz_db/blob/master/zones.csv).
