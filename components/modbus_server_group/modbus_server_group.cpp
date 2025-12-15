#include "modbus_server_group.h"
#include "esphome/core/log.h"
#include "esphome/core/application.h"

namespace esphome {
namespace modbus_server_group {

static const char *const TAG = "modbus_server_group";

void ModbusServerGroup::setup() {
  ESP_LOGCONFIG(TAG, "Setting up Modbus Server Group...");

  if (this->modbus_controller_ == nullptr) {
    ESP_LOGE(TAG, "Modbus controller not set!");
    this->mark_failed();
    return;
  }

  // Setup discrete inputs from inputs group
  if (this->inputs_group_ != nullptr) {
    this->setup_discrete_inputs_();
  }

  // Setup coils from outputs group
  if (this->outputs_group_ != nullptr) {
    this->setup_coils_();
  }

  // Setup count registers
  this->setup_count_registers_();

  ESP_LOGCONFIG(TAG, "Modbus Server Group setup complete");
}

void ModbusServerGroup::setup_discrete_inputs_() {
  ESP_LOGD(TAG, "Setting up discrete inputs from group '%s'", this->inputs_group_->get_name().c_str());
  
  uint16_t address = this->inputs_start_address_;
  this->inputs_count_ = 0;

#ifdef USE_BINARY_SENSOR
  for (EntityBase *entity : this->inputs_group_->items()) {
    // Cast to binary sensor (safe because we checked the type)
    auto *binary_sensor = static_cast<binary_sensor::BinarySensor *>(entity);
    
    // Create a discrete input for this binary sensor
    auto *discrete_input = new modbus_controller::ServerDiscreteInput(address);
    
    // Set read lambda to return the binary sensor state
    // Capture binary_sensor by value (pointer copy)
    discrete_input->set_read_lambda([binary_sensor](uint16_t addr) -> bool {
      return binary_sensor->state;
    });
    
    this->modbus_controller_->add_server_discrete_input(discrete_input);
    
    ESP_LOGD(TAG, "  Added discrete input at 0x%04X for '%s'", 
              address, entity->get_name().c_str());
    
    address++;
    this->inputs_count_++;
  }
#endif

  ESP_LOGCONFIG(TAG, "Configured %d discrete input(s) starting at 0x%04X", 
                this->inputs_count_, this->inputs_start_address_);
}

void ModbusServerGroup::setup_coils_() {
  ESP_LOGD(TAG, "Setting up coils from group '%s'", this->outputs_group_->get_name().c_str());
  
  uint16_t address = this->outputs_start_address_;
  this->outputs_count_ = 0;

#ifdef USE_SWITCH
  for (EntityBase *entity : this->outputs_group_->items()) {
    // Cast to switch (safe because we checked the type)
    auto *switch_entity = static_cast<switch_::Switch *>(entity);
    
    // Create a coil for this switch
    auto *coil = new modbus_controller::ServerCoil(address);
    
    // Set read lambda to return the switch state
    // Capture switch_entity by value (pointer copy)
    coil->set_read_lambda([switch_entity](uint16_t addr) -> bool {
      return switch_entity->state;
    });
    
    // Set write lambda to control the switch
    // Capture switch_entity by value (pointer copy)
    coil->set_write_lambda([switch_entity](uint16_t addr, bool value) -> bool {
      if (value) {
        switch_entity->turn_on();
      } else {
        switch_entity->turn_off();
      }
      return true;
    });
    
    this->modbus_controller_->add_server_coil(coil);
    
    ESP_LOGD(TAG, "  Added coil at 0x%04X for '%s'", 
              address, entity->get_name().c_str());
    
    address++;
    this->outputs_count_++;
  }
#endif

  ESP_LOGCONFIG(TAG, "Configured %d coil(s) starting at 0x%04X", 
                this->outputs_count_, this->outputs_start_address_);
}

void ModbusServerGroup::setup_count_registers_() {
  ESP_LOGD(TAG, "Setting up count registers at 0x0200 and 0x0201");
  
  // Register for inputs count at 0x0200
  auto *inputs_count_register = new modbus_controller::ServerInputRegister(
      0x0200, 
      modbus_controller::SensorValueType::U_WORD, 
      1
  );
  
  // Capture inputs_count_ by reference through 'this'
  inputs_count_register->set_read_lambda<uint16_t>(
      [this](uint16_t addr) -> uint16_t {
        return this->inputs_count_;
      }
  );
  
  this->modbus_controller_->add_server_input_register(inputs_count_register);
  ESP_LOGD(TAG, "  Added input register at 0x0200 (inputs_count = %d)", this->inputs_count_);
  
  // Register for outputs count at 0x0201
  auto *outputs_count_register = new modbus_controller::ServerInputRegister(
      0x0201, 
      modbus_controller::SensorValueType::U_WORD, 
      1
  );
  
  // Capture outputs_count_ by reference through 'this'
  outputs_count_register->set_read_lambda<uint16_t>(
      [this](uint16_t addr) -> uint16_t {
        return this->outputs_count_;
      }
  );
  
  this->modbus_controller_->add_server_input_register(outputs_count_register);
  ESP_LOGD(TAG, "  Added input register at 0x0201 (outputs_count = %d)", this->outputs_count_);
}

void ModbusServerGroup::dump_config() {
  ESP_LOGCONFIG(TAG, "Modbus Server Group:");
  
  if (this->is_failed()) {
    ESP_LOGE(TAG, "  Setup failed!");
    return;
  }
  
  if (this->inputs_group_ != nullptr) {
    ESP_LOGCONFIG(TAG, "  Discrete Inputs:");
    ESP_LOGCONFIG(TAG, "    Group: %s", this->inputs_group_->get_name().c_str());
    ESP_LOGCONFIG(TAG, "    Start Address: 0x%04X", this->inputs_start_address_);
    ESP_LOGCONFIG(TAG, "    Count: %d", this->inputs_count_);
  }
  
  if (this->outputs_group_ != nullptr) {
    ESP_LOGCONFIG(TAG, "  Coils:");
    ESP_LOGCONFIG(TAG, "    Group: %s", this->outputs_group_->get_name().c_str());
    ESP_LOGCONFIG(TAG, "    Start Address: 0x%04X", this->outputs_start_address_);
    ESP_LOGCONFIG(TAG, "    Count: %d", this->outputs_count_);
  }
  
  ESP_LOGCONFIG(TAG, "  Count Registers:");
  ESP_LOGCONFIG(TAG, "    0x0200 (inputs_count): %d", this->inputs_count_);
  ESP_LOGCONFIG(TAG, "    0x0201 (outputs_count): %d", this->outputs_count_);
}

}  // namespace modbus_server_group
}  // namespace esphome

