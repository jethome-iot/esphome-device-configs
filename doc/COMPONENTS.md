# Custom and Modified Components

This repository includes custom and modified ESPHome components. Some are copied from ESPHome 2025.10.5 and enhanced, others are completely custom.

## Components Overview

1. **display_menu_base** - Menu system base (modified from ESPHome)
2. **graphical_display_menu** - OLED menu renderer (modified from ESPHome)
3. **groups** - Entity grouping system (custom)
4. **modbus** - Modbus RTU protocol (from ESPHome 2025.10.5)
5. **modbus_controller** - Modbus communication (enhanced from ESPHome)
6. **modbus_server_group** - Automatic Modbus mapping (custom)

---

## display_menu_base

Base component for menu systems on displays. Copied from ESPHome core and modified.

**Modifications**:
- BackAction support - add back action
- `right_for_menu_enter` option - joystick navigation mode
- `reset_menu()` method - programmatic menu reset

---

## graphical_display_menu

Graphical menu rendering for OLED displays (SSD1306, SH1106). Copied from ESPHome core and modified.

**Modifications**:
- `fill_row` and `restore_page` options
- MenuItemValue support - display read-only values in menus with separation into text and value
- `shrink_label` option - truncate long text to fit display

---

## groups

Custom component for entity grouping system - organizing relays, inputs, sensors, etc.

**Purpose**: Groups entities for easier management and automatic configuration by other components (like modbus_server_group).

**Example**:
```yaml
groups:
  - id: relays_group_id
    name: "Relays"
    entities:
      - relay_1
      - relay_2
```

---

## modbus

Core Modbus RTU protocol implementation. Copied from ESPHome 2025.10.5 with minor enhancements for server mode support.

---

## modbus_controller

Modbus communication controller that maps ESPHome entities to Modbus registers. Copied from ESPHome 2025.10.5 and enhanced.

**Main Enhancement**: Added server role with support for coils, discrete inputs, analog inputs, and holding registers.

**Configuration for Server Mode**:

```yaml
modbus:
  id: modbus_server
  uart_id: uart_bus
  role: server

modbus_controller:
  - id: modbus_ctrl
    modbus_id: modbus_server
    address: 0x01
    
    server_discrete_inputs:
      # Digital Input 1 at address 0xA000
      - address: 0xA000
        read_lambda: |-
          return id(input_1).state;
    
    server_coils:
      # Relay 1 at address 0xA000
      - address: 0xA000
        read_lambda: |-
          return id(relay_1).state;
        write_lambda: |-
          if (x) {
            id(relay_1).turn_on();
          } else {
            id(relay_1).turn_off();
          }
```

---

## modbus_server_group

Custom component for simple automatic mapping of coils and discrete inputs from ESPHome groups to Modbus.

**Features**:
- Auto-maps switches from group to Modbus coils (read/write)
- Auto-maps binary sensors to discrete inputs (read-only)
- Provides count registers (0x0200, 0x0201) for discovery
- Configurable start addresses
- Eliminates manual lambda-based Modbus configuration

**Example**:
```yaml
modbus_server_group:
  modbus_controller_id: jxd_modbus_server
  outputs_group: relays_group_id
  outputs_start_address: 0xA000
  inputs_group: digital_inputs_group_id
  inputs_start_address: 0xA000
```

**Result**: With 6 relays and 6 inputs:
- Coils 0xA000-0xA005: Relay 1-6 (read/write)
- Discrete Inputs 0xA000-0xA005: Input 1-6 (read-only)
- Register 0x0200: Input count (6)
- Register 0x0201: Output count (6)

---

## References

- [ESPHome Official Documentation](https://esphome.io/)
- [Modbus Protocol Specification](https://www.modbus.org/docs/Modbus_Application_Protocol_V1_1b3.pdf)
- Component source code in `components/` directory
