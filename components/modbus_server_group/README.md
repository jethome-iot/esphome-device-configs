# Modbus Server Group Component

Automatically configures a Modbus RTU server with coils and discrete inputs based on ESPHome groups. Eliminates manual lambda configuration.

The component is a Modbus server device in its own right: it attaches directly to a `modbus:` hub running with `role: server` and answers the coil / discrete input / register function codes itself. No `modbus_controller` is involved — since ESPHome 2026.8.0 `modbus_controller` is a client-only component.

## Why not upstream `modbus_server`?

ESPHome 2026.8 ships a `modbus_server` component with `registers:` and `bits:` blocks, which covers the same function codes. It serves FC 0x01 (read coils) and FC 0x02 (read discrete inputs) from **one shared bit table**, and its config validation rejects a duplicate bit address. That makes it impossible to expose a relay coil and a digital input at the same address.

This component keeps **two independent bit address spaces** by overriding `modbus::ModbusServerDevice::on_read_coils()` and `on_read_discrete_inputs()` separately, so the historical JXD map — relays and digital inputs both starting at 0xA000 — is preserved.

## Features

- **Auto Coils**: Switches → Modbus coils (read/write)
- **Auto Discrete Inputs**: Binary sensors → Discrete inputs (read-only)
- **Separate address spaces** for coils and discrete inputs
- **Count Registers**: 0x0200 (input count), 0x0201 (output count) — input registers, FC 0x04
- **Courtesy response**: unmapped addresses answer with a default instead of a Modbus exception
- **Configurable Addresses**: Custom start addresses for inputs/outputs

## Configuration

```yaml
# Define groups
groups:
  - id: outputs_group
    name: "Outputs"
  - id: inputs_group
    name: "Inputs"

# Modbus setup - the hub must run in server role
modbus:
  id: modbus_server
  uart_id: uart_bus
  role: server

# Automatic configuration
modbus_server_group:
  id: modbus_auto
  modbus_id: modbus_server
  address: 0x01
  courtesy_response:
    enabled: true
  outputs_group: outputs_group
  outputs_start_address: 0xA000
  inputs_group: inputs_group
  inputs_start_address: 0xA000
```

## Configuration Variables

- **modbus_id** (*Required*): ID of the `modbus:` hub (must have `role: server`)
- **address** (*Optional*, default: `0x01`): Modbus unit address of this server device
- **inputs_group** (*Optional*): Group with binary sensors
- **inputs_start_address** (*Optional*, default: 0xA100): Discrete inputs start address
- **outputs_group** (*Optional*): Group with switches
- **outputs_start_address** (*Optional*, default: 0xA000): Coils start address
- **courtesy_response** (*Optional*): what to answer for addresses this device does not map
  - **enabled** (*Optional*, default: `false`)
  - **register_last_address** (*Optional*, default: `0xFFFF`) / **register_value** (*Optional*, default: `0`)
  - **coil_last_address** (*Optional*, default: `0xFFFF`) / **coil_value** (*Optional*, default: `false`)
  - **discrete_input_last_address** (*Optional*, default: `0xFFFF`) / **discrete_input_value** (*Optional*, default: `false`)

*At least one of inputs_group or outputs_group must be specified.*

## Register Map

### Coils (FC 0x01, 0x05, 0x0F)
- **Addresses**: `outputs_start_address` + N
- **Access**: Read/Write
- **Maps to**: Switches in outputs_group

### Discrete Inputs (FC 0x02)
- **Addresses**: `inputs_start_address` + N
- **Access**: Read-only
- **Maps to**: Binary sensors in inputs_group

### Input Registers (FC 0x04)
- **0x0200**: Inputs count (read-only)
- **0x0201**: Outputs count (read-only)

### Holding Registers (FC 0x03)
None are mapped; with `courtesy_response` enabled every address answers with `register_value`.

## Example Result

With 6 relays and 6 inputs:
- **0xA000-0xA005**: Relay 1-6 (coils, read/write)
- **0xA000-0xA005**: Input 1-6 (discrete inputs, read-only, separate address space)
- **0x0200**: 6 (inputs count)
- **0x0201**: 6 (outputs count)

## Dependencies

- `modbus` (built-in, `role: server`)
- `groups`
