# panel_text

Fits text to a row of the display menu, which a monospace font draws one glyph per code point:
what the font cannot draw becomes `?`, and a text too long for the row is cut with `…`. It also
holds the rules a label follows, the one [`entity_config`](../entity_config/README.md#labels) keeps
for a relay or an input and [`dallas_scan`](../dallas_scan/README.md#labels) for a temperature
slot.

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/jethome-iot/esphome-device-configs
      ref: master
      path: components
    components: [panel_text]

panel_text:
```

Nothing to configure. The key makes the functions reachable from lambdas; a component whose own
code calls them auto-loads it instead.

## From lambdas

In `esphome::panel_text`, `has_glyph` being whether the menu font has a code point, such as
`[font](uint32_t cp) { return font->find_glyph(cp) != nullptr; }`:

| Call | Returns |
| --- | --- |
| `panel_safe(text, max_bytes, has_glyph)` | `text` with one `?` for each code point the font lacks, each control character and each piece of malformed UTF-8, so the font draws all of it. Reads the first `max_bytes`; a longer text ends in `…` |
| `panel_glyphs(text)` | how many glyphs `text` takes: its code points |
| `panel_fit(text, glyphs)` | at most `glyphs` code points; a cut text ends in `…`, with no space before it |
| `panel_pair(name, value, width)` | `name: value` in `width` glyphs: whole when it fits, otherwise the name keeps at least six glyphs and each cut part ends in `…` |

`f7x14_mod2`, the menu font, is at most 7 px wide per glyph, so a 128 px row holds 18. `…` is in
`GF_Latin_Kernel`, so a font built from that glyph set draws it.

## Labels

A label is text an owner gives a relay, an input or a temperature slot, shown on the panel and
the dashboard in place of a name. Well-formed UTF-8, no control character (C0, DEL or C1), at most
`LABEL_MAX_LENGTH` (24) characters, counted in code points once the spaces (U+0020 only) at both
ends are trimmed; empty means no label. `parse_label(text, length, out)` puts the trimmed label in
`out` and returns true, or returns false and leaves `out` alone. It reads `length` bytes, so a NUL
inside is a control character, not the end. A label passes `panel_safe()` whole when the font has
its glyphs.

## Tests

`tests/components/panel_text/` covers each function on the host, down to a sweep showing that
nothing `panel_safe()` returns stops the font's decoder, and that it passes a text whole exactly
when the text is well-formed UTF-8 with no control character, and the label rules: the trim, the
length in code points, control characters, malformed UTF-8 and an embedded NUL.
