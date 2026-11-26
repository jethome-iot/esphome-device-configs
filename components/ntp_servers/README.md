# ntp_servers

The NTP servers the device asks: up to three set at run time — over
[`web_device_dashboard`](../web_device_dashboard/README.md)'s `/api/device/time`, which the
dashboard's **Settings → Date & Time** uses — then the SNTP clock's own, unless that fallback
is turned off. For a site with an NTP server of its own, such as its router or a domain
controller. ESP-IDF only.

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/jethome-iot/esphome-device-configs
      ref: master
      path: components
    components: [ntp_servers]

ntp_servers:
  id: device_ntp
  sntp_id: sntp_time

time:
  - platform: sntp
    id: sntp_time
    servers:                         # the fallback
      - 0.pool.ntp.org
      - 1.pool.ntp.org
      - 2.pool.ntp.org
```

| Option | | |
|---|---|---|
| `sntp_id` | required | the `sntp` time component: its `servers` are the fallback, its `update_interval` the poll |

## Which server is asked

The servers set at run time come first, in their order, then the clock's own that are not
among them already, case aside. One server is asked at a time:

- One that gives no answer within 30 seconds makes way for the next; after the last comes the
  first again. Once a whole round has gone unanswered, each server is given twice as long, up
  to one poll.
- One that answers is asked again at every poll. When it is not the first, the first is asked
  at the next poll instead, so a server that is back is used again.
- While the network is down no server is given up on; when it is back, the first is asked at
  once.
- A list of one is left to lwIP, which retries it on its own.

With no server set and the fallback off, the device asks none; its clock then comes from Home
Assistant, the RTC, or a time set by hand.

A server is a host name or an IPv4 address of at most 63 bytes: letters, digits and hyphens in
dot-separated labels, none starting or ending with a hyphen. A name of digits alone must be an
address, four numbers up to 255 without leading zeros. The same server twice, case aside, is
refused. Anything else is refused and changes nothing. A change is stored before it takes
effect, and the first server is asked at once. The servers survive reboots and OTA updates; a
factory reset clears them, back to the clock's own.

The last answer, when and from which server, is kept in RAM until the next reboot.

## Tests

`tests/components/ntp_servers/` covers the name rules, the list and its storage, a record
another firmware stored, which server is asked as servers fail and answer and the network
comes and goes, the waits across the `millis()` wrap, and the configuration refusals. What
lwIP does with the server it is handed is ESP-IDF only and out of reach there.
