# Modbus server

The device answers as a Modbus RTU slave on the RS-485 connector JXM2: address 1 at 9600 8N1 by
default. Address, baud rate, parity and stop bits are set in **Settings → Modbus** on the display or
the dashboard, or through the `Modbus …` entities in Home Assistant, and take effect at once.

The register map is on the same dashboard tab: the relays are coils, the inputs discrete inputs and
the temperatures holding registers.

A write that would move a relay a running thermostat drives answers exception `0x04`. A register
outside the map reads `0` instead of an exception; a coil or discrete input outside the map
answers exception `0x02`, so read the relays and the inputs in separate requests.

## Modbus TCP

The same map answers over Modbus TCP on Ethernet and on the WiFi network, with the same addresses,
function codes and exceptions. It is off until **Modbus TCP** is switched on in **Settings → Modbus**
on the dashboard or from Home Assistant; port (502) and unit ID (1) are set there too and take
effect at once.

Modbus has no password: whoever reaches the port moves the relays. Keep the device on a network
you trust and do not forward the port to it from the internet. Connections through the device's
fallback WiFi access point are refused.

- Requests to unit ID 0 and 255 reach the device as well as its own; any other unit answers
  exception `0x0B`.
- Two clients at a time; a third is refused. A client that sends nothing for 60 s is disconnected.

## Connector

JXM2: pins 1 and 3 are B, pins 2 and 4 are A.
