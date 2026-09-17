# graphical_display_menu

Upstream's `graphical_display_menu` with a full-width row highlight. Listing it in
`external_components` shadows the copy that ships with ESPHome; everything else about the
component is upstream's. List [`display_menu_base`](../display_menu_base/README.md) from here
alongside it: `external_components` serves only the names under `components:`, so an auto-load
left off that list resolves to ESPHome's own.

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/jethome-iot/esphome-device-configs
      ref: master
      path: components
    components: [display_menu_base, graphical_display_menu]

graphical_display_menu:
  id: display_menu
  display: display1
  font: menu_font
  fill_row: true
  items: ...
```

## What this copy adds

| Option     | Default | Meaning                                                                   |
| ---------- | ------- | ------------------------------------------------------------------------- |
| `fill_row` | `false` | Whether a row is as wide as the display. The selected row's highlight then reaches both edges instead of ending with its text |
