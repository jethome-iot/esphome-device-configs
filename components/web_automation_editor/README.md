# web_automation_editor

The JSON/REST API of the `automations` component, served on the device's own web server under
`/automation-editor/api/`. List, read, create, update, delete and export the rules, discover the
entities a rule may name, and read what the editor may offer — everything a browser editor does,
and everything `curl` can do without one. ESP-IDF only.

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/jethome-iot/esphome-device-configs
      ref: master
      path: components
    components: [filesystem_storage_abstract, littlefs_storage, automations, web_origin_guard, web_automation_editor]

web_server:
  port: 80

littlefs_storage:
  id: user_storage

automations:
  storage: user_storage

web_automation_editor:
```

## Options

| Option           | Default              | Meaning                                                                  |
| ---------------- | -------------------- | ------------------------------------------------------------------------ |
| `automations_id` | the only `automations` | The engine to edit                                                     |
| `url_prefix`     | `/automation-editor` | The routes live at `<url_prefix>/api/`. A dashboard bakes this in at build time, so changing it needs the client rebuilt |

The handler registers on the shared `web_server_base`, so it answers on the same port as
`web_server` and behind its `auth:` credentials. Those credentials come from the `web_server:`
block and from nowhere else: with no `web_server:`, or no `auth:` in it, anything that can reach
the port can rewrite every rule and reboot the device. The configs in this repository set them,
and [`web_auth`](../web_auth/README.md) lets the device replace the pair. Credentials alone do
not settle it: a browser attaches them to whatever a page on another site makes it fetch, so
every route here is also behind [`web_origin_guard`](../web_origin_guard/README.md), which
answers `403` to a request whose `Origin` is not the `Host` it was sent to. Clients that send no
`Origin`, `curl` among them, are unaffected.

## REST

Routes match on the exact URL path under `<url_prefix>/api/`, and each answers the one method
below: anything else is `405` with an `Allow` header.

Every route that touches the rules runs its whole read or write on the loop task, which owns
them, and answers `503 Service Unavailable` when the loop does not get to it within five
seconds. Nothing was read or written then, and the call can simply be made again.

| Method | Path | |
|---|---|---|
| GET | `list` | `{"automations": [{"id", "name", "enabled", "trigger_count", "action_count", "else_action_count", "mode"}, ...]}` |
| GET | `get?id=` | One rule, in the file format of [automations](../automations/README.md#a-rule) |
| POST | `save` | A rule as a JSON body; `id` absent or `0` creates, an existing `id` replaces that rule, renamed or not. Answers `{"success": true, "message", "id"}` on create, without `id` on update |
| POST | `delete?id=` | Removes the rule and its file; `Failed to delete automation` when the engine refuses |
| GET | `export` | `{"version": 1, "automations": [...]}`, every rule as `get` returns it — the backup file a client re-imports rule by rule through `save`, with each `id` dropped so the rules are created |
| GET | `entities` | `{"binary_sensors", "sensors", "switches"}`, each `[{"object_id", "name"}, ...]`, sensors with `"unit"`; internal entities are left out |
| GET | `schema` | The trigger, condition and action types and subtypes and the cron presets the engine parses, for an editor's menus |
| POST | `reboot` | Answers, then `App.safe_reboot()` |
| GET | `ping` | `{"status": "ok"}` |

`save` reads a raw JSON body, so it needs a `Content-Type` that is not a form:
`application/json`. A form-encoded body is parsed into fields by the server and answered
`Empty request body`; a body over 16 KiB, the engine's own ceiling on a rule file, is `413`. A
name another rule already owns — names collide by file name, so `Porch light` and `porch-LIGHT`
are one — is refused with the reason, and the engine's own refusals (a rule limit reached, a
file the loader had refused under that name, a call from inside a running rule) come back as
`Failed to create automation` / `Failed to update automation`.

Every failure is `{"success": false, "error"}`: `400` for a bad request (an `id` that is missing
or not a plain non-zero number included), `404` for an `id` no
rule has (`get`, `delete`, and a `save` carrying one), `405` for the wrong method, `413` for an
oversized body. The same contract,
machine-readable: [openapi.yaml](openapi.yaml) (OpenAPI 3.1).

A firmware with an `auth:` block wants the credentials on every one of these; `--digest -u`
covers what the configs in this repository build.

```sh
A='--digest -u admin:admin'
curl $A 'http://<device>/automation-editor/api/list'
curl -X POST $A 'http://<device>/automation-editor/api/save' -H 'Content-Type: application/json' \
  --data-binary @porch-light.json
curl -X POST $A 'http://<device>/automation-editor/api/delete?id=3' -d ''
curl $A 'http://<device>/automation-editor/api/export' > automations.json
```

A POST without a body needs `-d ''`: `curl` then sends the `Content-Length: 0` that
ESP-IDF's server insists on (`411` without it), as a browser's `fetch` does on its own.

## client/

`client/automationApi.ts` is the TypeScript client for these routes, `client/types.ts` the wire
types, `client/cron.ts` and `client/naming.ts` what an editor needs to predict the device (how a
cron expression comes back re-serialized, which names collide), and
`client/mock/automationMock.ts` a dependency-free in-memory implementation of the same routes
for a dev server or unit tests. They are the contract a browser client codes against and they
live here so they change with the C++ that they mirror. Nothing in this repository builds or
type-checks them yet.

## Testing

`python tests/run.py web_automation_editor` builds the handler for the ESPHome `host` platform
over the real engine, a directory standing in for the flash and the harness's `web_server_base`
stand-in, and drives every route through it; the cases are in
`tests/components/web_automation_editor/`. See [doc/TESTING.md](../../doc/TESTING.md).
