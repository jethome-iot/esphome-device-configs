# config_json

Keeps settings types as JSON files on a `filesystem_storage_abstract` mount, one file per type,
loaded at boot and written back a few seconds after a change. A settings type is a C++ class —
`components/entity_config` supplies the ones this firmware uses.

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/jethome-iot/esphome-device-configs
      ref: master
      path: components
    components: [filesystem_storage_abstract, littlefs_storage, config_base, config_json]

littlefs_storage:
  id: user_storage
  partition_label: littlefs
  partition_size: 4MB
  base_path: /littlefs

config_json:
  id: config_json_keeper
  storage: user_storage
```

`config_base` carries the keeper and the settings interface; it has nothing to configure, but it
has to be named in `components:` or the auto-load fails.

## Options

| Option       | Default  | Meaning                                                      |
| ------------ | -------- | ------------------------------------------------------------ |
| `storage`    |          | A `filesystem_storage_abstract` mount, such as `littlefs_storage` |
| `config_dir` | `config` | One folder below the mount's base path, created at setup     |
| `save_delay` | `10s`    | Debounce: edits inside this window end in one write          |

## Files

One `<config_dir>/<key>.json` per settings type, `{"version": 1, "records": [...]}`, where the
key is the type's own (`switch`, `binary_sensor`). They are plain files and can be edited on the
partition. Unknown keys, in the document and in a record, are ignored; a record the type rejects
is logged and dropped, the rest of the file still loads; of two records for the same entity the
later one wins, with a warning.

A file that is missing, empty, larger than 64 KiB or not parseable leaves the type at its
compiled defaults and is left on disk untouched; a type that would serialize past that limit is
not written either, and stays dirty — nothing is dirty, so the boot writes nothing
and a file that is corrupt for an unrelated reason is not overwritten by mistake. A save writes
`<key>.json.tmp` and renames it over the file, so a power cut mid-save keeps the last good copy.

## When the mount is not there

The keeper fails at setup and stays read-only for the run: every save is refused with an error
in the log rather than queued, and nothing is written at shutdown either. Ask `can_save()`
before offering an edit — `web_device_dashboard` answers `503` on it, the display menu's rows
change the live value and log that it will not survive the reboot.

## Boot order

The keeper sets up at `HARDWARE + 5` and loads every file. Each type then declares an
`APPLY_PRIORITY` at which its values are pushed into the entities — `HARDWARE + 1` for the types
in `entity_config`, i.e. after the load and before the entities set themselves up.

## From lambdas

- `save()`, `save("<key>")`: schedule the debounced write; an unknown key is logged and ignored
- `save_immediate()`, `save_immediate("<key>")`: write now
- `is_save_pending()`, `cancel_pending_save()`
- `reset_all()`: clear every type and write at once
- `can_save()`: false when the mount failed, so nothing written here would survive a reboot
