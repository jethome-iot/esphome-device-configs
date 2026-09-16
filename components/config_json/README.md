# config_json

Keeps settings types as JSON files on a `filesystem_storage_abstract` mount, one file per type,
loaded at boot and written back a few seconds after a change. A settings type is a C++ class —
`components/jxd_config` supplies the ones this firmware uses.

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/jethome-iot/esphome-device-configs
      ref: master
      path: components
    components: [config_base, config_json]

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
| `config_dir` | `config` | Directory below the mount's base path, created at setup      |
| `save_delay` | `10s`    | Debounce: edits inside this window end in one write          |

## Files

One `<config_dir>/<key>.json` per settings type, `{"version": 1, "records": [...]}`, where the
key is the type's own (`switch`, `binary_sensor`). They are plain files and can be edited on the
partition. Unknown keys, in the document and in a record, are ignored; a record the type rejects
is logged and dropped, the rest of the file still loads.

A file that is missing, empty, larger than 64 KiB or not parseable leaves the type at its
compiled defaults and is left on disk untouched — nothing is dirty, so the boot writes nothing
and a file that is corrupt for an unrelated reason is not overwritten by mistake. Writes are not
atomic: a power cut mid-save truncates the file, which reads back as defaults on the next boot.

## Boot order

The keeper sets up at `HARDWARE + 5` and loads every file. Each type then declares an
`APPLY_PRIORITY` at which its values are pushed into the entities — `HARDWARE + 1` for the types
in `jxd_config`, i.e. after the load and before the entities set themselves up.

## From lambdas

- `save()`, `save("<key>")`: schedule the debounced write; an unknown key is logged and ignored
- `save_immediate()`, `save_immediate("<key>")`: write now
- `is_save_pending()`, `cancel_pending_save()`
- `reset_all()`: clear every type and write at once
