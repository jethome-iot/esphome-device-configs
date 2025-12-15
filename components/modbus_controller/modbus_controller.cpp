#include "modbus_controller.h"
#include "esphome/core/application.h"
#include "esphome/core/log.h"

namespace esphome {
namespace modbus_controller {

static const char *const TAG = "modbus_controller";

void ModbusController::setup() { this->create_register_ranges_(); }

/*
 To work with the existing modbus class and avoid polling for responses a command queue is used.
 send_next_command will submit the command at the top of the queue and set the corresponding callback
 to handle the response from the device.
 Once the response has been processed it is removed from the queue and the next command is sent
*/
bool ModbusController::send_next_command_() {
  uint32_t last_send = millis() - this->last_command_timestamp_;

  if ((last_send > this->command_throttle_) && !waiting_for_response() && !this->command_queue_.empty()) {
    auto &command = this->command_queue_.front();

    // remove from queue if command was sent too often
    if (!command->should_retry(this->max_cmd_retries_)) {
      if (!this->module_offline_) {
        ESP_LOGW(TAG, "Modbus device=%d set offline", this->address_);

        if (this->offline_skip_updates_ > 0) {
          // Update skip_updates_counter to stop flooding channel with timeouts
          for (auto &r : this->register_ranges_) {
            r.skip_updates_counter = this->offline_skip_updates_;
          }
        }

        this->module_offline_ = true;
        this->offline_callback_.call((int) command->function_code, command->register_address);
      }
      ESP_LOGD(TAG, "Modbus command to device=%d register=0x%02X no response received - removed from send queue",
               this->address_, command->register_address);
      this->command_queue_.pop_front();
    } else {
      ESP_LOGV(TAG, "Sending next modbus command to device %d register 0x%02X count %d", this->address_,
               command->register_address, command->register_count);
      command->send();

      this->last_command_timestamp_ = millis();

      this->command_sent_callback_.call((int) command->function_code, command->register_address);

      // remove from queue if no handler is defined
      if (!command->on_data_func) {
        this->command_queue_.pop_front();
      }
    }
  }
  return (!this->command_queue_.empty());
}

// Queue incoming response
void ModbusController::on_modbus_data(const std::vector<uint8_t> &data) {
  auto &current_command = this->command_queue_.front();
  if (current_command != nullptr) {
    if (this->module_offline_) {
      ESP_LOGW(TAG, "Modbus device=%d back online", this->address_);

      if (this->offline_skip_updates_ > 0) {
        // Restore skip_updates_counter to restore commands updates
        for (auto &r : this->register_ranges_) {
          r.skip_updates_counter = 0;
        }
      }
      // Restore module online state
      this->module_offline_ = false;
      this->online_callback_.call((int) current_command->function_code, current_command->register_address);
    }

    // Move the commandItem to the response queue
    current_command->payload = data;
    this->incoming_queue_.push(std::move(current_command));
    ESP_LOGV(TAG, "Modbus response queued");
    this->command_queue_.pop_front();
  }
}

// Dispatch the response to the registered handler
void ModbusController::process_modbus_data_(const ModbusCommandItem *response) {
  ESP_LOGV(TAG, "Process modbus response for address 0x%X size: %zu", response->register_address,
           response->payload.size());
  response->on_data_func(response->register_type, response->register_address, response->payload);
}

void ModbusController::on_modbus_error(uint8_t function_code, uint8_t exception_code) {
  ESP_LOGE(TAG, "Modbus error function code: 0x%X exception: %d ", function_code, exception_code);
  // Remove pending command waiting for a response
  auto &current_command = this->command_queue_.front();
  if (current_command != nullptr) {
    ESP_LOGE(TAG,
             "Modbus error - last command: function code=0x%X  register address = 0x%X  "
             "registers count=%d "
             "payload size=%zu",
             function_code, current_command->register_address, current_command->register_count,
             current_command->payload.size());
    this->command_queue_.pop_front();
  }
}

void ModbusController::on_modbus_read_registers(uint8_t function_code, uint16_t start_address,
                                                uint16_t number_of_registers) {
  ESP_LOGD(TAG,
           "Received read holding/input registers for device 0x%X. FC: 0x%X. Start address: 0x%X. Number of registers: "
           "0x%X.",
           this->address_, function_code, start_address, number_of_registers);

  // Determine which collection to use based on function code and call template directly
  if (function_code == static_cast<uint8_t>(ModbusFunctionCode::READ_INPUT_REGISTERS)) {
    // FC 0x04 - Input Registers (read-only) - pass collection directly
    this->read_registers_(server_input_registers_, start_address, number_of_registers, function_code, "input register");
  } else if (function_code == static_cast<uint8_t>(ModbusFunctionCode::READ_HOLDING_REGISTERS)) {
    // FC 0x03 - Holding Registers (read-write) - pass collection directly
    this->read_registers_(server_holding_registers_, start_address, number_of_registers, function_code,
                          "holding register");
  } else {
    ESP_LOGW(TAG, "Unsupported function code 0x%X for read registers. Sending exception response.", function_code);
    this->send_error(function_code, ModbusExceptionCode::ILLEGAL_FUNCTION);
    return;
  }
}

void ModbusController::on_modbus_write_registers(uint8_t function_code, const std::vector<uint8_t> &data) {
  uint16_t number_of_registers;
  uint16_t payload_offset;

  if (function_code == ModbusFunctionCode::WRITE_MULTIPLE_REGISTERS) {
    number_of_registers = uint16_t(data[3]) | (uint16_t(data[2]) << 8);
    if (number_of_registers == 0 || number_of_registers > modbus::MAX_NUM_OF_REGISTERS_TO_WRITE) {
      ESP_LOGW(TAG, "Invalid number of registers %d. Sending exception response.", number_of_registers);
      this->send_error(function_code, ModbusExceptionCode::ILLEGAL_DATA_VALUE);
      return;
    }
    uint16_t payload_size = data[4];
    if (payload_size != number_of_registers * 2) {
      ESP_LOGW(TAG, "Payload size of %d bytes is not 2 times the number of registers (%d). Sending exception response.",
               payload_size, number_of_registers);
      this->send_error(function_code, ModbusExceptionCode::ILLEGAL_DATA_VALUE);
      return;
    }
    payload_offset = 5;
  } else if (function_code == ModbusFunctionCode::WRITE_SINGLE_REGISTER) {
    number_of_registers = 1;
    payload_offset = 2;
  } else {
    ESP_LOGW(TAG, "Invalid function code 0x%X. Sending exception response.", function_code);
    this->send_error(function_code, ModbusExceptionCode::ILLEGAL_FUNCTION);
    return;
  }

  uint16_t start_address = uint16_t(data[1]) | (uint16_t(data[0]) << 8);
  ESP_LOGD(TAG,
           "Received write holding registers for device 0x%X. FC: 0x%X. Start address: 0x%X. Number of registers: "
           "0x%X.",
           this->address_, function_code, start_address, number_of_registers);

  // Check for address range overflow (start_address + number_of_registers must not exceed address space)
  if (static_cast<uint32_t>(start_address) + number_of_registers > MODBUS_ADDRESS_SPACE_SIZE) {
    ESP_LOGW(TAG,
             "Address range overflow: start_address 0x%04X + number_of_registers %d exceeds maximum address space. "
             "Sending exception response.",
             start_address, number_of_registers);
    this->send_error(function_code, ModbusExceptionCode::ILLEGAL_DATA_ADDRESS);
    return;
  }

  auto for_each_register = [this, start_address, number_of_registers, payload_offset](
                               const std::function<bool(ServerHoldingRegister *, uint16_t offset)> &callback) -> bool {
    uint16_t offset = payload_offset;
    uint16_t end_address = start_address + number_of_registers;  // Safe after overflow check above
    for (uint16_t current_address = start_address; current_address < end_address;) {
      bool ok = false;
      for (auto *server_holding_register : this->server_holding_registers_) {
        if (server_holding_register->address == current_address) {
          ok = callback(server_holding_register, offset);
          current_address += server_holding_register->register_count;
          offset += server_holding_register->register_count * sizeof(uint16_t);
          break;
        }
      }

      if (!ok) {
        return false;
      }
    }
    return true;
  };

  // check all registers are writable before writing to any of them:
  if (!for_each_register([](ServerHoldingRegister *server_holding_register, uint16_t offset) -> bool {
        return server_holding_register->write_lambda != nullptr;
      })) {
    this->send_error(function_code, ModbusExceptionCode::ILLEGAL_FUNCTION);
    return;
  }

  // Actually write to the registers:
  if (!for_each_register([&data](ServerHoldingRegister *server_holding_register, uint16_t offset) {
        int64_t number = payload_to_number(data, server_holding_register->value_type, offset, 0xFFFFFFFF);
        return server_holding_register->write_lambda(number);
      })) {
    this->send_error(function_code, ModbusExceptionCode::SERVICE_DEVICE_FAILURE);
    return;
  }

  std::vector<uint8_t> response;
  response.reserve(6);
  response.push_back(this->address_);
  response.push_back(function_code);
  response.insert(response.end(), data.begin(), data.begin() + 4);
  this->send_raw(response);
}

void ModbusController::on_modbus_read_coils(uint16_t start_address, uint16_t number_of_coils) {
  ESP_LOGD(TAG, "Received read coils for device 0x%X. Start address: 0x%X. Number of coils: 0x%X.", this->address_,
           start_address, number_of_coils);

  // Pass collection directly to template function - zero overhead
  this->read_boolean_items_(server_coils_, start_address, number_of_coils,
                            static_cast<uint8_t>(ModbusFunctionCode::READ_COILS), "coil");
}

void ModbusController::on_modbus_read_discrete_inputs(uint16_t start_address, uint16_t number_of_inputs) {
  ESP_LOGD(TAG, "Received read discrete inputs for device 0x%X. Start address: 0x%X. Number of inputs: 0x%X.",
           this->address_, start_address, number_of_inputs);

  // Pass collection directly to template function - zero overhead
  this->read_boolean_items_(server_discrete_inputs_, start_address, number_of_inputs,
                            static_cast<uint8_t>(ModbusFunctionCode::READ_DISCRETE_INPUTS), "discrete input");
}

void ModbusController::on_modbus_write_coils(uint8_t function_code, const std::vector<uint8_t> &data) {
  uint16_t number_of_coils;
  uint16_t payload_offset;

  if (function_code == ModbusFunctionCode::WRITE_MULTIPLE_COILS) {
    number_of_coils = uint16_t(data[3]) | (uint16_t(data[2]) << 8);
    if (number_of_coils == 0 || number_of_coils > 1968) {  // Modbus spec limit for write multiple coils
      ESP_LOGW(TAG, "Invalid number of coils %d. Sending exception response.", number_of_coils);
      this->send_error(function_code, ModbusExceptionCode::ILLEGAL_DATA_VALUE);
      return;
    }
    uint16_t payload_size = data[4];
    uint16_t expected_byte_count = (number_of_coils + 7) / 8;
    if (payload_size != expected_byte_count) {
      ESP_LOGW(TAG, "Payload size of %d bytes doesn't match expected %d bytes. Sending exception response.",
               payload_size, expected_byte_count);
      this->send_error(function_code, ModbusExceptionCode::ILLEGAL_DATA_VALUE);
      return;
    }
    payload_offset = 5;
  } else if (function_code == ModbusFunctionCode::WRITE_SINGLE_COIL) {
    number_of_coils = 1;
    payload_offset = 2;
  } else {
    ESP_LOGW(TAG, "Invalid function code 0x%X. Sending exception response.", function_code);
    this->send_error(function_code, ModbusExceptionCode::ILLEGAL_FUNCTION);
    return;
  }

  uint16_t start_address = uint16_t(data[1]) | (uint16_t(data[0]) << 8);
  ESP_LOGD(TAG, "Received write coils for device 0x%X. FC: 0x%X. Start address: 0x%X. Number of coils: 0x%X.",
           this->address_, function_code, start_address, number_of_coils);

  // Check for address range overflow (start_address + number_of_coils must not exceed address space)
  if (static_cast<uint32_t>(start_address) + number_of_coils > MODBUS_ADDRESS_SPACE_SIZE) {
    ESP_LOGW(TAG,
             "Address range overflow: start_address 0x%04X + number_of_coils %d exceeds maximum address space. "
             "Sending exception response.",
             start_address, number_of_coils);
    this->send_error(function_code, ModbusExceptionCode::ILLEGAL_DATA_ADDRESS);
    return;
  }

  // Check all coils are writable before writing to any of them
  uint16_t end_address = start_address + number_of_coils;  // Safe after overflow check above
  for (uint16_t current_address = start_address; current_address < end_address; current_address++) {
    bool found = false;
    for (auto *server_coil : this->server_coils_) {
      if (server_coil->address == current_address) {
        if (!server_coil->write_lambda) {
          ESP_LOGW(TAG, "Coil at address 0x%02X is not writable. Sending exception response.", current_address);
          this->send_error(function_code, ModbusExceptionCode::ILLEGAL_FUNCTION);
          return;
        }
        found = true;
        break;
      }
    }
    if (!found) {
      ESP_LOGW(TAG, "Could not match any coil to address 0x%02X. Sending exception response.", current_address);
      this->send_error(function_code, ModbusExceptionCode::ILLEGAL_DATA_ADDRESS);
      return;
    }
  }

  // Actually write to the coils
  for (uint16_t current_address = start_address; current_address < end_address; current_address++) {
    uint16_t coil_index = current_address - start_address;
    bool value;

    if (function_code == ModbusFunctionCode::WRITE_SINGLE_COIL) {
      // For WRITE_SINGLE_COIL, value is 0xFF00 for ON, 0x0000 for OFF
      uint16_t coil_value = uint16_t(data[3]) | (uint16_t(data[2]) << 8);
      value = (coil_value == 0xFF00);
    } else {
      // For WRITE_MULTIPLE_COILS, unpack bits from bytes
      size_t byte_index = payload_offset + (coil_index / 8);
      size_t bit_index = coil_index % 8;
      value = (data[byte_index] & (1 << bit_index)) != 0;
    }

    for (auto *server_coil : this->server_coils_) {
      if (server_coil->address == current_address) {
        if (!server_coil->write_lambda(value)) {
          ESP_LOGW(TAG, "Failed to write coil at address 0x%02X. Sending exception response.", current_address);
          this->send_error(function_code, ModbusExceptionCode::SERVICE_DEVICE_FAILURE);
          return;
        }
        ESP_LOGV(TAG, "Wrote coil at address 0x%02X. Value: %s.", current_address, ONOFF(value));
        break;
      }
    }
  }

  std::vector<uint8_t> response;
  response.reserve(6);
  response.push_back(this->address_);
  response.push_back(function_code);
  response.insert(response.end(), data.begin(), data.begin() + 4);
  this->send_raw(response);
}

SensorSet ModbusController::find_sensors_(ModbusRegisterType register_type, uint16_t start_address) const {
  auto reg_it = std::find_if(
      std::begin(this->register_ranges_), std::end(this->register_ranges_),
      [=](RegisterRange const &r) { return (r.start_address == start_address && r.register_type == register_type); });

  if (reg_it == this->register_ranges_.end()) {
    ESP_LOGE(TAG, "No matching range for sensor found - start_address : 0x%X", start_address);
  } else {
    return reg_it->sensors;
  }

  // not found
  return {};
}
void ModbusController::on_register_data(ModbusRegisterType register_type, uint16_t start_address,
                                        const std::vector<uint8_t> &data) {
  ESP_LOGV(TAG, "data for register address : 0x%X : ", start_address);

  // loop through all sensors with the same start address
  auto sensors = find_sensors_(register_type, start_address);
  for (auto *sensor : sensors) {
    sensor->parse_and_publish(data);
  }
}

void ModbusController::queue_command(const ModbusCommandItem &command) {
  if (!this->allow_duplicate_commands_) {
    // check if this command is already qeued.
    // not very effective but the queue is never really large
    for (auto &item : this->command_queue_) {
      if (item->is_equal(command)) {
        ESP_LOGW(TAG, "Duplicate modbus command found: type=0x%x address=%u count=%u",
                 static_cast<uint8_t>(command.register_type), command.register_address, command.register_count);
        // update the payload of the queued command
        // replaces a previous command
        item->payload = command.payload;
        return;
      }
    }
  }
  this->command_queue_.push_back(make_unique<ModbusCommandItem>(command));
}

void ModbusController::update_range_(RegisterRange &r) {
  ESP_LOGV(TAG, "Range : %X Size: %x (%d) skip: %d", r.start_address, r.register_count, (int) r.register_type,
           r.skip_updates_counter);
  if (r.skip_updates_counter == 0) {
    // if a custom command is used the user supplied custom_data is only available in the SensorItem.
    if (r.register_type == ModbusRegisterType::CUSTOM) {
      auto sensors = this->find_sensors_(r.register_type, r.start_address);
      if (!sensors.empty()) {
        auto sensor = sensors.cbegin();
        auto command_item = ModbusCommandItem::create_custom_command(
            this, (*sensor)->custom_data,
            [this](ModbusRegisterType register_type, uint16_t start_address, const std::vector<uint8_t> &data) {
              this->on_register_data(ModbusRegisterType::CUSTOM, start_address, data);
            });
        command_item.register_address = (*sensor)->start_address;
        command_item.register_count = (*sensor)->register_count;
        command_item.function_code = ModbusFunctionCode::CUSTOM;
        queue_command(command_item);
      }
    } else {
      queue_command(ModbusCommandItem::create_read_command(this, r.register_type, r.start_address, r.register_count));
    }
    r.skip_updates_counter = r.skip_updates;  // reset counter to config value
  } else {
    r.skip_updates_counter--;
  }
}
//
// Queue the modbus requests to be send.
// Once we get a response to the command it is removed from the queue and the next command is send
//
void ModbusController::update() {
  if (!this->command_queue_.empty()) {
    ESP_LOGV(TAG, "%zu modbus commands already in queue", this->command_queue_.size());
  } else {
    ESP_LOGV(TAG, "Updating modbus component");
  }

  for (auto &r : this->register_ranges_) {
    ESP_LOGVV(TAG, "Updating range 0x%X", r.start_address);
    update_range_(r);
  }
}

// walk through the sensors and determine the register ranges to read
size_t ModbusController::create_register_ranges_() {
  this->register_ranges_.clear();
  if (this->parent_->role == modbus::ModbusRole::CLIENT && this->sensorset_.empty()) {
    ESP_LOGW(TAG, "No sensors registered");
    return 0;
  }

  // iterator is sorted see SensorItemsComparator for details
  auto ix = this->sensorset_.begin();
  RegisterRange r = {};
  uint8_t buffer_offset = 0;
  SensorItem *prev = nullptr;
  while (ix != this->sensorset_.end()) {
    SensorItem *curr = *ix;

    ESP_LOGV(TAG, "Register: 0x%X %d %d %d offset=%u skip=%u addr=%p", curr->start_address, curr->register_count,
             curr->offset, curr->get_register_size(), curr->offset, curr->skip_updates, curr);

    if (r.register_count == 0) {
      // this is the first register in range
      r.start_address = curr->start_address;
      r.register_count = curr->register_count;
      r.register_type = curr->register_type;
      r.sensors.insert(curr);
      r.skip_updates = curr->skip_updates;
      r.skip_updates_counter = 0;
      buffer_offset = curr->get_register_size();

      ESP_LOGV(TAG, "Started new range");
    } else {
      // this is not the first register in range so it might be possible
      // to reuse the last register or extend the current range
      if (!curr->force_new_range && r.register_type == curr->register_type &&
          curr->register_type != ModbusRegisterType::CUSTOM) {
        if (curr->start_address == (r.start_address + r.register_count - prev->register_count) &&
            curr->register_count == prev->register_count && curr->get_register_size() == prev->get_register_size()) {
          // this register can re-use the data from the previous register

          // remove this sensore because start_address is changed (sort-order)
          ix = this->sensorset_.erase(ix);

          curr->start_address = r.start_address;
          curr->offset += prev->offset;

          this->sensorset_.insert(curr);
          // move iterator backwards because it will be incremented later
          ix--;

          ESP_LOGV(TAG, "Re-use previous register - change to register: 0x%X %d offset=%u", curr->start_address,
                   curr->register_count, curr->offset);
        } else if (curr->start_address == (r.start_address + r.register_count)) {
          // this register can extend the current range

          // remove this sensore because start_address is changed (sort-order)
          ix = this->sensorset_.erase(ix);

          curr->start_address = r.start_address;
          curr->offset += buffer_offset;
          buffer_offset += curr->get_register_size();
          r.register_count += curr->register_count;

          this->sensorset_.insert(curr);
          // move iterator backwards because it will be incremented later
          ix--;

          ESP_LOGV(TAG, "Extend range - change to register: 0x%X %d offset=%u", curr->start_address,
                   curr->register_count, curr->offset);
        }
      }
    }

    if (curr->start_address == r.start_address && curr->register_type == r.register_type) {
      // use the lowest non zero value for the whole range
      // Because zero is the default value for skip_updates it is excluded from getting the min value.
      if (curr->skip_updates != 0) {
        if (r.skip_updates != 0) {
          r.skip_updates = std::min(r.skip_updates, curr->skip_updates);
        } else {
          r.skip_updates = curr->skip_updates;
        }
      }

      // add sensor to this range
      r.sensors.insert(curr);

      ix++;
    } else {
      ESP_LOGV(TAG, "Add range 0x%X %d skip:%d", r.start_address, r.register_count, r.skip_updates);
      this->register_ranges_.push_back(r);
      r = {};
      buffer_offset = 0;
      // do not increment the iterator here because the current sensor has to be re-evaluated
    }

    prev = curr;
  }

  if (r.register_count > 0) {
    // Add the last range
    ESP_LOGV(TAG, "Add last range 0x%X %d skip:%d", r.start_address, r.register_count, r.skip_updates);
    this->register_ranges_.push_back(r);
  }

  return this->register_ranges_.size();
}

void ModbusController::dump_config() {
  ESP_LOGCONFIG(TAG,
                "ModbusController:\n"
                "  Address: 0x%02X\n"
                "  Max Command Retries: %d\n"
                "  Offline Skip Updates: %d\n"
                "  Server Courtesy Response:\n"
                "    Enabled: %s\n"
                "    Register Last Address: 0x%02X\n"
                "    Register Value: %d\n"
                "    Coil Last Address: 0x%02X\n"
                "    Coil Value: %s\n"
                "    Discrete Input Last Address: 0x%02X\n"
                "    Discrete Input Value: %s",
                this->address_, this->max_cmd_retries_, this->offline_skip_updates_,
                this->server_courtesy_response_.enabled ? "true" : "false",
                this->server_courtesy_response_.register_last_address, this->server_courtesy_response_.register_value,
                this->server_courtesy_response_.coil_last_address,
                this->server_courtesy_response_.coil_value ? "true" : "false",
                this->server_courtesy_response_.discrete_input_last_address,
                this->server_courtesy_response_.discrete_input_value ? "true" : "false");

#if ESPHOME_LOG_LEVEL >= ESPHOME_LOG_LEVEL_VERBOSE
  ESP_LOGCONFIG(TAG, "sensormap");
  for (auto &it : this->sensorset_) {
    ESP_LOGCONFIG(TAG, " Sensor type=%zu start=0x%X offset=0x%X count=%d size=%d",
                  static_cast<uint8_t>(it->register_type), it->start_address, it->offset, it->register_count,
                  it->get_register_size());
  }
  ESP_LOGCONFIG(TAG, "ranges");
  for (auto &it : this->register_ranges_) {
    ESP_LOGCONFIG(TAG, "  Range type=%zu start=0x%X count=%d skip_updates=%d", static_cast<uint8_t>(it.register_type),
                  it.start_address, it.register_count, it.skip_updates);
  }
  ESP_LOGCONFIG(TAG, "server input registers");
  for (auto &r : this->server_input_registers_) {
    ESP_LOGCONFIG(TAG, "  Address=0x%02X value_type=%zu register_count=%u", r->address,
                  static_cast<uint8_t>(r->value_type), r->register_count);
  }
  ESP_LOGCONFIG(TAG, "server holding registers");
  for (auto &r : this->server_holding_registers_) {
    ESP_LOGCONFIG(TAG, "  Address=0x%02X value_type=%zu register_count=%u", r->address,
                  static_cast<uint8_t>(r->value_type), r->register_count);
  }
#endif
}

void ModbusController::loop() {
  // Incoming data to process?
  if (!this->incoming_queue_.empty()) {
    auto &message = this->incoming_queue_.front();
    if (message != nullptr)
      this->process_modbus_data_(message.get());
    this->incoming_queue_.pop();

  } else {
    // all messages processed send pending commands
    this->send_next_command_();
  }
}

void ModbusController::on_write_register_response(ModbusRegisterType register_type, uint16_t start_address,
                                                  const std::vector<uint8_t> &data) {
  ESP_LOGV(TAG, "Command ACK 0x%X %d ", get_data<uint16_t>(data, 0), get_data<int16_t>(data, 1));
}

void ModbusController::dump_sensors_() {
  ESP_LOGV(TAG, "sensors");
  for (auto &it : this->sensorset_) {
    ESP_LOGV(TAG, "  Sensor start=0x%X count=%d size=%d offset=%d", it->start_address, it->register_count,
             it->get_register_size(), it->offset);
  }
}

ModbusCommandItem ModbusCommandItem::create_read_command(
    ModbusController *modbusdevice, ModbusRegisterType register_type, uint16_t start_address, uint16_t register_count,
    std::function<void(ModbusRegisterType register_type, uint16_t start_address, const std::vector<uint8_t> &data)>
        &&handler) {
  ModbusCommandItem cmd;
  cmd.modbusdevice = modbusdevice;
  cmd.register_type = register_type;
  cmd.function_code = modbus_register_read_function(register_type);
  cmd.register_address = start_address;
  cmd.register_count = register_count;
  cmd.on_data_func = std::move(handler);
  return cmd;
}

ModbusCommandItem ModbusCommandItem::create_read_command(ModbusController *modbusdevice,
                                                         ModbusRegisterType register_type, uint16_t start_address,
                                                         uint16_t register_count) {
  ModbusCommandItem cmd;
  cmd.modbusdevice = modbusdevice;
  cmd.register_type = register_type;
  cmd.function_code = modbus_register_read_function(register_type);
  cmd.register_address = start_address;
  cmd.register_count = register_count;
  cmd.on_data_func = [modbusdevice](ModbusRegisterType register_type, uint16_t start_address,
                                    const std::vector<uint8_t> &data) {
    modbusdevice->on_register_data(register_type, start_address, data);
  };
  return cmd;
}

ModbusCommandItem ModbusCommandItem::create_write_multiple_command(ModbusController *modbusdevice,
                                                                   uint16_t start_address, uint16_t register_count,
                                                                   const std::vector<uint16_t> &values) {
  ModbusCommandItem cmd;
  cmd.modbusdevice = modbusdevice;
  cmd.register_type = ModbusRegisterType::HOLDING;
  cmd.function_code = ModbusFunctionCode::WRITE_MULTIPLE_REGISTERS;
  cmd.register_address = start_address;
  cmd.register_count = register_count;
  cmd.on_data_func = [modbusdevice, cmd](ModbusRegisterType register_type, uint16_t start_address,
                                         const std::vector<uint8_t> &data) {
    modbusdevice->on_write_register_response(cmd.register_type, start_address, data);
  };
  for (auto v : values) {
    auto decoded_value = decode_value(v);
    cmd.payload.push_back(decoded_value[0]);
    cmd.payload.push_back(decoded_value[1]);
  }
  return cmd;
}

ModbusCommandItem ModbusCommandItem::create_write_single_coil(ModbusController *modbusdevice, uint16_t address,
                                                              bool value) {
  ModbusCommandItem cmd;
  cmd.modbusdevice = modbusdevice;
  cmd.register_type = ModbusRegisterType::COIL;
  cmd.function_code = ModbusFunctionCode::WRITE_SINGLE_COIL;
  cmd.register_address = address;
  cmd.register_count = 1;
  cmd.on_data_func = [modbusdevice, cmd](ModbusRegisterType register_type, uint16_t start_address,
                                         const std::vector<uint8_t> &data) {
    modbusdevice->on_write_register_response(cmd.register_type, start_address, data);
  };
  cmd.payload.push_back(value ? 0xFF : 0);
  cmd.payload.push_back(0);
  return cmd;
}

ModbusCommandItem ModbusCommandItem::create_write_multiple_coils(ModbusController *modbusdevice, uint16_t start_address,
                                                                 const std::vector<bool> &values) {
  ModbusCommandItem cmd;
  cmd.modbusdevice = modbusdevice;
  cmd.register_type = ModbusRegisterType::COIL;
  cmd.function_code = ModbusFunctionCode::WRITE_MULTIPLE_COILS;
  cmd.register_address = start_address;
  cmd.register_count = values.size();
  cmd.on_data_func = [modbusdevice, cmd](ModbusRegisterType register_type, uint16_t start_address,
                                         const std::vector<uint8_t> &data) {
    modbusdevice->on_write_register_response(cmd.register_type, start_address, data);
  };

  uint8_t bitmask = 0;
  int bitcounter = 0;
  for (auto coil : values) {
    if (coil) {
      bitmask |= (1 << bitcounter);
    }
    bitcounter++;
    if (bitcounter % 8 == 0) {
      cmd.payload.push_back(bitmask);
      bitmask = 0;
    }
  }
  // add remaining bits
  if (bitcounter % 8) {
    cmd.payload.push_back(bitmask);
  }
  return cmd;
}

ModbusCommandItem ModbusCommandItem::create_write_single_command(ModbusController *modbusdevice, uint16_t start_address,
                                                                 uint16_t value) {
  ModbusCommandItem cmd;
  cmd.modbusdevice = modbusdevice;
  cmd.register_type = ModbusRegisterType::HOLDING;
  cmd.function_code = ModbusFunctionCode::WRITE_SINGLE_REGISTER;
  cmd.register_address = start_address;
  cmd.register_count = 1;  // not used here anyways
  cmd.on_data_func = [modbusdevice, cmd](ModbusRegisterType register_type, uint16_t start_address,
                                         const std::vector<uint8_t> &data) {
    modbusdevice->on_write_register_response(cmd.register_type, start_address, data);
  };

  auto decoded_value = decode_value(value);
  cmd.payload.push_back(decoded_value[0]);
  cmd.payload.push_back(decoded_value[1]);
  return cmd;
}

ModbusCommandItem ModbusCommandItem::create_custom_command(
    ModbusController *modbusdevice, const std::vector<uint8_t> &values,
    std::function<void(ModbusRegisterType register_type, uint16_t start_address, const std::vector<uint8_t> &data)>
        &&handler) {
  ModbusCommandItem cmd;
  cmd.modbusdevice = modbusdevice;
  cmd.function_code = ModbusFunctionCode::CUSTOM;
  if (handler == nullptr) {
    cmd.on_data_func = [](ModbusRegisterType register_type, uint16_t start_address, const std::vector<uint8_t> &data) {
      ESP_LOGI(TAG, "Custom Command sent");
    };
  } else {
    cmd.on_data_func = handler;
  }
  cmd.payload = values;

  return cmd;
}

ModbusCommandItem ModbusCommandItem::create_custom_command(
    ModbusController *modbusdevice, const std::vector<uint16_t> &values,
    std::function<void(ModbusRegisterType register_type, uint16_t start_address, const std::vector<uint8_t> &data)>
        &&handler) {
  ModbusCommandItem cmd = {};
  cmd.modbusdevice = modbusdevice;
  cmd.function_code = ModbusFunctionCode::CUSTOM;
  if (handler == nullptr) {
    cmd.on_data_func = [](ModbusRegisterType register_type, uint16_t start_address, const std::vector<uint8_t> &data) {
      ESP_LOGI(TAG, "Custom Command sent");
    };
  } else {
    cmd.on_data_func = handler;
  }
  for (auto v : values) {
    cmd.payload.push_back((v >> 8) & 0xFF);
    cmd.payload.push_back(v & 0xFF);
  }

  return cmd;
}

bool ModbusCommandItem::send() {
  if (this->function_code != ModbusFunctionCode::CUSTOM) {
    modbusdevice->send(uint8_t(this->function_code), this->register_address, this->register_count, this->payload.size(),
                       this->payload.empty() ? nullptr : &this->payload[0]);
  } else {
    modbusdevice->send_raw(this->payload);
  }
  this->send_count_++;
  ESP_LOGV(TAG, "Command sent %d 0x%X %d send_count: %d", uint8_t(this->function_code), this->register_address,
           this->register_count, this->send_count_);
  return true;
}

bool ModbusCommandItem::is_equal(const ModbusCommandItem &other) {
  // for custom commands we have to check for identical payloads, since
  // address/count/type fields will be set to zero
  return this->function_code == ModbusFunctionCode::CUSTOM
             ? this->payload == other.payload
             : other.register_address == this->register_address && other.register_count == this->register_count &&
                   other.register_type == this->register_type && other.function_code == this->function_code;
}

void number_to_payload(std::vector<uint16_t> &data, int64_t value, SensorValueType value_type) {
  switch (value_type) {
    case SensorValueType::U_WORD:
    case SensorValueType::S_WORD:
      data.push_back(value & 0xFFFF);
      break;
    case SensorValueType::U_DWORD:
    case SensorValueType::S_DWORD:
    case SensorValueType::FP32:
      data.push_back((value & 0xFFFF0000) >> 16);
      data.push_back(value & 0xFFFF);
      break;
    case SensorValueType::U_DWORD_R:
    case SensorValueType::S_DWORD_R:
    case SensorValueType::FP32_R:
      data.push_back(value & 0xFFFF);
      data.push_back((value & 0xFFFF0000) >> 16);
      break;
    case SensorValueType::U_QWORD:
    case SensorValueType::S_QWORD:
      data.push_back((value & 0xFFFF000000000000) >> 48);
      data.push_back((value & 0xFFFF00000000) >> 32);
      data.push_back((value & 0xFFFF0000) >> 16);
      data.push_back(value & 0xFFFF);
      break;
    case SensorValueType::U_QWORD_R:
    case SensorValueType::S_QWORD_R:
      data.push_back(value & 0xFFFF);
      data.push_back((value & 0xFFFF0000) >> 16);
      data.push_back((value & 0xFFFF00000000) >> 32);
      data.push_back((value & 0xFFFF000000000000) >> 48);
      break;
    default:
      ESP_LOGE(TAG, "Invalid data type for modbus number to payload conversation: %d",
               static_cast<uint16_t>(value_type));
      break;
  }
}

int64_t payload_to_number(const std::vector<uint8_t> &data, SensorValueType sensor_value_type, uint8_t offset,
                          uint32_t bitmask) {
  int64_t value = 0;  // int64_t because it can hold signed and unsigned 32 bits

  size_t size = data.size() - offset;
  bool error = false;
  switch (sensor_value_type) {
    case SensorValueType::U_WORD:
      if (size >= 2) {
        value = mask_and_shift_by_rightbit(get_data<uint16_t>(data, offset), bitmask);  // default is 0xFFFF ;
      } else {
        error = true;
      }
      break;
    case SensorValueType::U_DWORD:
    case SensorValueType::FP32:
      if (size >= 4) {
        value = get_data<uint32_t>(data, offset);
        value = mask_and_shift_by_rightbit((uint32_t) value, bitmask);
      } else {
        error = true;
      }
      break;
    case SensorValueType::U_DWORD_R:
    case SensorValueType::FP32_R:
      if (size >= 4) {
        value = get_data<uint32_t>(data, offset);
        value = static_cast<uint32_t>(value & 0xFFFF) << 16 | (value & 0xFFFF0000) >> 16;
        value = mask_and_shift_by_rightbit((uint32_t) value, bitmask);
      } else {
        error = true;
      }
      break;
    case SensorValueType::S_WORD:
      if (size >= 2) {
        value = mask_and_shift_by_rightbit(get_data<int16_t>(data, offset),
                                           bitmask);  // default is 0xFFFF ;
      } else {
        error = true;
      }
      break;
    case SensorValueType::S_DWORD:
      if (size >= 4) {
        value = mask_and_shift_by_rightbit(get_data<int32_t>(data, offset), bitmask);
      } else {
        error = true;
      }
      break;
    case SensorValueType::S_DWORD_R: {
      if (size >= 4) {
        value = get_data<uint32_t>(data, offset);
        // Currently the high word is at the low position
        // the sign bit is therefore at low before the switch
        uint32_t sign_bit = (value & 0x8000) << 16;
        value = mask_and_shift_by_rightbit(
            static_cast<int32_t>(((value & 0x7FFF) << 16 | (value & 0xFFFF0000) >> 16) | sign_bit), bitmask);
      } else {
        error = true;
      }
    } break;
    case SensorValueType::U_QWORD:
    case SensorValueType::S_QWORD:
      // Ignore bitmask for QWORD
      if (size >= 8) {
        value = get_data<uint64_t>(data, offset);
      } else {
        error = true;
      }
      break;
    case SensorValueType::U_QWORD_R:
    case SensorValueType::S_QWORD_R: {
      // Ignore bitmask for QWORD
      if (size >= 8) {
        uint64_t tmp = get_data<uint64_t>(data, offset);
        value = (tmp << 48) | (tmp >> 48) | ((tmp & 0xFFFF0000) << 16) | ((tmp >> 16) & 0xFFFF0000);
      } else {
        error = true;
      }
    } break;
    case SensorValueType::RAW:
    default:
      break;
  }
  if (error)
    ESP_LOGE(TAG, "not enough data for value");
  return value;
}

void ModbusController::add_on_command_sent_callback(std::function<void(int, int)> &&callback) {
  this->command_sent_callback_.add(std::move(callback));
}

void ModbusController::add_on_online_callback(std::function<void(int, int)> &&callback) {
  this->online_callback_.add(std::move(callback));
}

void ModbusController::add_on_offline_callback(std::function<void(int, int)> &&callback) {
  this->offline_callback_.add(std::move(callback));
}

// Template implementations for generic read functions

template<typename Container>
bool ModbusController::read_boolean_items_(const Container &items, uint16_t start_address, uint16_t item_count,
                                           uint8_t function_code, const char *item_type_name) {
  if (item_count == 0 || item_count > MAX_BOOLEAN_ITEMS) {
    ESP_LOGW(TAG, "Invalid number of %s %d. Sending exception response.", item_type_name, item_count);
    this->send_error(function_code, ModbusExceptionCode::ILLEGAL_DATA_ADDRESS);
    return false;
  }

  // Check for address range overflow (start_address + item_count must not exceed address space)
  if (static_cast<uint32_t>(start_address) + item_count > MODBUS_ADDRESS_SPACE_SIZE) {
    ESP_LOGW(TAG,
             "Address range overflow: start_address 0x%04X + item_count %d exceeds maximum address space. "
             "Sending exception response.",
             start_address, item_count);
    this->send_error(function_code, ModbusExceptionCode::ILLEGAL_DATA_ADDRESS);
    return false;
  }

  // Determine courtesy response parameters based on function code
  bool is_coil = (function_code == static_cast<uint8_t>(ModbusFunctionCode::READ_COILS));
  uint16_t courtesy_last_address = is_coil ? this->server_courtesy_response_.coil_last_address
                                           : this->server_courtesy_response_.discrete_input_last_address;
  bool courtesy_value =
      is_coil ? this->server_courtesy_response_.coil_value : this->server_courtesy_response_.discrete_input_value;

  std::bitset<MAX_BOOLEAN_ITEMS> states;

  for (uint16_t i = 0; i < item_count; i++) {
    uint16_t current_address = start_address + i;
    bool found = false;

    for (auto *item : items) {
      if (item->address == current_address) {
        if (!item->read_lambda) {
          ESP_LOGW(TAG, "%s at address 0x%02X has no read lambda. Sending exception response.", item_type_name,
                   current_address);
          this->send_error(function_code, ModbusExceptionCode::ILLEGAL_DATA_ADDRESS);
          return false;
        }
        bool value = item->read_lambda();
        ESP_LOGV(TAG, "Matched %s. Address: 0x%02X. Value: %s.", item_type_name, item->address, ONOFF(value));
        states[i] = value;
        found = true;
        break;
      }
    }

    if (!found) {
      if (this->server_courtesy_response_.enabled && (current_address <= courtesy_last_address)) {
        ESP_LOGV(TAG, "Could not match any %s to address 0x%02X, but default allowed. Returning default value: %s.",
                 item_type_name, current_address, ONOFF(courtesy_value));
        states[i] = courtesy_value;
      } else {
        ESP_LOGW(TAG, "Could not match any %s to address 0x%02X and default not allowed. Sending exception response.",
                 item_type_name, current_address);
        this->send_error(function_code, ModbusExceptionCode::ILLEGAL_DATA_ADDRESS);
        return false;
      }
    }
  }

  std::vector<uint8_t> response_bytes;
  uint8_t byte_count = (item_count + 7) / 8;
  response_bytes.reserve(byte_count);

  for (size_t i = 0; i < byte_count; i++) {
    uint8_t byte_value = 0;
    for (size_t bit = 0; bit < 8 && (i * 8 + bit) < item_count; bit++) {
      if (states[i * 8 + bit]) {
        byte_value |= (1 << bit);
      }
    }
    response_bytes.push_back(byte_value);
  }

  this->send(function_code, start_address, item_count, response_bytes.size(), response_bytes.data());
  return true;
}

template<typename Container>
bool ModbusController::read_registers_(const Container &items, uint16_t start_address, uint16_t register_count,
                                       uint8_t function_code, const char *register_type_name) {
  if (register_count == 0 || register_count > modbus::MAX_NUM_OF_REGISTERS_TO_READ) {
    ESP_LOGW(TAG, "Invalid number of %s %d. Sending exception response.", register_type_name, register_count);
    this->send_error(function_code, ModbusExceptionCode::ILLEGAL_DATA_ADDRESS);
    return false;
  }

  // Check for address range overflow (start_address + register_count must not exceed address space)
  if (static_cast<uint32_t>(start_address) + register_count > MODBUS_ADDRESS_SPACE_SIZE) {
    ESP_LOGW(TAG,
             "Address range overflow: start_address 0x%04X + register_count %d exceeds maximum address space. "
             "Sending exception response.",
             start_address, register_count);
    this->send_error(function_code, ModbusExceptionCode::ILLEGAL_DATA_ADDRESS);
    return false;
  }

  std::vector<uint16_t> sixteen_bit_response;
  uint16_t end_address = start_address + register_count;  // Safe after overflow check above
  for (uint16_t current_address = start_address; current_address < end_address;) {
    bool found = false;
    for (auto *item : items) {
      if (item->address == current_address) {
        if (!item->read_lambda) {
          break;
        }
        int64_t value = item->read_lambda();
        ESP_LOGV(TAG, "Matched %s. Address: 0x%02X. Value type: %zu. Register count: %u. Value: %s.",
                 register_type_name, item->address, static_cast<size_t>(item->value_type), item->register_count,
                 item->format_value(value).c_str());

        std::vector<uint16_t> payload;
        payload.reserve(item->register_count * 2);
        number_to_payload(payload, value, item->value_type);
        sixteen_bit_response.insert(sixteen_bit_response.end(), payload.cbegin(), payload.cend());
        current_address += item->register_count;
        found = true;
        break;
      }
    }

    if (!found) {
      if (this->server_courtesy_response_.enabled &&
          (current_address <= this->server_courtesy_response_.register_last_address)) {
        ESP_LOGV(TAG,
                 "Could not match any %s to address 0x%02X, but default allowed. "
                 "Returning default value: %d.",
                 register_type_name, current_address, this->server_courtesy_response_.register_value);
        sixteen_bit_response.push_back(this->server_courtesy_response_.register_value);
        current_address += 1;  // Just increment by 1, as the default response is a single register
      } else {
        ESP_LOGW(TAG, "Could not match any %s to address 0x%02X and default not allowed. Sending exception response.",
                 register_type_name, current_address);
        this->send_error(function_code, ModbusExceptionCode::ILLEGAL_DATA_ADDRESS);
        return false;
      }
    }
  }

  std::vector<uint8_t> response;
  for (auto v : sixteen_bit_response) {
    auto decoded_value = decode_value(v);
    response.push_back(decoded_value[0]);
    response.push_back(decoded_value[1]);
  }

  this->send(function_code, start_address, register_count, response.size(), response.data());
  return true;
}

// Explicit template instantiations
template bool ModbusController::read_boolean_items_<std::vector<ServerCoil *>>(const std::vector<ServerCoil *> &items,
                                                                               uint16_t start_address,
                                                                               uint16_t item_count,
                                                                               uint8_t function_code,
                                                                               const char *item_type_name);
template bool ModbusController::read_boolean_items_<std::vector<ServerDiscreteInput *>>(
    const std::vector<ServerDiscreteInput *> &items, uint16_t start_address, uint16_t item_count, uint8_t function_code,
    const char *item_type_name);
template bool ModbusController::read_registers_<std::vector<ServerInputRegister *>>(
    const std::vector<ServerInputRegister *> &items, uint16_t start_address, uint16_t register_count,
    uint8_t function_code, const char *register_type_name);
template bool ModbusController::read_registers_<std::vector<ServerHoldingRegister *>>(
    const std::vector<ServerHoldingRegister *> &items, uint16_t start_address, uint16_t register_count,
    uint8_t function_code, const char *register_type_name);

}  // namespace modbus_controller
}  // namespace esphome
