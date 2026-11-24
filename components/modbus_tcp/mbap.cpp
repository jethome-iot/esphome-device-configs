#include "mbap.h"

#include "esphome/core/log.h"

#include <algorithm>
#include <cstring>

namespace esphome::modbus_tcp {

using modbus::ExceptionCode;
using modbus::FunctionCode;
using modbus::MutablePackedBits;
using modbus::PackedBits;
using modbus::RegisterValues;
using modbus::ResponseStatus;
using modbus::helpers::get_data;

static const char *const TAG = "modbus_tcp";

// Request data offsets after the function code, as the RTU hub reads them.
static constexpr size_t WRITE_SINGLE_VALUE_OFFSET = 2;
static constexpr size_t WRITE_MULTIPLE_VALUES_OFFSET = 5;
static constexpr size_t READ_WRITE_VALUES_OFFSET = 9;
static constexpr uint16_t MAX_MBAP_LENGTH = 1 + modbus::MAX_PDU_SIZE;

Frame find_frame(std::span<const uint8_t> buffer) {
  if (buffer.size() < MBAP_HEADER_SIZE)
    return {FrameStatus::INCOMPLETE, 0};
  const uint16_t protocol_id = get_data<uint16_t>(buffer.data(), 2);
  const uint16_t length = get_data<uint16_t>(buffer.data(), 4);
  // A length of 1 is a unit id with no function code.
  if (protocol_id != 0 || length < 2 || length > MAX_MBAP_LENGTH)
    return {FrameStatus::INVALID, 0};
  const size_t size = MBAP_HEADER_SIZE - 1 + length;
  if (buffer.size() < size)
    return {FrameStatus::INCOMPLETE, 0};
  return {FrameStatus::COMPLETE, size};
}

namespace {

void put_u16(uint8_t *out, uint16_t value) {
  out[0] = static_cast<uint8_t>(value >> 8);
  out[1] = static_cast<uint8_t>(value);
}

size_t exception(uint8_t function_code, ExceptionCode code, std::span<uint8_t> response) {
  ESP_LOGD(TAG, "Exception %u replied to function 0x%02X", static_cast<uint8_t>(code), function_code);
  response[0] = function_code | modbus::FUNCTION_CODE_EXCEPTION_MASK;
  response[1] = static_cast<uint8_t>(code);
  return 2;
}

ResponseStatus check_range(uint16_t start_address, uint16_t count) {
  if (!modbus::helpers::address_range_fits(start_address, count))
    return ExceptionCode::ILLEGAL_DATA_ADDRESS;
  return std::nullopt;
}

ResponseStatus parse_read(std::span<const uint8_t> data, uint16_t max_count, uint16_t &start_address, uint16_t &count) {
  start_address = get_data<uint16_t>(data.data(), 0);
  count = get_data<uint16_t>(data.data(), 2);
  if (count == 0 || count > max_count)
    return ExceptionCode::ILLEGAL_DATA_VALUE;
  return check_range(start_address, count);
}

void assemble_registers(std::span<const uint8_t> values, RegisterValues &registers) {
  for (size_t offset = 0; offset + 1 < values.size(); offset += 2)
    registers.push_back(get_data<uint16_t>(values.data(), offset));
}

// After the function code: byte count(1) + the registers, big-endian. The caller has checked the
// count against the read limit, so the response fits.
size_t put_registers(const RegisterValues &registers, std::span<uint8_t> response) {
  size_t len = 1;
  response[len++] = static_cast<uint8_t>(registers.size() * 2);
  for (uint16_t value : registers) {
    put_u16(&response[len], value);
    len += 2;
  }
  return len;
}

}  // namespace

size_t answer_pdu(modbus::ModbusServerDevice &device, std::span<const uint8_t> request, std::span<uint8_t> response) {
  const uint8_t function_code = request[0];
  // The hub's own length rule, so a request is the shape it would accept off the bus. An unknown code
  // comes back as the minimum, which every known one exceeds.
  const bool known = !modbus::helpers::is_function_code_unknown_length(function_code) &&
                     !modbus::helpers::is_function_code_exception(function_code);
  if (known && modbus::helpers::client_pdu_length(request.data(), request.size()) != request.size()) {
    ESP_LOGW(TAG, "Function 0x%02X request of %zu bytes has the wrong length", function_code, request.size());
    return exception(function_code, ExceptionCode::ILLEGAL_DATA_VALUE, response);
  }
  const std::span<const uint8_t> data = request.subspan(1);
  ResponseStatus status;
  response[0] = function_code;

  switch (static_cast<FunctionCode>(function_code)) {
    case FunctionCode::READ_HOLDING_REGISTERS:
    case FunctionCode::READ_INPUT_REGISTERS: {
      uint16_t start_address;
      uint16_t count;
      status = parse_read(data, modbus::MAX_NUM_OF_REGISTERS_TO_READ, start_address, count);
      if (status.has_value())
        break;
      RegisterValues registers;
      status = static_cast<FunctionCode>(function_code) == FunctionCode::READ_HOLDING_REGISTERS
                   ? device.on_read_holding_registers(start_address, count, registers)
                   : device.on_read_input_registers(start_address, count, registers);
      if (status.has_value())
        break;
      if (registers.size() != count) {
        ESP_LOGE(TAG, "Incorrect response %u requested, %zu returned", count, registers.size());
        status = ExceptionCode::SERVICE_DEVICE_FAILURE;
        break;
      }
      return put_registers(registers, response);
    }
    case FunctionCode::READ_COILS:
    case FunctionCode::READ_DISCRETE_INPUTS: {
      uint16_t start_address;
      uint16_t count;
      status = parse_read(data, modbus::MAX_NUM_OF_COILS_TO_READ, start_address, count);
      if (status.has_value())
        break;
      const uint8_t byte_count = static_cast<uint8_t>(modbus::packed_bit_bytes(count));
      std::span<uint8_t> packed = response.subspan(2, byte_count);
      std::fill(packed.begin(), packed.end(), 0);
      MutablePackedBits bits(packed, count);
      status = static_cast<FunctionCode>(function_code) == FunctionCode::READ_COILS
                   ? device.on_read_coils(start_address, bits)
                   : device.on_read_discrete_inputs(start_address, bits);
      if (status.has_value())
        break;
      response[1] = byte_count;
      return 2 + byte_count;
    }
    case FunctionCode::WRITE_SINGLE_COIL: {
      const uint16_t address = get_data<uint16_t>(data.data(), 0);
      const uint16_t raw_value = get_data<uint16_t>(data.data(), WRITE_SINGLE_VALUE_OFFSET);
      if (raw_value != 0xFF00 && raw_value != 0x0000) {
        status = ExceptionCode::ILLEGAL_DATA_VALUE;
        break;
      }
      const uint8_t single_bit = raw_value == 0xFF00 ? 0x01 : 0x00;
      status = device.on_write_coils(address, PackedBits(std::span<const uint8_t>(&single_bit, 1), 1));
      break;
    }
    case FunctionCode::WRITE_MULTIPLE_COILS: {
      const uint16_t start_address = get_data<uint16_t>(data.data(), 0);
      const uint16_t count = get_data<uint16_t>(data.data(), 2);
      const uint8_t byte_count = get_data<uint8_t>(data.data(), 4);
      if (count == 0 || count > modbus::MAX_NUM_OF_COILS_TO_WRITE || modbus::packed_bit_bytes(count) != byte_count) {
        status = ExceptionCode::ILLEGAL_DATA_VALUE;
        break;
      }
      status = check_range(start_address, count);
      if (status.has_value())
        break;
      status = device.on_write_coils(start_address,
                                     PackedBits(data.subspan(WRITE_MULTIPLE_VALUES_OFFSET, byte_count), count));
      break;
    }
    case FunctionCode::WRITE_SINGLE_REGISTER: {
      RegisterValues registers;
      assemble_registers(data.subspan(WRITE_SINGLE_VALUE_OFFSET, 2), registers);
      status = device.on_write_registers(get_data<uint16_t>(data.data(), 0), registers);
      break;
    }
    case FunctionCode::WRITE_MULTIPLE_REGISTERS: {
      const uint16_t start_address = get_data<uint16_t>(data.data(), 0);
      const uint16_t count = get_data<uint16_t>(data.data(), 2);
      const uint8_t byte_count = get_data<uint8_t>(data.data(), 4);
      if (count == 0 || count > modbus::MAX_NUM_OF_REGISTERS_TO_WRITE || count * 2 != byte_count) {
        status = ExceptionCode::ILLEGAL_DATA_VALUE;
        break;
      }
      status = check_range(start_address, count);
      if (status.has_value())
        break;
      RegisterValues registers;
      assemble_registers(data.subspan(WRITE_MULTIPLE_VALUES_OFFSET, byte_count), registers);
      status = device.on_write_registers(start_address, registers);
      break;
    }
    case FunctionCode::READ_WRITE_MULTIPLE_REGISTERS: {
      const uint16_t read_start = get_data<uint16_t>(data.data(), 0);
      const uint16_t read_count = get_data<uint16_t>(data.data(), 2);
      const uint16_t write_start = get_data<uint16_t>(data.data(), 4);
      const uint16_t write_count = get_data<uint16_t>(data.data(), 6);
      const uint8_t byte_count = get_data<uint8_t>(data.data(), 8);
      if (read_count == 0 || read_count > modbus::MAX_NUM_OF_REGISTERS_TO_READ || write_count == 0 ||
          write_count > modbus::MAX_NUM_OF_REGISTERS_TO_WRITE_RW || write_count * 2 != byte_count) {
        status = ExceptionCode::ILLEGAL_DATA_VALUE;
        break;
      }
      status = check_range(read_start, read_count);
      if (!status.has_value())
        status = check_range(write_start, write_count);
      if (status.has_value())
        break;
      // The write goes first (Modbus 6.17).
      {
        RegisterValues write_registers;
        assemble_registers(data.subspan(READ_WRITE_VALUES_OFFSET, byte_count), write_registers);
        status = device.on_write_registers(write_start, write_registers);
      }
      if (status.has_value())
        break;
      RegisterValues registers;
      status = device.on_read_holding_registers(read_start, read_count, registers);
      if (status.has_value())
        break;
      if (registers.size() != read_count) {
        ESP_LOGE(TAG, "Incorrect response %u requested, %zu returned", read_count, registers.size());
        status = ExceptionCode::SERVICE_DEVICE_FAILURE;
        break;
      }
      return put_registers(registers, response);
    }
    default:
      ESP_LOGW(TAG, "Unsupported function code 0x%02X", function_code);
      status = ExceptionCode::ILLEGAL_FUNCTION;
      break;
  }

  if (status.has_value())
    return exception(function_code, *status, response);
  // A write answers with the first four bytes of its request (Modbus 6.5, 6.6, 6.11, 6.12).
  std::memcpy(&response[1], data.data(), 4);
  return 5;
}

size_t answer_adu(modbus::ModbusServerDevice &device, uint8_t unit_id, std::span<const uint8_t> request,
                  std::span<uint8_t> response) {
  // Transaction id, protocol id and unit id go back as they came; the length is filled in below.
  std::memcpy(response.data(), request.data(), MBAP_HEADER_SIZE);
  const uint8_t requested_unit = request[MBAP_HEADER_SIZE - 1];
  const std::span<const uint8_t> pdu = request.subspan(MBAP_HEADER_SIZE);
  const std::span<uint8_t> out = response.subspan(MBAP_HEADER_SIZE, modbus::MAX_PDU_SIZE);
  size_t pdu_len;
  // 0 and 0xFF reach the device itself: a client of a plain Modbus TCP device sends one of those.
  if (requested_unit != unit_id && requested_unit != UNIT_ID_DIRECT && requested_unit != 0) {
    ESP_LOGD(TAG, "Request for unit %u, this device is unit %u", requested_unit, unit_id);
    pdu_len = exception(pdu[0], ExceptionCode::GATEWAY_TARGET_DEVICE_FAILED_TO_RESPOND, out);
  } else {
    pdu_len = answer_pdu(device, pdu, out);
  }
  put_u16(&response[4], static_cast<uint16_t>(1 + pdu_len));
  return MBAP_HEADER_SIZE + pdu_len;
}

}  // namespace esphome::modbus_tcp
