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
  - platform: web_server        # multipart uploads exist only with this platform

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
`web_server` and inherits its `auth:` credentials. Without `auth:` the whole partition is
readable and writable by anything on the network.

## REST

Paths are relative to the mount, and `..` is rejected. Routes match on the URL path alone: the
HTTP method below is the conventional one, not an enforced one.

| Method | Path | |
|---|---|---|
| GET | `info` | `{"valid", "total", "used", "free", "filesystem"}` |
| GET | `list?path=/` | `[{"name", "type": "file"\|"directory", "size", "mtime"}, ...]` |
| GET | `read?path=` | `{"success", "content"}`, text files up to 1 MB |
| POST | `write?path=` | Raw request body becomes the file (any `Content-Type` but form-encoded); an empty body creates an empty file |
| POST | `upload?path=` | `multipart/form-data` with one file part; a zero-length part is an error, use `write` |
| GET | `download?path=` | The file, streamed |
| POST | `delete`, `mkdir` | Field `path`; delete is recursive, mkdir is idempotent and not recursive |
| POST | `rename`, `copy` | Fields `old_path`, `new_path`; copy is recursive |

Fields are read from the query string and from an urlencoded body alike. Every route except
`list`, `info` and `download` answers `{"success": true, "message"}` or, with a 400 (404 for a
missing path), `{"success": false, "error"}`.

```sh
curl 'http://<device>/files/info'
curl 'http://<device>/files/list?path=/'
curl -X POST 'http://<device>/files/write?path=/notes.txt' -H 'Content-Type: text/plain' --data-binary 'hello'
curl 'http://<device>/files/read?path=/notes.txt'
curl -X POST 'http://<device>/files/upload?path=/backup.json' -F file=@backup.json
curl -OJ 'http://<device>/files/download?path=/backup.json'
curl -X POST 'http://<device>/files/mkdir' -d 'path=/logs'
curl -X POST 'http://<device>/files/copy' -d 'old_path=/notes.txt&new_path=/logs/notes.txt'
curl -X POST 'http://<device>/files/rename' -d 'old_path=/notes.txt&new_path=/readme.txt'
curl -X POST 'http://<device>/files/delete' -d 'path=/logs'
```

`write` needs an explicit `Content-Type`: `curl` sends `application/x-www-form-urlencoded` by
default, the server then parses the body as form fields, and the file comes out empty.

## client/

`client/fileApi.ts` is the TypeScript client for these routes, `client/types.ts` the wire
types, and `client/mock/fileBrowserMock.ts` a dependency-free in-memory implementation of the
same routes for a dev server or unit tests. They are the contract a browser client codes
against and they live here so they change with the C++ that they mirror. Nothing in this
repository builds or type-checks them yet.
