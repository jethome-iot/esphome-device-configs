#include "jethome_manifest.h"

#include <cctype>
#include <cstring>

#include "esphome/components/json/json_util.h"
#include "esphome/core/log.h"

namespace esphome::jethome_manifest {

static const char *const TAG = "jethome_manifest";

// A slot is named "<slug>.<channel>" and the slug carries dots of its own, so only the last
// component is the channel.
static bool is_channel_slot(const char *key, const std::string &channel) {
  const size_t len = strlen(key);
  if (len < channel.size() || memcmp(key + len - channel.size(), channel.c_str(), channel.size()) != 0)
    return false;
  return len == channel.size() || key[len - channel.size() - 1] == '.';
}

// The OTA component compares the image against a lowercase md5 digest; anything else in that
// field — a sha256, say — would fail the update at the end of the download instead of here.
static bool to_md5(std::string &hash) {
  if (hash.size() != 32)
    return false;
  for (char &c : hash) {
    if (!std::isxdigit(static_cast<unsigned char>(c)))
      return false;
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return true;
}

std::string resolve_url(const std::string &source_url, const std::string &url) {
  if (url.empty() || url.compare(0, 7, "http://") == 0 || url.compare(0, 8, "https://") == 0)
    return url;

  // A query or a fragment is not part of the manifest's path.
  const std::string base = source_url.substr(0, source_url.find_first_of("?#"));

  const size_t scheme = base.find("//");
  if (url.compare(0, 2, "//") == 0)
    return base.substr(0, scheme == std::string::npos ? 0 : scheme) + url;

  const size_t host_end = scheme == std::string::npos ? base.find('/') : base.find('/', scheme + 2);
  if (url[0] == '/')
    return base.substr(0, host_end) + url;
  // A manifest URL that is the bare server has no directory to resolve against.
  if (host_end == std::string::npos)
    return base + "/" + url;
  return base.substr(0, base.rfind('/') + 1) + url;
}

bool parse_manifest(const uint8_t *data, size_t len, const std::string &channel, const std::string &source_url,
                    update::UpdateInfo &info) {
  return json::parse_json(data, len, [&](JsonObject root) -> bool {
    if (!root[ESPHOME_F("latest_firmware")].is<JsonObject>()) {
      ESP_LOGE(TAG, "Manifest has no latest_firmware");
      return false;
    }

    // Named: the proxy `root[...]` returns dies at the end of the expression, and GCC warns
    // about iterating it directly even though the object itself points into the document.
    JsonObjectConst slots = root[ESPHOME_F("latest_firmware")].as<JsonObjectConst>();
    JsonVariantConst firmware;
    for (JsonPairConst slot : slots) {
      if (is_channel_slot(slot.key().c_str(), channel)) {
        firmware = slot.value();
        break;
      }
    }
    if (firmware.isNull()) {
      ESP_LOGE(TAG, "Manifest has no '%s' firmware", channel.c_str());
      return false;
    }

    JsonVariantConst image = firmware[ESPHOME_F("images")][ESPHOME_F("esp.ota")];
    const char *version = firmware[ESPHOME_F("version")].as<const char *>();
    const char *image_url = image[ESPHOME_F("url")].as<const char *>();
    if (version == nullptr || *version == '\0' || image_url == nullptr || *image_url == '\0' ||
        !image[ESPHOME_F("hash")].is<const char *>()) {
      ESP_LOGE(TAG, "The '%s' firmware has no version, or no esp.ota image", channel.c_str());
      return false;
    }

    std::string md5 = image[ESPHOME_F("hash")].as<std::string>();
    if (!to_md5(md5)) {
      ESP_LOGE(TAG, "The esp.ota hash '%s' is not an md5", md5.c_str());
      return false;
    }

    info.latest_version = version;
    info.firmware_url = resolve_url(source_url, image_url);
    info.md5 = std::move(md5);
    if (root[ESPHOME_F("device_name")].is<const char *>())
      info.title = root[ESPHOME_F("device_name")].as<std::string>();
    // Not every channel serves a changelog.
    if (firmware[ESPHOME_F("changelog")].is<const char *>())
      info.release_url = resolve_url(source_url, firmware[ESPHOME_F("changelog")].as<std::string>());
    return true;
  });
}

}  // namespace esphome::jethome_manifest
