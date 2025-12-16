# OneWire Temperature Sensors Workflow

Guide for adding Dallas DS18B20 temperature sensors to your device.

## Step 1: Find Sensor Address

Connect DS18B20 sensor(s) to the onewire connector, then check logs:

```
[15:45:33.923][C][gpio.one_wire:021]: GPIO 1-wire bus:
[15:45:33.929][C][gpio.one_wire:022]:   Pin: GPIO15
[15:45:33.934][C][gpio.one_wire:084]:   Found devices:
[15:45:33.940][C][gpio.one_wire:086]:     0xeb01227905460228 (DS18B20)
```

Copy the address: `0xeb01227905460228`

## Step 2: Add to Configuration

Edit `include/jxd-r6-one-wire.yaml`:

```yaml
sensor:
  - platform: dallas_temp
    name: "Temperature"
    id: temperature_sensor
    update_interval: 20s
  
  # Add new sensor with discovered address
  - platform: dallas_temp
    address: 0xeb01227905460228
    name: "Temperature 2"
    id: temperature_sensor_2
    update_interval: 20s
```

## Step 3: Add to Group

Edit `include/jxd-r6-e1eth-base.yaml`:

```yaml
groups:
  - id: onewire_group_id
    name: "OneWire Temp"
    entities:
      - temperature_sensor
      - temperature_sensor_2  # Add your sensor ID here
```

## Step 4: Add to Display Menu

Edit `include/jxd-r6-e1eth-display.yaml` (around line 94-103):

```yaml
- type: menu
  text: "Temperatures"
  items:
  - type: value
    text: !lambda |-
      return std::string(id(temperature_sensor).get_name());
    value_lambda: !lambda |-
      return str_sprintf(": %2.1f°C", id(temperature_sensor).state);
  
  # Add new sensor
  - type: value
    text: !lambda |-
      return std::string(id(temperature_sensor_2).get_name());
    value_lambda: !lambda |-
      return str_sprintf(": %2.1f°C", id(temperature_sensor_2).state);
```

## Step 5: Upload Firmware

```bash
esphome run JXD/jxd-r6-e1eth-lcd-eth.yaml
```

The sensor will now appear in Home Assistant and on the device display.

## Hardware Connection

Connect DS18B20 sensors to the onewire connector on the device. Multiple sensors can be connected in parallel on the same bus.
