#include "modbus_tcp.h"

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#ifdef USE_WIFI_AP
#include "esphome/components/wifi/wifi_component.h"
#endif

#include <cerrno>
#include <cstring>

namespace esphome::modbus_tcp {

static const char *const TAG = "modbus_tcp";

// A client that keeps sending would otherwise keep the loop here: the rest wait for the next pass.
static constexpr uint8_t MAX_REQUESTS_PER_LOOP = 4;

static bool would_block(int err) { return err == EWOULDBLOCK || err == EAGAIN; }

void ModbusTcpServer::set_enabled(bool enabled) {
  if (enabled != this->enabled_)
    this->restart_ = true;
  this->enabled_ = enabled;
}

void ModbusTcpServer::set_port(uint16_t port) {
  if (port != this->port_)
    this->restart_ = true;
  this->port_ = port;
}

void ModbusTcpServer::loop() {
  if (this->restart_) {
    this->restart_ = false;
    this->stop_();
    if (this->enabled_ && !this->start_())
      this->stop_();
  }
  if (this->listener_ == nullptr)
    return;

  if (this->listener_->ready())
    this->accept_();

  const uint32_t now = millis();
  for (auto it = this->clients_.begin(); it != this->clients_.end();) {
    if (this->serve_(**it, now)) {
      ++it;
    } else {
      (*it)->sock->close();
      it = this->clients_.erase(it);
    }
  }
}

bool ModbusTcpServer::start_() {
  this->listener_ = socket::socket_ip_loop_monitored(SOCK_STREAM, 0);
  if (this->listener_ == nullptr) {
    ESP_LOGW(TAG, "Could not create the socket: errno %d", errno);
    return false;
  }
  int enable = 1;
  this->listener_->setsockopt(SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));
  struct sockaddr_storage address;
  const socklen_t length = socket::set_sockaddr_any((struct sockaddr *) &address, sizeof(address), this->port_);
  if (length == 0 || this->listener_->setblocking(false) != 0 ||
      this->listener_->bind((struct sockaddr *) &address, length) != 0 ||
      this->listener_->listen(this->max_clients_) != 0) {
    ESP_LOGW(TAG, "Could not listen on port %u: errno %d", this->port_, errno);
    return false;
  }
  ESP_LOGI(TAG, "Listening on port %u", this->port_);
  return true;
}

void ModbusTcpServer::stop_() {
  for (auto &client : this->clients_)
    client->sock->close();
  this->clients_.clear();
  if (this->listener_ != nullptr) {
    this->listener_->close();
    this->listener_.reset();
    ESP_LOGI(TAG, "Stopped");
  }
}

void ModbusTcpServer::accept_() {
  // ready() promises nothing more until the listener is drained.
  while (true) {
    struct sockaddr_storage address;
    socklen_t length = sizeof(address);
    auto sock = this->listener_->accept_loop_monitored((struct sockaddr *) &address, &length);
    if (sock == nullptr) {
      if (!would_block(errno))
        ESP_LOGW(TAG, "Accept failed: errno %d", errno);
      return;
    }
    char peer[socket::SOCKADDR_STR_LEN];
    sock->getpeername_to(peer);
    if (this->clients_.size() >= this->max_clients_) {
      // Closed at once, so the client sees a refusal instead of waiting for an answer.
      ESP_LOGW(TAG, "Refused %s: %u clients already connected", peer, this->max_clients_);
      sock->close();
      continue;
    }
    if (!this->serve_wifi_ap_ && this->through_wifi_ap_(*sock)) {
      ESP_LOGW(TAG, "Refused %s: it came through the WiFi access point", peer);
      sock->close();
      continue;
    }
    int enable = 1;
    sock->setsockopt(IPPROTO_TCP, TCP_NODELAY, &enable, sizeof(enable));
    if (sock->setblocking(false) != 0) {
      ESP_LOGW(TAG, "Could not make the socket of %s non-blocking: errno %d", peer, errno);
      sock->close();
      continue;
    }
    auto client = std::make_unique<Client>();
    client->sock = std::move(sock);
    client->last_request = millis();
    std::memcpy(client->peer, peer, sizeof(peer));
    ESP_LOGD(TAG, "Client %s connected", client->peer);
    this->clients_.push_back(std::move(client));
  }
}

bool ModbusTcpServer::through_wifi_ap_(socket::Socket &sock) {
#ifdef USE_WIFI_AP
  auto *wifi = wifi::global_wifi_component;
  if (wifi == nullptr || !wifi->is_ap_active())
    return false;
  char local[socket::SOCKADDR_STR_LEN];
  sock.getsockname_to(local);
  char ap[network::IP_ADDRESS_BUFFER_SIZE];
  wifi->wifi_soft_ap_ip().str_to(ap);
  // An IPv6 socket shows an IPv4 peer's address in its mapped form.
  static constexpr const char *MAPPED = "::ffff:";
  const char *address = strncmp(local, MAPPED, strlen(MAPPED)) == 0 ? local + strlen(MAPPED) : local;
  return strcmp(address, ap) == 0;
#else
  return false;
#endif
}

bool ModbusTcpServer::serve_(Client &client, uint32_t now) {
  if (!this->flush_(client))
    return false;
  // A request is only taken in once the previous answer is out, so a client that sends and never
  // reads cannot pile answers up here. Once ready() has said there is data, reading goes on until
  // the socket is empty, across loops if an answer is still going out.
  bool may_read = client.unread || client.sock->ready();
  uint8_t answered = 0;
  while (client.tx_len == 0 && answered < MAX_REQUESTS_PER_LOOP) {
    const Frame frame = find_frame(std::span<const uint8_t>(client.rx.data(), client.rx_len));
    if (frame.status == FrameStatus::INVALID) {
      ESP_LOGW(TAG, "Client %s sent a malformed header; disconnecting", client.peer);
      return false;
    }
    if (frame.status == FrameStatus::COMPLETE) {
      client.last_request = now;
      answered++;
      client.tx_len =
          answer_adu(*this->server_, this->unit_id_, std::span<const uint8_t>(client.rx.data(), frame.size), client.tx);
      client.tx_sent = 0;
      client.rx_len -= frame.size;
      std::memmove(client.rx.data(), client.rx.data() + frame.size, client.rx_len);
      if (!this->flush_(client))
        return false;
      continue;
    }
    if (!may_read)
      break;
    // The buffer holds one whole request, so INCOMPLETE always leaves room to read into.
    const ssize_t received = client.sock->read(client.rx.data() + client.rx_len, client.rx.size() - client.rx_len);
    if (received == 0) {
      ESP_LOGD(TAG, "Client %s disconnected", client.peer);
      return false;
    }
    if (received < 0) {
      if (!would_block(errno)) {
        ESP_LOGD(TAG, "Client %s: read failed, errno %d", client.peer, errno);
        return false;
      }
      may_read = false;
      break;
    }
    client.rx_len += received;
  }
  client.unread = may_read;
  if (now - client.last_request > this->idle_timeout_ms_) {
    ESP_LOGD(TAG, "Client %s idle for %" PRIu32 " s; disconnecting", client.peer, this->idle_timeout_ms_ / 1000);
    return false;
  }
  return true;
}

bool ModbusTcpServer::flush_(Client &client) {
  while (client.tx_sent < client.tx_len) {
    const ssize_t sent = client.sock->write(client.tx.data() + client.tx_sent, client.tx_len - client.tx_sent);
    if (sent < 0) {
      if (would_block(errno))
        return true;
      ESP_LOGD(TAG, "Client %s: write failed, errno %d", client.peer, errno);
      return false;
    }
    client.tx_sent += sent;
  }
  client.tx_len = 0;
  client.tx_sent = 0;
  return true;
}

void ModbusTcpServer::dump_config() {
  ESP_LOGCONFIG(TAG,
                "Modbus TCP:\n"
                "  Enabled: %s\n"
                "  Port: %u\n"
                "  Unit ID: %u\n"
                "  Max clients: %u\n"
                "  Idle timeout: %" PRIu32 " s\n"
                "  Through the WiFi AP: %s",
                YESNO(this->enabled_), this->port_, this->unit_id_, this->max_clients_, this->idle_timeout_ms_ / 1000,
                YESNO(this->serve_wifi_ap_));
}

}  // namespace esphome::modbus_tcp
