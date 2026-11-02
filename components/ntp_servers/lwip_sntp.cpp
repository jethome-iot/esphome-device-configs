#include "lwip_sntp.h"

#ifdef USE_ESP32
// lwIP's own calls, not esp_sntp.h's: those queue the stop and the start for lwIP's task but set
// the slots at once, and the header hides the plain names behind them.
#include "esp_netif.h"
#include "lwip/apps/sntp.h"

namespace esphome::ntp_servers {

static esp_err_t restart(void *ctx) {
  const char *server = static_cast<const char *>(ctx);
  sntp_stop();
  // Every slot: the clock's setup filled one per compiled server, and lwIP moves on to any left.
  for (u8_t i = 0; i < SNTP_MAX_SERVERS; i++)
    sntp_setserver(i, nullptr);
  if (server != nullptr) {
    sntp_setservername(0, server);
    sntp_init();
  }
  return ESP_OK;
}

void restart_sntp(const char *server) { esp_netif_tcpip_exec(restart, const_cast<char *>(server)); }

}  // namespace esphome::ntp_servers
#endif
