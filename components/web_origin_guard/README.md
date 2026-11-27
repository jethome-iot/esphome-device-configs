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

The rule is `web_server`'s own, so every handler on the port answers the same way:

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

Before ESPHome 2026.11 neither replaced the other. Since then upstream's own `add_handler()`
guard applies the same rule to everything registered through it, our wrapped components
included, so the wrapper repeats it, and only the catch-all covers what is registered without
it (#348).

**The wrapper.** Our three web components register a `WebOriginGuard` around themselves rather
than registering themselves, so there is no way to reach their handlers that does not pass the
check — including `handleBody` and `handleUpload`, which arrive before `handleRequest` and which
a handler may stream straight to a file. A refused `POST /files/write` opens no file and writes
no bytes.

**The catch-all.** `CrossOriginRefuser` is a component with no YAML of its own, registered ahead
of every handler on the server, whose `canHandle` is exactly "this request is cross-origin". It
claims a request only in order to refuse it, so an allowed request falls through to whatever
would have served it. It covers what nothing else does: whatever is registered with
`add_handler_without_auth()`, the captive portal among it; before 2026.11 that also took in
`web_server`'s REST routes and its OTA handler at `/update`. It is
registered without authentication on purpose — a request from another site should be refused
outright, not answered with a challenge that makes the browser ask the person for credentials.

## Where this differs from `web_server`'s own check

Since ESPHome 2026.11 `web_server_base` applies the same rule to every handler registered with
`add_handler()`; this component still covers what is registered without it, the captive portal
among it (#348).

- **`allowed_origins` is not consulted.** Upstream's rule also accepts any origin listed in the
  `web_server:` `allowed_origins:` option, and a build that allows any origin skips it. This
  does not read either, and the catch-all answers before every other handler, so a firmware that
  ever sets that option would have cross-origin requests refused on upstream's routes as well as
  ours.
- **The `403` is written out by hand.** `AsyncWebServerRequest::send()` turned 403 into a 500
  before ESPHome 2026.11, so the status line and the `text/plain` body are set through
  `httpd_resp_*` directly.

## What this breaks

A browser-based development server proxying to a real device. The dashboard's dev server sends
`Origin: http://localhost:5173` with `Host` rewritten to the device, so every mutating call
answers `403`. Strip or rewrite `Origin` in the proxy, or develop against the mock client. Any
reverse proxy in front of a device fails the same way if it rewrites `Host` while forwarding the
browser's `Origin` — Home Assistant ingress and a plain nginx front end both do by default.

Nothing that does not send `Origin` is affected: `scripts/device-files.py`, the `curl` recipes
in the component READMEs, the backup and restore flow, and the ESPHome native API and OTA, which
do not go through this server at all.

## Tests

`tests/components/web_origin_guard/` drives both halves over the harness's `web_server_base`
stand-in: which origins pass, which are refused, that a refused request reaches the wrapped
handler on none of `handleRequest`, `handleBody` and `handleUpload`, and that the catch-all
refuses a request bound for a handler it knows nothing about while leaving an allowed one
unclaimed. `test_schema.py` covers the configuration.
