# virtual_display

A display with no bus behind it, served over HTTP as a front panel page: the
screen as a canvas, plus on-screen keys that publish into real binary sensors.

Pages, fonts and menus are written against ESPHome's generic display API — the
driver is only the part that pushes a finished buffer at a chip. Swap the driver
and the same code renders somewhere else, unchanged. That makes a
display-carrying device testable where its panel cannot exist: under QEMU, which
emulates no I2C controller, so the physical SSD1306/SH1106 is dead and its
joystick with it.

```yaml
display:
  - platform: virtual_display
    id: display1
    width: 128
    height: 64
    url_prefix: /panel
    keys:
      up: up_button_id
      down: down_button_id
      enter: center_button_id
```

Open `http://<device>/panel`.

## Configuration

| Option | Default | |
|---|---|---|
| `width` / `height` | `128` / `64` | Framebuffer size, 1 bpp. |
| `url_prefix` | `/panel` | Where the page and its endpoints live. |
| `hold_time` | `120ms` | How long an injected press stays down when the caller does not manage down/up itself. |
| `keys` | — | Key name → the `binary_sensor` it publishes to. |

Everything from the standard display schema (`pages`, `lambda`, `rotation`,
`update_interval`, …) applies as usual.

**The mapped sensors must be ones nothing else drives every loop** — `template`
ones. A `gpio` sensor re-reads its pin on each loop and would overwrite the
injected state before any automation saw it.

## Endpoints

| | |
|---|---|
| `GET <prefix>` | the front panel page |
| `GET <prefix>/info` | `{"width","height","keys":[…]}` |
| `GET <prefix>/frame` | raw 1 bpp framebuffer, MSB first, row-major, served from a snapshot of the last finished render. `X-Frame-Id` counts renders and always matches the bytes sent with it; `?since=<id>` answers **204** while that frame is still the newest |
| `POST <prefix>/key/<name>` | inject a key. `?action=down` / `?action=up` hold and release it; without one the press releases itself after `hold_time`. Any other method gets 405 |

Key names are limited to letters, digits, `_` and `-`, so the posted path is the
name itself whatever the client encodes.

A frame is `((width + 7) / 8) * height` bytes — 1024 for 128×64. Scripted use is
just as easy as the page:

```bash
curl -s http://device/panel/frame -o frame.bin
curl -s -X POST -d "" http://device/panel/key/enter   # an empty body: httpd wants Content-Length
```

## Why a frame is a snapshot

`update()` renders on the main loop; the HTTP handler runs on the httpd task
ESP-IDF starts for the web server, not inside `loop()`. Serving the live
framebuffer would let a response carry half of one frame and half of the next,
and report an `X-Frame-Id` that belongs to neither.

So the finished frame is copied into a second buffer at the end of `update()`,
with its id, under a mutex, and `/frame` reads both back out under the same
mutex. Diffing two frames therefore compares two renders that actually happened,
which is the whole point of the endpoint. The lock is held only for a 1 KB
`memcpy` on either side — the render itself is never inside it, so the HTTP task
cannot stall the display.

## Why a press publishes a release first

Every injected press publishes `false` before `true`. `on_press` fires on a
false→true edge, and `publish_state(true)` on a sensor that is already `true` is
dropped by the de-dup in `send_state_internal()` — so a press landing on a key
something else left held would run no automation at all. Publishing the release
first states the edge the way hardware does.

It costs nothing when the key was already up: that de-dup sits *after* the filter
chain, so the `false` still reaches any filters (a latching one — `autorepeat`,
`delayed_on` — needs to see it) and stops before the callbacks.
