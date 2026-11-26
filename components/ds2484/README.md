# ds2484

Upstream's `ds2484` 1-Wire bus with one change: it waits up to 5 ms for the DS2484 to finish a
1-Wire operation, where upstream gives up after ten status reads. Listing it in
`external_components` shadows the copy that ships with ESPHome; the configuration is upstream's.

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/jethome-iot/esphome-device-configs
      ref: master
      path: components
    components: [ds2484]

one_wire:
  - platform: ds2484
    address: 0x18
```

## Why

At 400 kHz ten back-to-back status reads end about when a 1-Wire reset releases the DS2484, so
whether the reset counts as done depends on microseconds of I2C timing. On ESPHome 2026.10 it
often does not: the search at boot then finds no sensor, and a sensor that has no temperature
slot yet does not get one until a boot whose search succeeds (#323). A wait by time does not
depend on how fast the bus reads.

A DS2484 that is still busy after 5 ms is reported as `read status error: busy after 5000 us`,
and the operation fails as upstream's would.
