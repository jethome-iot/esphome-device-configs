#pragma once

#include "esphome/core/component.h"
#include "esphome/core/defines.h"
#include "esphome/core/log.h"
#include "esphome/components/web_server_base/web_server_base.h"
#include "esphome/components/filesystem_storage_abstract/filesystem_storage_abstract.h"
#include "routes.h"
#include <string>

#ifdef USE_ESP32
#include <dirent.h>
#include <sys/stat.h>
#endif

namespace esphome {
namespace web_file_browser {

// JSON file API at <url_prefix>/* (list, read, write, upload, download, delete,
// mkdir, rename, copy, info) over any FilesystemStorageAbstract mount. The
// dashboard's Files screen is its client; the TS contract lives in client/.
class WebFileBrowser : public AsyncWebHandler, public Component {
 public:
  WebFileBrowser(web_server_base::WebServerBase *base, filesystem_storage_abstract::FilesystemStorageAbstract *storage)
      : base_(base), storage_(storage) {}

  void setup() override;
  void dump_config() override;
  float get_setup_priority() const override {
    // After WiFi. Nothing orders this against web_server: WIFI - 1.0f is exactly
    // web_server's own priority, so the two fall to registration order.
    return setup_priority::WIFI - 1.0f;
  }

  bool canHandle(AsyncWebServerRequest *request) const override;
  void handleRequest(AsyncWebServerRequest *request) override;
  void handleUpload(AsyncWebServerRequest *request, const std::string &filename, size_t index, uint8_t *data,
                    size_t len, bool final) override;
  void handleBody(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) override;

  void set_url_prefix(const std::string &prefix) { this->url_prefix_ = prefix; }
  const std::string &get_url_prefix() const { return this->url_prefix_; }

 protected:
  web_server_base::WebServerBase *base_;
  filesystem_storage_abstract::FilesystemStorageAbstract *storage_;
  std::string url_prefix_{"/files"};

  // Upload state. upload_error_ carries the failure from handleUpload() (which
  // runs per chunk) to handle_upload_request_() (which sends the reply).
  FILE *upload_file_{nullptr};
  std::string upload_path_;
  std::string upload_error_;
  bool upload_active_{false};
  size_t upload_written_{0};
  // Whether handleUpload() ran at all — a zero-length part never reaches it.
  bool upload_seen_{false};
  // Whether this upload created the destination, so failure may remove it.
  bool upload_created_{false};

  // /write state: the raw body streams through handleBody() straight into the file.
  FILE *write_file_{nullptr};
  std::string write_error_;
  bool write_seen_{false};

  // Handler methods
  void handle_list_request_(AsyncWebServerRequest *request);
  void handle_download_request_(AsyncWebServerRequest *request);
  void handle_upload_request_(AsyncWebServerRequest *request);
  void handle_read_request_(AsyncWebServerRequest *request);
  void handle_write_request_(AsyncWebServerRequest *request);
  void handle_delete_request_(AsyncWebServerRequest *request);
  void handle_mkdir_request_(AsyncWebServerRequest *request);
  void handle_rename_request_(AsyncWebServerRequest *request);
  void handle_copy_request_(AsyncWebServerRequest *request);
  void handle_info_request_(AsyncWebServerRequest *request);

  // Helper methods
  std::string url_(AsyncWebServerRequest *request) const;
  // Answers 405 itself when the method is wrong, so a false return is a finished
  // request.
  bool check_method_(AsyncWebServerRequest *request, const Route &route);
  std::string get_base_path_() const;
  std::string resolve_path_(const std::string &path) const;
  bool is_valid_path_(const std::string &path) const;
  // depth bounds the recursion: both run on the 4352-byte esp_http_server task
  // stack, where a deep enough tree would smash it instead of failing the request.
  // tree_too_deep_ applies the same bound without deleting, ahead of a delete.
  bool tree_too_deep_(const std::string &path, unsigned depth = 0) const;
  bool delete_recursive_(const std::string &path, unsigned depth = 0);
  bool copy_file_(const std::string &src, const std::string &dst);
  bool copy_recursive_(const std::string &src, const std::string &dst, unsigned depth = 0);
  // Lets go of what a transfer was holding when a format latched the filesystem off.
  void abandon_transfers_();
  // Closes the upload file and removes whatever was written of it. Called with an Access
  // claim held, so the handle it closes is still the format's to free, not already freed.
  void discard_upload_();
#ifdef USE_ESP32
  // A format frees every open handle, so a close has to happen inside a claim; false says
  // the claim was gone and the handle was let go rather than closed.
  bool close_file_(FILE *file);
  bool close_dir_(DIR *dir);
#endif
  void send_json_error_(AsyncWebServerRequest *request, const std::string &message, int code = 400);
  void send_json_success_(AsyncWebServerRequest *request, const std::string &message = "success");
  std::string json_escape_(const std::string &str) const;
};

}  // namespace web_file_browser
}  // namespace esphome
