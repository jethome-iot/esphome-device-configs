# Local ESPHome components

This directory holds the `external_components` of this repository. Two of them —
`display_menu_base` and `graphical_display_menu` — are **vendored copies of upstream ESPHome
components**, not original code. The rest (`groups`, `modbus_server_group`) are ours end to end.

## Why the two menu components are vendored

The device menu needs behaviour upstream does not offer, and the changes are too intrusive for a
subclass: they touch protected members, add a new `MenuItemType` enumerator, change a class
hierarchy (`MenuItemEditable` now derives from a new `MenuItemValueBase`), and add config keys to
schemas defined in upstream `__init__.py`. So the components are copied wholesale and patched in
place.

What the local patches add:

* `display_menu_base` — a `back()` navigation entry point and a `display_menu.back` action, a
  `reset_menu()` / `is_at_main()` pair, a `right_for_menu_enter` option that stops "right" from
  entering submenus, and a read-only `value` menu item type (`MenuItemValue`) built on the new
  `MenuItemValueBase`.
* `graphical_display_menu` — `restore_page`, `fill_row` and `shrink_label` options, a
  middle-ellipsis `shrink_text_to_width_()` helper, and a different default `menu_item_value`
  formatting.

## Tracked upstream version

**ESPHome 2026.8.2** — the version pinned in `requirements.txt`. Both directories are byte-identical
to `esphome/components/<name>` of that release apart from the marked regions described below.

## Marker convention

Every local deviation from upstream is delimited in the source, so a re-sync can be done by eye.

C++ (`.h`, `.cpp`) — a block:

```cpp
// JETHOME-BEGIN: <what the block is>
...local code...
// JETHOME-END
```

C++ — a single modified upstream line:

```cpp
  bool right_for_menu_enter_opt_{true};  // JETHOME: right-enters-submenu flag
```

Python (`__init__.py`) — the same two forms with `#`:

```python
# JETHOME-BEGIN: <what the block is>
...local code...
# JETHOME-END

CONF_RIGHT_FOR_MENU_ENTER = "right_for_menu_enter"  # JETHOME: option key
```

Rules:

* A marker says **what** the local change is, in one short clause. Never why — that belongs in the
  commit message.
* **Deleted upstream code cannot be bracketed.** At each site where upstream code was removed, a
  single `// JETHOME: <what was removed and what replaced it>` line is left behind, e.g.
  `// JETHOME: value_getter_ removed here, now on MenuItemValueBase` in `menu_item.h`.
* A renamed symbol is a modification, not an addition: it gets a trailing marker on the changed
  line, e.g. `menu_item.cpp`'s `MenuItemValueBase::get_value_text()` (upstream:
  `MenuItemCustom::get_value_text()`).

The invariant to preserve: **every hunk in the diff against upstream is either a marker line or sits
inside a marked region, and no marker sits on unchanged upstream code.**

## Re-diffing against upstream

Fetch the upstream tree for the version in `requirements.txt`, then, from the repository root:

```sh
diff -ru -x __pycache__ <upstream>/esphome/components/<c> components/<c>
```

for `<c>` in `display_menu_base` and `graphical_display_menu`, where `<upstream>` is an unpacked
ESPHome source tree (e.g. `pip download esphome==2026.8.2 --no-deps --no-binary :all:` and unpack,
or a `git clone` of `esphome/esphome` checked out at tag `2026.8.2`).

## Upgrading to a newer ESPHome

1. Diff the *upstream* copies of both components between the old and the new version to see what
   changed on their side.
2. Copy the new upstream files over the vendored ones.
3. Re-apply every marked region from git history, keeping the markers.
4. Re-run the two `diff -ru` commands above and check the invariant still holds.
5. Update the tracked version stated in this file.
