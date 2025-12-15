# Modbus Server Group Component

Automatically configures Modbus RTU server with coils and discrete inputs based on ESPHome groups. Eliminates manual lambda configuration.

## Features

- **Auto Coils**: Switches → Modbus coils (read/write)
- **Auto Discrete Inputs**: Binary sensors → Discrete inputs (read-only)
- **Count Registers**: 0x0200 (input count), 0x0201 (output count)
- **Configurable Addresses**: Custom start addresses for inputs/outputs

## Configuration

```yaml
# Define groups
groups:
  - id: outputs_group
    name: "Outputs"
  - id: inputs_group
    name: "Inputs"

# Modbus setup
modbus:
  id: modbus_server
  uart_id: uart_bus
  send_wait_time: 200ms
  role: server

modbus_controller:
  - id: modbus_ctrl
    modbus_id: modbus_server
    address: 0x01
    update_interval: 1s

# Automatic configuration
modbus_server_group:
  id: modbus_auto
  modbus_controller_id: modbus_ctrl
  outputs_group: outputs_group
  outputs_start_address: 0xA000
  inputs_group: inputs_group
  inputs_start_address: 0xA000
```

## Configuration Variables

- **modbus_controller_id** (*Required*): Modbus controller ID
- **inputs_group** (*Optional*): Group with binary sensors
- **inputs_start_address** (*Optional*, default: 0xA100): Discrete inputs start address
- **outputs_group** (*Optional*): Group with switches
- **outputs_start_address** (*Optional*, default: 0xA000): Coils start address

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

### Holding Registers (FC 0x03)
- **0x0200**: Inputs count (read-only)
- **0x0201**: Outputs count (read-only)

## Example Result

With 6 relays and 6 inputs:
- **0xA000-0xA005**: Relay 1-6 (coils, read/write)
- **0xA000-0xA005**: Input 1-6 (discrete inputs, read-only)
- **0x0200**: 6 (inputs count)
- **0x0201**: 6 (outputs count)

## Dependencies

- `modbus_controller`
- `groups`
