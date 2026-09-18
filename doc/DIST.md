# dist/ and new devices

`dist/<device>.yaml` is the self-contained copy of a device config that the ESPHome Builder
imports through `dashboard_import`. `scripts/build-dist.py` generates it; never edit it by hand.

## What build-dist.py does

It flattens each device config with ESPHome's own package merge but skips the substitution pass,
so `${name}` and `${friendly_name}` stay symbolic for the Builder to rename. It:

- requires `dashboard_import.package_import_url` to pin a ref and to point at
  `dist/<device>.yaml`, never at the source config;
- rewrites `file:` values (the `ASSET_KEYS` set) that resolve to repository files into raw GitHub
  URLs under that ref, drops substitutions naming repository directories, and fails on any other
  value still pointing at a repository file: add the key to `ASSET_KEYS` if it names an asset,
  otherwise inline the value;
- rewrites local `external_components` sources (`source: ${components}`) into the git form
  (`type: git`, `url`, `ref`, `path`) under that same ref, so the Builder clones the components
  from the release branch;
- emits values YAML 1.1 would misread (`Yes`, `12:30`) as block scalars and verifies a ruamel
  round-trip leaves the config unchanged;
- leaves out the packages named in `EXCLUDED_PACKAGES` — `firmware-update` and the menu rows
  that drive it, because a firmware built from `dist/` is the importing user's own and the
  update entity would offer to replace it with JetHome's build;
- refuses to run while a device config is missing from `firmwares.yaml`, or while a config's
  `fw_device` is not the slug that file publishes it under.

## Adding a device

1. `devices/<family>/<device>.yaml` with the path substitutions (`assets`, `components`, `boards`,
   `features`, `display`) and the package list.
2. The import pointing at the generated file:

   ```yaml
   dashboard_import:
     package_import_url: github://jethome-iot/esphome-device-configs/dist/<device>.yaml@master
     import_full_config: true
   ```

   `@master` on purpose: users import from the release branch, not from `dev`.

3. An entry in `firmwares.yaml`: `config`, the `device` slug on fw.jethome.com, `upload`.
4. `python scripts/build-dist.py`, then commit `dist/`.
