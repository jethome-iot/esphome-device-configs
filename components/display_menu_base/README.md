# display_menu_base

Upstream's `display_menu_base` with a back action, a switch for what the "right" input does, a
`weight` that orders the rows of a menu, submenus that may be empty, and `fit_text` for a row
labelled with text the device did not author. Listing it in
`external_components` shadows the copy that ships with ESPHome; everything else about the
component is upstream's.

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
| `weight`                | `0`     | Where a row sits among its siblings: any integer, lower first, negative included. Rows of equal weight keep the order they are written in. Every item type takes it |

`right_for_menu_enter` belongs to whichever menu component carries it — `graphical_display_menu:`
or `lcd_menu:` — since both extend this schema. `weight` goes on the item.

```yaml
graphical_display_menu:
  id: display_menu
  right_for_menu_enter: false
  items:
    - type: label
      text: "About"
      weight: 10  # last, whatever order the packages built the list in
    - type: menu
      text: "Settings"
      items: ...

binary_sensor:
  - platform: gpio
    pin: GPIO14
    on_press:
      - display_menu.back: display_menu
```

## Ordering

Every `items:` list is sorted by weight on its own, while the config is validated — so a weight
also orders rows that another package appended to the same list with `!extend`, whatever the
package order is. It reaches only the rows written in YAML: what a lambda adds with `add_item()`
at boot lands at the end of the list, after every weighted row, in the order it was added.

## Empty submenus

A `type: menu` item may leave `items:` out or leave it empty; the top-level `items:` still needs
at least one row. An empty submenu is drawn as an ordinary row, but "enter" (and "right") on it
does nothing: it only opens once something has filled it — another package via `!extend`, or a
lambda at boot.

## Actions

- `display_menu.back`: ends an edit in progress, or otherwise goes up one level. At the root
  with nothing being edited it does nothing — the menu never closes itself.

## From lambdas

- `back()`: the same, and returns whether it went anywhere. `false` is the case above, which is
  where a config closes the menu itself
- `is_at_main()`: the root menu is the one on screen
- `fit_text(text, chars, can_draw)`: a row label for text the user wrote. Code points
  `can_draw` turns down become `?`, malformed UTF-8 counts as one of those, and the result is
  cut to `chars` characters — not bytes. Call it when the row is built, not while drawing:

  ```cpp
  auto *font = id(menu_font);
  row->set_text(fit_text(name, 12, [font](uint32_t cp) { return font->find_glyph(cp) != nullptr; }));
  ```
