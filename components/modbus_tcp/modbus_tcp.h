#pragma once

#include "esphome/components/modbus/modbus.h"
#include "esphome/components/socket/socket.h"
#include "esphome/core/component.h"

#include "mbap.h"

#include <array>
#include <memory>
#include <vector>

namespace esphome::modbus_tcp {

/// Serves a modbus_server's map over Modbus TCP. Requests go to the server's own handlers, so
/// they answer exactly as over RTU.
class ModbusTcpServer : public Component {
 public:
  void set_server(modbus::ModbusServerDevice *server) { this->server_ = server; }
  void set_max_clients(uint8_t max_clients) { this->max_clients_ = max_clients; }
  void set_idle_timeout(uint32_t idle_timeout_ms) { this->idle_timeout_ms_ = idle_timeout_ms; }
  void set_serve_wifi_ap(bool serve_wifi_ap) { this->serve_wifi_ap_ = serve_wifi_ap; }

  /// The settings below take effect in the next loop(), so the entities that hold them can
  /// restore in any order: the listener opens once, with all of them.
  void set_enabled(bool enabled);
  void set_port(uint16_t port);
  void set_unit_id(uint8_t unit_id) { this->unit_id_ = unit_id; }

  bool is_enabled() const { return this->enabled_; }
  uint16_t get_port() const { return this->port_; }
  uint8_t get_unit_id() const { return this->unit_id_; }
  size_t client_count() const { return this->clients_.size(); }

  void setup() override {}
  void loop() override;
  void dump_config() override;
  void on_shutdown() override { this->stop_(); }
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

 protected:
  struct Client {
    std::unique_ptr<socket::Socket> sock;
    std::array<uint8_t, MAX_ADU_SIZE> rx;
    size_t rx_len{0};
    std::array<uint8_t, MAX_ADU_SIZE> tx;
    size_t tx_len{0};
    size_t tx_sent{0};
    uint32_t last_request{0};
    // ready() said there was data and the socket has not been read empty since.
    bool unread{false};
    char peer[socket::SOCKADDR_STR_LEN]{};
  };

  bool start_();
  void stop_();
  void accept_();
  bool through_wifi_ap_(socket::Socket &sock);
  /// False when the client has to go.
  bool serve_(Client &client, uint32_t now);
  bool flush_(Client &client);

  modbus::ModbusServerDevice *server_{nullptr};
  std::unique_ptr<socket::ListenSocket> listener_;
  std::vector<std::unique_ptr<Client>> clients_;
  uint32_t idle_timeout_ms_{60000};
  uint16_t port_{502};
  uint8_t max_clients_{2};
  uint8_t unit_id_{1};
  bool enabled_{false};
  bool serve_wifi_ap_{false};
  bool restart_{false};
};

}  // namespace esphome::modbus_tcp
