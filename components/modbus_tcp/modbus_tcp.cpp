#include "modbus_tcp.h"

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#ifdef USE_WIFI_AP
#include "esphome/components/wifi/wifi_component.h"
#ifdef USE_ESP32
#include <esp_netif.h>
#endif
#endif

#include <cerrno>
#include <cstring>

namespace esphome::modbus_tcp {

static const char *const TAG = "modbus_tcp";

// A client that keeps sending would otherwise keep the loop here: the rest wait for the next pass.
static constexpr uint8_t MAX_REQUESTS_PER_LOOP = 4;

// A client that keeps reconnecting past a refusal, or keeps sending what is not Modbus, would
// otherwise put a warning in the log on each attempt: one goes out per this interval, the rest at
// verbose.
static constexpr uint32_t CLIENT_WARN_INTERVAL_MS = 10000;

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
  const uint32_t now = millis();
  if (this->restart_) {
    this->restart_ = false;
    this->stop_();
    this->start_failed_ = false;
  }
  // A port another server holds, or a socket pool that was full, may be free later.
  if (this->enabled_ && this->listener_ == nullptr &&
      (!this->start_failed_ || now - this->last_start_ >= this->retry_interval_ms_)) {
    this->last_start_ = now;
    this->start_failed_ = !this->start_();
  }
  if (this->listener_ == nullptr)
    return;

  // Clients that left go before new ones come in, so one that reconnects within a pass gets its
  // own slot back instead of a refusal.
  for (auto it = this->clients_.begin(); it != this->clients_.end();) {
    if (this->serve_(**it, now)) {
      ++it;
    } else {
      (*it)->sock->close();
      it = this->clients_.erase(it);
    }
  }

  if (this->listener_->ready())
    this->accept_(now);
}

bool ModbusTcpServer::start_() {
  this->listener_ = socket::socket_ip_loop_monitored(SOCK_STREAM, 0);
  if (this->listener_ == nullptr) {
    ESP_LOGW(TAG, "Could not create the socket: errno %d; trying again in %" PRIu32 " s", errno,
             this->retry_interval_ms_ / 1000);
    return false;
  }
  int enable = 1;
  this->listener_->setsockopt(SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));
  struct sockaddr_storage address;
  const socklen_t length = socket::set_sockaddr_any((struct sockaddr *) &address, sizeof(address), this->port_);
  if (length == 0 || this->listener_->setblocking(false) != 0 ||
      this->listener_->bind((struct sockaddr *) &address, length) != 0 ||
      this->listener_->listen(this->max_clients_) != 0) {
    ESP_LOGW(TAG, "Could not listen on port %u: errno %d; trying again in %" PRIu32 " s", this->port_, errno,
             this->retry_interval_ms_ / 1000);
    this->listener_->close();
    this->listener_.reset();
    return false;
  }
  ESP_LOGI(TAG, "Listening on port %u", this->port_);
  return true;
}

void ModbusTcpServer::stop_() {
  for (auto &client : this->clients_)
    client->sock->close();
  this->clients_.clear();
  // A new listener's first failure is news again.
  this->accept_failing_ = false;
  if (this->listener_ != nullptr) {
    this->listener_->close();
    this->listener_.reset();
    ESP_LOGI(TAG, "Stopped");
  }
}

void ModbusTcpServer::accept_(uint32_t now) {
  // ready() promises nothing more until the listener is drained.
  while (true) {
    struct sockaddr_storage address;
    socklen_t length = sizeof(address);
    auto sock = this->listener_->accept_loop_monitored((struct sockaddr *) &address, &length);
    if (sock == nullptr) {
      // The connection stays queued and ready() stays true, so a failure that lasts would repeat
      // every loop pass: it is said once, until an accept succeeds again.
      if (!would_block(errno) && !this->accept_failing_) {
        ESP_LOGW(TAG, "Accept failed: errno %d", errno);
        this->accept_failing_ = true;
      }
      return;
    }
    this->accept_failing_ = false;
    char peer[socket::SOCKADDR_STR_LEN];
    sock->getpeername_to(peer);
    const char *refusal = nullptr;
    if (this->clients_.size() >= this->max_clients_) {
      refusal = "the clients allowed are connected";
    } else if (!this->serve_wifi_ap_ && this->through_wifi_ap_(address)) {
      refusal = "it came through the WiFi access point";
    }
    if (refusal != nullptr) {
      // Closed at once, so the client sees a refusal instead of waiting for an answer.
      if (this->client_warning_due_(now)) {
        ESP_LOGW(TAG, "Refused %s: %s", peer, refusal);
      } else {
        ESP_LOGV(TAG, "Refused %s: %s", peer, refusal);
      }
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
    // The same clock the idle check reads in this pass: a later millis() would make now minus it wrap.
    client->last_request = now;
    std::memcpy(client->peer, peer, sizeof(peer));
    ESP_LOGD(TAG, "Client %s connected", client->peer);
    this->clients_.push_back(std::move(client));
  }
}

bool ModbusTcpServer::through_wifi_ap_(const struct sockaddr_storage &peer) {
#if defined(USE_WIFI_AP) && defined(USE_ESP32)
  auto *wifi = wifi::global_wifi_component;
  if (wifi == nullptr || !wifi->is_ap_active())
    return false;
  // By the peer's address, not the one it connected to: lwIP takes a packet for any of the
  // device's addresses on any interface, so a client of the access point can reach Ethernet's.
  esp_netif_t *ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
  esp_netif_ip_info_t info;
  if (ap == nullptr || esp_netif_get_ip_info(ap, &info) != ESP_OK)
    return true;  // the access point is up and its subnet unknown: refuse rather than guess
  uint32_t address;
  if (peer.ss_family == AF_INET) {
    address = reinterpret_cast<const struct sockaddr_in &>(peer).sin_addr.s_addr;
#if USE_NETWORK_IPV6
  } else if (peer.ss_family == AF_INET6) {
    // An IPv4 client of an IPv6 listener shows up as ::ffff:a.b.c.d. lwIP's in6_addr is a union
    // without IN6_IS_ADDR_V4MAPPED, so the prefix is read off its words, as socket.cpp does.
    const auto &in6 = reinterpret_cast<const struct sockaddr_in6 &>(peer).sin6_addr;
    if (in6.un.u32_addr[0] != 0 || in6.un.u32_addr[1] != 0 || in6.un.u32_addr[2] != htonl(0xFFFF))
      return false;
    address = in6.un.u32_addr[3];
#endif
  } else {
    return false;
  }
  return (address & info.netmask.addr) == (info.ip.addr & info.netmask.addr);
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
      if (this->client_warning_due_(now)) {
        ESP_LOGW(TAG, "Client %s sent a header that is not Modbus; disconnecting", client.peer);
      } else {
        ESP_LOGV(TAG, "Client %s sent a header that is not Modbus; disconnecting", client.peer);
      }
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

bool ModbusTcpServer::client_warning_due_(uint32_t now) {
  if (this->client_warned_ && now - this->last_client_warning_ < CLIENT_WARN_INTERVAL_MS)
    return false;
  this->client_warned_ = true;
  this->last_client_warning_ = now;
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
