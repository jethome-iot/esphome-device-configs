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
| `GET <prefix>/frame` | raw 1 bpp framebuffer, MSB first, row-major. `X-Frame-Id` counts renders; `?since=<id>` returns an **empty body** while that frame is still current |
| `POST <prefix>/key/<name>` | inject a key. `?action=down` / `?action=up` hold and release it; without one the press releases itself after `hold_time` |

Key names are limited to letters, digits, `_` and `-`, so the posted path is the
name itself whatever the client encodes.

A frame is `((width + 7) / 8) * height` bytes — 1024 for 128×64. Scripted use is
just as easy as the page:

```bash
curl -s http://device/panel/frame -o frame.bin
curl -s -X POST -d "" http://device/panel/key/enter   # an empty body: httpd wants Content-Length
```

## Why a press publishes a release first

Every injected press publishes `false` before `true`. A latching filter —
`autorepeat`, which the JXD buttons carry — drops a press while it still believes
the previous one is held, and on a device whose keys were never physically
released nothing ever told it otherwise, so the first press after boot would
vanish. Publishing the release first states the edge the way hardware does. It
costs nothing when the key was already up: `publish_state()` de-dups only *after*
the filter chain has seen the value.
