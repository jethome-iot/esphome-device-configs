#pragma once

#include <string>

#include "esphome/components/update/update_entity.h"

namespace esphome::jethome_manifest {

/// Fills `info` from a JetHome firmware server's device manifest.
///
/// `channel` picks one entry of `latest_firmware` ("release", "nightly", ...) and `source_url`,
/// the URL the manifest came from, resolves the relative URLs inside it.
bool parse_manifest(const uint8_t *data, size_t len, const std::string &channel, const std::string &source_url,
                    update::UpdateInfo &info);

/// Resolves a manifest URL against the manifest's own: an absolute URL is kept, "/path" takes the
/// server, anything else the directory the manifest sits in.
std::string resolve_url(const std::string &source_url, const std::string &url);

}  // namespace esphome::jethome_manifest
