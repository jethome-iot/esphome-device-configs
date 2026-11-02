#pragma once

#include <ArduinoJson.h>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
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

/// `base`, but never more than `budget` bytes held at once: a parse past it ends in NoMemory,
/// where a body that expands in its document would otherwise take whatever the heap has.
/// Each block carries its size in front, which the budget counts too.
class BoundedAllocator : public ArduinoJson::Allocator {
 public:
  BoundedAllocator(ArduinoJson::Allocator *base, size_t budget) : base_(base), budget_(budget) {}

  void *allocate(size_t size) override {
    if (!this->take_(HEADER + size))
      return nullptr;
    auto *block = static_cast<uint8_t *>(this->base_->allocate(HEADER + size));
    if (block == nullptr) {
      this->used_ -= HEADER + size;
      return nullptr;
    }
    memcpy(block, &size, sizeof(size));
    return block + HEADER;
  }
  void deallocate(void *ptr) override {
    if (ptr == nullptr)
      return;
    uint8_t *block = static_cast<uint8_t *>(ptr) - HEADER;
    this->used_ -= HEADER + size_of_(block);
    this->base_->deallocate(block);
  }
  // A refusal leaves the old block as it was, as realloc does.
  void *reallocate(void *ptr, size_t size) override {
    if (ptr == nullptr)
      return this->allocate(size);
    uint8_t *block = static_cast<uint8_t *>(ptr) - HEADER;
    const size_t old = size_of_(block);
    if (size > old && !this->take_(size - old))
      return nullptr;
    auto *moved = static_cast<uint8_t *>(this->base_->reallocate(block, HEADER + size));
    if (moved == nullptr) {
      if (size > old)
        this->used_ -= size - old;
      return nullptr;
    }
    if (size < old)
      this->used_ -= old - size;
    memcpy(moved, &size, sizeof(size));
    return moved + HEADER;
  }

 protected:
  // Keeps whatever alignment `base` gives.
  static constexpr size_t HEADER = alignof(std::max_align_t);
  static_assert(sizeof(size_t) <= HEADER, "the size fits in front of the block");

  static size_t size_of_(const uint8_t *block) {
    size_t size;
    memcpy(&size, block, sizeof(size));
    return size;
  }
  bool take_(size_t more) {
    if (more > this->budget_ - this->used_)
      return false;
    this->used_ += more;
    return true;
  }

  ArduinoJson::Allocator *base_;
  size_t budget_;
  size_t used_{0};
};

}  // namespace esphome::schedules
