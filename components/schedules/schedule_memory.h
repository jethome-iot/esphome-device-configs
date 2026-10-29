#pragma once

#include <ArduinoJson.h>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>
#include "esphome/core/defines.h"
#include "esphome/core/helpers.h"
#ifdef USE_PSRAM
#include "esphome/components/json/json_util.h"
#endif

namespace esphome::schedules {

/// PSRAM first, the internal heap when the board has none or it is full: what a schedule keeps
/// grows with its document, and the internal heap is the one the network stack needs.
/// Stateless, so containers move and compare as with std::allocator.
template<class T> struct PsramAllocator {
  using value_type = T;
  PsramAllocator() = default;
  template<class U> constexpr PsramAllocator(const PsramAllocator<U> & /*other*/) noexcept {}
  T *allocate(size_t n) {
    T *p = RAMAllocator<T>().allocate(n);
    // As operator new without exceptions: the engine's room checks keep a schedule from here.
    if (p == nullptr)
      abort();
    return p;
  }
  void deallocate(T *p, size_t n) { RAMAllocator<T>().deallocate(p, n); }
  template<class U> bool operator==(const PsramAllocator<U> & /*other*/) const { return true; }
};

using PsramString = std::basic_string<char, std::char_traits<char>, PsramAllocator<char>>;
template<class T> using PsramVector = std::vector<T, PsramAllocator<T>>;

/// Found through the allocator's namespace, so a std::string compares with one anywhere.
inline bool operator==(const PsramString &a, const std::string &b) {
  return std::string_view(a) == std::string_view(b);
}

/// What every document of the schedules and their editor draws on: PSRAM first where the
/// firmware has it.
inline ArduinoJson::Allocator *json_allocator() {
#ifdef USE_PSRAM
  static json::SpiRamAllocator allocator;
  return &allocator;
#else
  return ArduinoJson::detail::DefaultAllocator::instance();
#endif
}

}  // namespace esphome::schedules
