# Local ESPHome components

This directory holds the `external_components` of this repository. Two of them —
`display_menu_base` and `graphical_display_menu` — are **vendored copies of upstream ESPHome
components**, not original code. The rest (`groups`, `modbus_server_group`) are ours end to end.

## Why the two menu components are vendored

The device menu needs behaviour upstream does not offer, and the changes are too intrusive for a
subclass: they touch protected members, add a new `MenuItemType` enumerator, change a class
hierarchy (`MenuItemEditable` derives from a new `MenuItemValueBase`), and add config keys to
schemas defined in upstream `__init__.py`. So the components are copied wholesale and patched in
place.

## Tracked upstream version

**ESPHome 2026.8.2** — the version pinned in `requirements.txt`. Both directories are upstream
`esphome/components/<name>` of that release plus the gated additions described below, and nothing
else.

## Feature flags

Every deviation from upstream sits behind a named flag. The flags are declared in one place —
[`display_menu_base/jethome_features.py`](display_menu_base/jethome_features.py) — and gate **both**
sides:

* **Python.** The same `FEATURES` dict removes the YAML surface: the config key stops validating,
  the action stops being registered, the codegen call disappears. A flag that is off leaves no key
  that validates but generates nothing.
* **C++.** `display_menu_to_code()` emits `cg.add_define(<name>)` for each enabled flag, so the name
  arrives in `esphome/core/defines.h` exactly like upstream's own `USE_*` flags, and the sources gate
  on it with `#ifdef`. Every file that tests a flag includes `esphome/core/defines.h` itself rather
  than relying on a transitive include.

`display_menu_to_code()` is the single emission point because every display-menu platform calls it,
so both components get the defines from one place.

Where a local change *replaced* an upstream line rather than adding to it, the upstream version is
restored under `#else`. Turning a flag off therefore restores upstream behaviour, it does not just
delete a feature.

### `display_menu_base`

| Flag | Covers |
| --- | --- |
| `JETHOME_MENU_BACK` | `DisplayMenuComponent::back()` and its declaration, `BackAction` in `automation.h`, the `display_menu.back` action registration. |
| `JETHOME_MENU_RIGHT_FOR_MENU_ENTER` | The `right_for_menu_enter` config key, `set_right_for_menu_enter_opt()`, the `right_for_menu_enter_opt_` member and the guard in `right()`. The guard is an in-place edit of an upstream line, so `#else` restores the unconditional `changed = this->enter_menu_();`. |
| `JETHOME_MENU_RESET` | `reset_menu()` and `is_at_main()`. |
| `JETHOME_MENU_ITEM_VALUE` | The read-only `type: value` item. Adds `MenuItemValueBase`, `MenuItemValue`, the `MENU_ITEM_VALUE` enumerator and its `menu_item_type_to_string()` case, the `type: value` schema branch and the `MENU_ITEM_TYPES` / `MENU_ITEMS_WITH_SPECIALIZED_CLASSES` entries. It also **restructures upstream**: `set_value_lambda()` / `value_getter_` move off `MenuItemEditable`, and `has_value()` / `get_value_text()` move off `MenuItemCustom`, onto the new base. With the flag off both classes are upstream verbatim under `#else` / `#ifndef`, and `menu_item.cpp` defines `MenuItemCustom::get_value_text()` again. |

### `graphical_display_menu`

| Flag | Covers |
| --- | --- |
| `JETHOME_GDM_RESTORE_PAGE` | The `restore_page` key, constant, codegen call, setter, `restore_page_` member and the guard in `on_before_hide()`. |
| `JETHOME_GDM_FILL_ROW` | The `fill_row` key, constant, codegen call, setter, `fill_row_` member and the `dimensions.w` assignment in `measure_item_()`. |
| `JETHOME_GDM_SHRINK_LABEL` | The `shrink_label` key, constant, codegen call, setter, `shrink_label_` member, the `shrink_text_to_width_()` helper and its `#include <cstring>`, and the label/value split in `draw_item_()` that the shrinking needs. `#else` restores upstream's plain `label.append(...)`. |
| `JETHOME_GDM_VALUE_FORMAT` | The rewritten default `menu_item_value` lambda (`"label: value"`, parentheses only for switches). Upstream's body is under `#else`. |

The flags are mutually independent — nothing gated by one names a symbol gated by another. If that
ever stops being true, record it in `REQUIRES` in `jethome_features.py`, which raises at import time
with a readable message instead of letting the build die on a C++ error.

## Turning a flag off

Edit `display_menu_base/jethome_features.py` and set the entry to `False`:

```python
FEATURES = {
    "JETHOME_MENU_ITEM_VALUE": False,
    ...
}
```

Then rebuild. Any YAML still using that feature fails validation with a normal ESPHome message
(`[fill_row] is an invalid option for [graphical_display_menu]`, `Unknown value 'value', valid
options are ...`, `Unable to find action with the name 'display_menu.back'`), not a traceback.

Note that `JXD/jxd-r6-e1eth-lcd-eth.yaml` uses every flag, so it only validates with all of them on.
To exercise an off state, write a throwaway upstream-only menu config outside the repository.

## Re-diffing against upstream

Fetch the upstream tree for the version in `requirements.txt`, then, from the repository root:

```sh
diff -ru -x __pycache__ <upstream>/esphome/components/<c> components/<c>
```

for `<c>` in `display_menu_base` and `graphical_display_menu`, where `<upstream>` is an unpacked
ESPHome source tree (e.g. `pip download esphome==2026.8.2 --no-deps --no-binary :all:` and unpack,
or a `git clone` of `esphome/esphome` checked out at tag `2026.8.2`).

**The invariant: turn every flag off and that diff must contain no deletions.** Every hunk is then a
pure insertion — an `#ifdef` / `#else` / `#endif`, a gated block, an `if FEATURES[...]:` block, an
`#include "esphome/core/defines.h"`, or the new `jethome_features.py` file. An upstream line that is
gone or altered means the gate is wrong.

## Upgrading to a newer ESPHome

1. Diff the *upstream* copies of both components between the old and the new version to see what
   changed on their side.
2. Copy the new upstream files over the vendored ones.
3. Re-apply every gated region from git history, keeping the flags.
4. Set every flag to `False` and re-run the two `diff -ru` commands; the diff must show no
   deletions.
5. Set every flag back to `True` and compile `JXD/jxd-r6-e1eth-lcd-eth.yaml`, which exercises all of
   them.
6. Update the tracked version stated in this file.
