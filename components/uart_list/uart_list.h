#pragma once

#include <vector>
#include "esphome/components/uart/uart_component.h"

namespace esphome::uart_list {

class UartList {
 public:
  void add_uart(uart::UARTComponent *uart) { this->uarts_.push_back(uart); }
  const std::vector<uart::UARTComponent *> &uarts() const { return this->uarts_; }
  size_t size() const { return this->uarts_.size(); }
  uart::UARTComponent *get(size_t index) const { return index < this->uarts_.size() ? this->uarts_[index] : nullptr; }

 protected:
  std::vector<uart::UARTComponent *> uarts_;
};

}  // namespace esphome::uart_list
