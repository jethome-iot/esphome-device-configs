#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

// Text for the display menu's rows, which a monospace font draws one glyph per code point.
namespace esphome::panel_text {

// Whether the font has a glyph for a code point.
using HasGlyph = std::function<bool(uint32_t)>;

// What the font can draw whole: a code point it lacks, a control character and each maximal
// ill-formed UTF-8 subpart become one '?'. The font's own decoder stops the row at malformed
// bytes, so nothing it refuses may pass. Reads the first `max_bytes` only; a longer text ends
// in '…' after them.
std::string panel_safe(const std::string &text, size_t max_bytes, const HasGlyph &has_glyph);
// Code points in well-formed UTF-8, such as panel_safe's result.
size_t panel_glyphs(const std::string &text);
// At most `glyphs` code points; a cut text ends in '…', with no space before it.
std::string panel_fit(const std::string &text, size_t glyphs);
// "name: value" in `width` glyphs. Whole when it fits; otherwise the name keeps
// max(6, width - 2 - value) glyphs, never more than it has, the value the rest, and each cut
// part ends in '…'.
std::string panel_pair(const std::string &name, const std::string &value, size_t width);

}  // namespace esphome::panel_text
