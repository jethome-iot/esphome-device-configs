# web_origin_guard

Refuses cross-origin browser requests on the shared web server. There is no configuration block
to write: `web_file_browser`, `web_automation_editor` and `web_device_dashboard` auto-load it.
The name still has to be listed next to its user in `external_components`, or the build stops
with `Component not found: web_origin_guard`:

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/jethome-iot/esphome-device-configs
      ref: master
      path: components
    components: [web_origin_guard, web_file_browser]
```

First the `Host` the request was sent to has to name this device, or the answer is `403
Forbidden` whatever the `Origin`. It names the device when it is

- an IP literal, IPv4 or IPv6 in brackets;
- the node name, `<name>.local`, or any `<name>.<domain>`, such as the name a router's DNS gives
  the device (`<name>.lan`, `<name>.fritz.box`);

each with or without a port, case aside. A request with no `Host` passes this step: a browser
always sends one. While the captive portal runs with its access point up, a `GET` skips the step:
the portal answers every host, because a phone finds it by probing its vendor's own, and it
answers only `GET`.

This is what stops DNS rebinding: a page on `attacker.example` whose name is made to resolve to
the device's address is same-origin to the browser, so its requests carry `Host:
attacker.example` and an `Origin` that matches it, or none at all.

Then the rule is `web_server`'s own, so every handler on the port answers the same way:

- no `Origin` header — allowed. `curl`, a script, a Home Assistant `rest_command` and any other
  non-browser client send none.
- `Origin` whose authority equals the request's `Host` — allowed. This is the device's own page,
  whether it was opened by IP, by mDNS name or by a DHCP name.
- anything else — `403 Forbidden`, `text/plain`, before the handler runs.

The authority is compared, not the scheme: `https://device.local` reaching `Host: device.local`
passes. A different host, a different port, or an `Origin: null` from a sandboxed frame does not.

Why it exists: HTTP authentication is attached by the browser to any request a page makes to the
device, so a page on another site can drive an authenticated `POST` at these routes. Answering
POST only keeps a mutating route out of reach of an `<img src>`; it does not keep it out of
reach of a cross-site form. This does.

## Two mechanisms, two jobs

Neither replaces the other; deleting either one opens a hole the other does not cover.

**The wrapper.** Our three web components register a `WebOriginGuard` around themselves rather
than registering themselves, so there is no way to reach their handlers that does not pass the
check — including `handleBody` and `handleUpload`, which arrive before `handleRequest` and which
a handler may stream straight to a file. A refused `POST /files/write` opens no file and writes
no bytes.

**The catch-all.** `CrossOriginRefuser` is a component with no YAML of its own, registered ahead
of every handler on the server, whose `canHandle` is exactly "this request is cross-origin". It
claims a request only in order to refuse it, so an allowed request falls through to whatever
would have served it. It covers what no wrapper of ours can: `web_server`'s REST routes, its OTA
handler at `/update`, and anything a future component registers on the same server. It is
registered without authentication on purpose — a request from another site should be refused
outright, not answered with a challenge that makes the browser ask the person for credentials.

## Where this differs from `web_server`'s own check

- **`Host` is checked.** Upstream's check reads `Origin` alone, so a rebound page passes it. An
  ESPHome bump that moves the origin check into `web_server_base` for every handler keeps
  `host_names_device()` in front of it.
- **`allowed_origins` is not consulted.** Upstream's rule also accepts any origin listed in the
  `web_server:` `allowed_origins:` option. This does not read it, so a firmware that ever sets
  that option would have upstream's routes honour the list and these refuse it.
- **`GET /` is not exempt.** Upstream serves its index page before running the origin check;
  here a cross-origin `GET /` is refused like anything else. A browser navigating to a page
  sends no `Origin`, so this is invisible in normal use.
- **The `403` is written out by hand.** `AsyncWebServerRequest::send()` maps every status it does
  not know — 403 among them — to a 500, so the status line and the `text/plain` body are set
  through `httpd_resp_*` directly.

## What this breaks

Reaching the device through a name that is not its own: a DNS alias, or a reverse proxy that
forwards the browser's `Host` unchanged. Use its IP, its name, or a name under it.

A browser-based development server proxying to a real device. The dashboard's dev server sends
`Origin: http://localhost:5173` with `Host` rewritten to the device, so every mutating call
answers `403`. Strip or rewrite `Origin` in the proxy, or develop against the mock client. Any
reverse proxy in front of a device fails the same way if it rewrites `Host` while forwarding the
browser's `Origin` — Home Assistant ingress and a plain nginx front end both do by default.

Nothing that sends no `Origin` and reaches the device by its IP, its name or a name under it is
affected: `scripts/device-files.py`, the `curl` recipes in the component READMEs, the backup and
restore flow. The ESPHome native API and OTA do not go through this server at all.

## Tests

`tests/components/web_origin_guard/` drives both halves over the harness's `web_server_base`
stand-in: which hosts name the device and which are refused, with and without an `Origin`, that
the captive portal lifts the `Host` step for a `GET` and only that, which origins pass, which are refused, that a refused request reaches the wrapped
handler on none of `handleRequest`, `handleBody` and `handleUpload`, and that the catch-all
refuses a request bound for a handler it knows nothing about while leaving an allowed one
unclaimed. `test_schema.py` covers the configuration.
