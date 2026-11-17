# Modbus RTU server

The device answers as a Modbus RTU slave on the RS-485 connector JXM2: address 1 at 9600 8N1 by
default. Address, baud rate, parity and stop bits are set in **Settings → Modbus** on the display or
the dashboard, or through the `Modbus …` entities in Home Assistant, and take effect at once.

The register map is on the same dashboard tab: the relays are coils, the inputs discrete inputs and
the temperatures holding registers.

A write that would move a relay a running thermostat drives answers exception `0x04`. An address
outside the map answers `0` instead of an exception.

## Connector

JXM2: pins 1 and 3 are B, pins 2 and 4 are A.
