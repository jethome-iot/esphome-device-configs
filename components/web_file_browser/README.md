# web_file_browser

A JSON/REST file API over a `filesystem_storage_abstract` mount, served on the device's own
web server under `/files/`. Upload, download, read, edit, list, create, rename, copy and
delete — everything the dashboard's Files screen does, and everything `curl` can do without
one. ESP-IDF only.

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/jethome-iot/esphome-device-configs
      ref: master
      path: components
    components: [littlefs_storage, web_file_browser]

web_server:
  port: 80
ota:
  - platform: web_server        # multipart uploads exist only with this platform;
                                # the component fails the config without it

littlefs_storage:
  id: user_storage
  partition_label: littlefs
  partition_size: 4MB
  base_path: /littlefs

web_file_browser:
  storage_id: user_storage
```

## Options

| Option       | Default  | Meaning                                                                 |
| ------------ | -------- | ----------------------------------------------------------------------- |
| `storage_id` |          | Required. The mount to serve; any `filesystem_storage_abstract` backend  |
| `url_prefix` | `/files` | Where the routes live. A dashboard bakes this in at build time, so changing it needs the client rebuilt |

The handler registers on the shared `web_server_base`, so it answers on the same port as
`web_server` and behind its `auth:` credentials — requests, raw bodies and multipart uploads
alike. Those credentials come from the `web_server:` block and from nowhere else: with no
`web_server:`, or no `auth:` in it, the whole partition is readable and writable by anything
that can reach the port. The configs in this repository set no `auth:`, so that is how they
ship. Method enforcement is not a substitute — it keeps a mutating route out of reach of an
`<img src>`, but a cross-site form can still POST.

## REST

Paths are relative to the mount, and `..` is rejected. Routes match on the exact URL path, and
each answers the one method below: anything else is `405` with an `Allow` header.

| Method | Path | |
|---|---|---|
| GET | `info` | `{"valid", "total", "used", "free", "filesystem"}` |
| GET | `list?path=/` | `[{"name", "type": "file"\|"directory", "size", "mtime"}, ...]`; `mtime` is the last write in seconds since the epoch, 0 where the filesystem keeps none (every directory) |
| GET | `read?path=` | `{"success", "content"}`, escaped and streamed; over 1 MB is refused |
| POST | `write?path=` | Raw request body becomes the file; an empty body creates an empty file, a form-encoded or multipart one is refused |
| POST | `upload?path=` | `multipart/form-data` with one file part; a zero-length part is an error, use `write` |
| GET | `download?path=` | The file, streamed, with a `Content-Disposition` filename |
| POST | `delete`, `mkdir` | Field `path`; delete is recursive but refuses the mount root, mkdir is idempotent and not recursive |
| POST | `rename`, `copy` | Fields `old_path`, `new_path`; both refuse an existing destination, copy is recursive and refuses its own subtree |

Fields are read from an urlencoded body first and from the query string second, and `+` decodes
to a space. `list` and `info` answer bare JSON and `download` the file; every other success is
`{"success": true, "message"}`, and every failure `{"success": false, "error"}` with a 400 — 404
when the path to read, download, delete, rename or copy does not exist, 405 with an `Allow`
header for the wrong method. The same contract, machine-readable: [openapi.yaml](openapi.yaml)
(OpenAPI 3.1).

`list`, `read` and `download` stream through a fixed 4 KB buffer and never hold a
response-sized one, so `read`'s 1 MB ceiling is about the editor on the other end, not about the
device's heap. A read that fails partway leaves the JSON unterminated (`list`, `read`) or drops
the connection (`download`), so an incomplete answer cannot pass for a complete short one.
`delete` and `copy` recurse at most eight directory levels — a deeper tree is an error before
anything is removed, not a smashed web server stack.

```sh
curl 'http://<device>/files/list?path=/'
curl -X POST 'http://<device>/files/write?path=/notes.txt' -H 'Content-Type: text/plain' --data-binary 'hello'
curl -X POST 'http://<device>/files/upload?path=/notes.bin' -F file=@notes.bin
curl -X POST 'http://<device>/files/rename' -d 'old_path=/notes.txt&new_path=/readme.txt'
```

`write` needs an explicit `Content-Type`: `curl` sends `application/x-www-form-urlencoded` by
default, which the server parses as form fields and refuses rather than write an empty file.

## client/

`client/fileApi.ts` is the TypeScript client for these routes, `client/types.ts` the wire
types, and `client/mock/fileBrowserMock.ts` a dependency-free in-memory implementation of the
same routes for a dev server or unit tests. They are the contract a browser client codes
against and they live here so they change with the C++ that they mirror. Nothing in this
repository builds or type-checks them yet.

The mock keeps file bytes as binary strings, one character per byte, so a transport around it
hands `handle()` the request body decoded as latin1 — never utf8 — and writes what
`rawDownload()` returns verbatim. `createMockFetch()` does both; a dev server does the same by
hand and answers `GET <prefix>/download` from `rawDownload()` before calling `handle()`, which
does not claim it.

## scripts/device-files.py

The same routes from a terminal, standard library only; `--host` or `DEVICE_HOST` names the
device.

```sh
scripts/device-files.py --host 192.168.1.50 ls -l /config
scripts/device-files.py --host 192.168.1.50 put -r ./www /www
scripts/device-files.py --host 192.168.1.50 backup                    # 192.168.1.50-<stamp>.tar.gz
scripts/device-files.py --host 192.168.1.50 restore jxd.tar.gz --clean
DEVICE_HOST=192.168.1.50 scripts/device-files.py shell
```

Also `info`, `tree`, `cat`, `get`, `write`, `rm`, `mkdir -p`, `mv`, `cp` and `edit`; a directory
needs `-r`, and `restore` deletes what the archive lacks only with `--clean`.
