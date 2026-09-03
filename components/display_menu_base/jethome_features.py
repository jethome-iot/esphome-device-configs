"""JetHome deviations from upstream ESPHome 2026.8.2.

Single source of truth for every difference between the vendored ``display_menu_base`` /
``graphical_display_menu`` and their upstream counterparts.  Everything is on; turn one off to
build the upstream behaviour and see exactly what the deviation is.

Each enabled name is emitted as a C++ define by ``display_menu_to_code()`` (``__init__.py``), so
the C++ side picks it up out of ``esphome/core/defines.h`` -- the same mechanism upstream uses for
its own ``USE_*`` flags.  The Python side gates the YAML surface with the same dict, so a flag that
is off has no config key, no action registration and no codegen call.
"""

FEATURES = {
    # display_menu_base
    #
    # back() navigation entry point, the BackAction and the display_menu.back action.
    "JETHOME_MENU_BACK": True,
    # right_for_menu_enter option: whether "right" enters a submenu or only "enter" does.
    "JETHOME_MENU_RIGHT_FOR_MENU_ENTER": True,
    # reset_menu() / is_at_main(): return to the root item without showing the menu.
    "JETHOME_MENU_RESET": True,
    # Read-only `value` item type: MenuItemValueBase, MenuItemValue, MENU_ITEM_VALUE, and the
    # move of set_value_lambda()/value_getter_/has_value()/get_value_text() off MenuItemEditable
    # and MenuItemCustom onto the new base.
    "JETHOME_MENU_ITEM_VALUE": True,
    # graphical_display_menu
    #
    # restore_page option: whether hiding the menu restores the previously shown display page.
    "JETHOME_GDM_RESTORE_PAGE": True,
    # fill_row option: whether a row's background spans the full width.
    "JETHOME_GDM_FILL_ROW": True,
    # shrink_label option, plus shrink_text_to_width_() and the label/value split it needs.
    "JETHOME_GDM_SHRINK_LABEL": True,
    # Rewritten default menu_item_value lambda ("label: value", parentheses for switches only).
    "JETHOME_GDM_VALUE_FORMAT": True,
}

# flag -> flags it cannot be built without.
#
# The eight flags above are mutually independent: nothing gated by one names a symbol gated by
# another, so this is empty today.  Add an entry the moment that stops being true, so a bad
# combination fails here with a readable message instead of as a C++ error mid-build.
REQUIRES: dict[str, tuple[str, ...]] = {}


def _validate() -> None:
    for name, enabled in FEATURES.items():
        if not isinstance(enabled, bool):
            raise ValueError(
                f"jethome_features: {name} must be True or False, got {enabled!r}"
            )
    for name, deps in REQUIRES.items():
        if name not in FEATURES:
            raise ValueError(f"jethome_features: REQUIRES names unknown flag {name}")
        for dep in deps:
            if dep not in FEATURES:
                raise ValueError(
                    f"jethome_features: {name} requires unknown flag {dep}"
                )
            if FEATURES[name] and not FEATURES[dep]:
                raise ValueError(
                    f"jethome_features: {name} is on but requires {dep}, which is off"
                )


_validate()
