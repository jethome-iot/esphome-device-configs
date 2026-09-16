# display_menu_base

Upstream's `display_menu_base` with a back action and a switch for what the "right" input does.
Listing it in `external_components` shadows the copy that ships with ESPHome; everything else
about the component, including the `items:` schema, is upstream's.

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/jethome-iot/esphome-device-configs
      ref: master
      path: components
    components: [display_menu_base, graphical_display_menu]
```

## What this copy adds

| Option                  | Default | Meaning                                                            |
| ----------------------- | ------- | ------------------------------------------------------------------ |
| `right_for_menu_enter`  | `true`  | Whether "right" opens the selected submenu. With `false` only "enter" descends, and "right" is left to the value keys |

The option belongs to whichever menu component carries it — `graphical_display_menu:` or
`lcd_menu:` — since both extend this schema.

```yaml
graphical_display_menu:
  id: display_menu
  right_for_menu_enter: false
  items: ...

binary_sensor:
  - platform: gpio
    pin: GPIO14
    on_press:
      - display_menu.back: display_menu
```

## Actions

- `display_menu.back`: ends an edit in progress, or otherwise goes up one level. At the root
  with nothing being edited it does nothing — the menu never closes itself.

## From lambdas

- `back()`: the same, and returns whether it went anywhere. `false` is the case above, which is
  where a config closes the menu itself
- `is_at_main()`: the root menu is the one on screen
