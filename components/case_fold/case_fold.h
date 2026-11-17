#pragma once

#include <cstdint>
#include <string>
#include <string_view>

// Names are compared without case in Latin, Greek and Cyrillic, and as they are in any other
// script. The dashboard's clients fold with the same table (client/caseFold.ts): a browser knows
// more of Unicode than the device, and the two must never disagree on whether two names are one.
namespace esphome::case_fold {

// What `code_point` folds to: its simple case folding when the table has it, itself otherwise.
uint32_t fold_code_point(uint32_t code_point);

// `text` with every code point folded; bytes that are not well-formed UTF-8 stay as they are.
std::string fold_case(std::string_view text);

}  // namespace esphome::case_fold
