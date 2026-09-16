#include "web_file_browser.h"
#include "json_escape.h"
#include "esphome/core/log.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>

#ifdef USE_ESP32
#include <sys/stat.h>
#include <dirent.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_http_server.h>
#endif

namespace esphome {
namespace web_file_browser {

static const char *const TAG = "web_file_browser";

// How deep delete/copy may recurse. Both run on the esp_http_server task, whose
// stack stock sizes at 4352 bytes; at roughly 200 bytes a frame, plus whatever
// readdir() puts there, a deeper tree would smash the stack instead of failing
// the request. mkdir imposes no depth limit of its own, so the tree can be
// deeper than this — say so rather than report a partial delete as success.
static const unsigned MAX_RECURSION_DEPTH = 8;

void WebFileBrowser::setup() {
  this->base_->init();
  this->base_->add_handler(this);
}

void WebFileBrowser::dump_config() {
  ESP_LOGCONFIG(TAG, "Web File Browser:");
  ESP_LOGCONFIG(TAG, "  Filesystem type: %s", this->storage_->get_filesystem_type());
  ESP_LOGCONFIG(TAG, "  Mount path: %s", this->storage_->get_base_path().c_str());
  ESP_LOGCONFIG(TAG, "  API: %s/*", this->url_prefix_.c_str());
}

std::string WebFileBrowser::url_(AsyncWebServerRequest *request) const {
#ifdef USE_ESP32
  char buf[AsyncWebServerRequest::URL_BUF_SIZE];
  return std::string(request->url_to(buf));
#else
  return request->url();
#endif
}

bool WebFileBrowser::canHandle(AsyncWebServerRequest *request) const {
  if (!this->storage_->is_mounted()) {
    return false;
  }
  return this->url_(request).starts_with(this->url_prefix_ + "/");
}

void WebFileBrowser::handleRequest(AsyncWebServerRequest *request) {
  const std::string url = this->url_(request);

  ESP_LOGD(TAG, "Handling request: %s", url.c_str());

  const Route *route = route_for(url, this->url_prefix_);
  if (route == nullptr) {
    request->send(404, "text/plain", "Not Found");
    return;
  }
  if (!this->check_method_(request, *route)) {
    return;
  }

  switch (route->id) {
    case RouteId::INFO:
      this->handle_info_request_(request);
      break;
    case RouteId::LIST:
      this->handle_list_request_(request);
      break;
    case RouteId::READ:
      this->handle_read_request_(request);
      break;
    case RouteId::DOWNLOAD:
      this->handle_download_request_(request);
      break;
    case RouteId::WRITE:
      this->handle_write_request_(request);
      break;
    case RouteId::UPLOAD:
      this->handle_upload_request_(request);
      break;
    case RouteId::DELETE:
      this->handle_delete_request_(request);
      break;
    case RouteId::MKDIR:
      this->handle_mkdir_request_(request);
      break;
    case RouteId::RENAME:
      this->handle_rename_request_(request);
      break;
    case RouteId::COPY:
      this->handle_copy_request_(request);
      break;
  }
}

bool WebFileBrowser::check_method_(AsyncWebServerRequest *request, const Route &route) {
  if (request->method() == (route.mutating ? HTTP_POST : HTTP_GET)) {
    return true;
  }
  const char *allow = route.mutating ? "POST" : "GET";
  ESP_LOGW(TAG, "Refusing %s on a %s-only route", route.name, allow);
#ifdef USE_ESP32
  // By hand because AsyncWebServerRequest::send() turns every status it does not
  // know into a 500, and because httpd_resp_set_hdr() keeps the pointer it is
  // given rather than a copy.
  httpd_req_t *req = *request;
  httpd_resp_set_status(req, "405 Method Not Allowed");
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Allow", allow);
  static const char BODY[] = R"({"success":false,"error":"Method not allowed"})";
  httpd_resp_send(req, BODY, sizeof(BODY) - 1);
#else
  this->send_json_error_(request, "Method not allowed", 405);
#endif
  return false;
}

void WebFileBrowser::handleUpload(AsyncWebServerRequest *request, const std::string &filename, size_t index,
                                  uint8_t *data, size_t len, bool final) {
  if (!this->storage_->is_mounted()) {
    return;
  }

  // canHandle() claims the whole prefix, so the reader offers us the parts of a
  // multipart POST to any route; only /upload may open a file for them.
  const Route *route = route_for(this->url_(request), this->url_prefix_);
  if (route == nullptr || route->id != RouteId::UPLOAD) {
    return;
  }

  this->upload_seen_ = true;

  // The multipart reader announces a new file with an empty chunk and then
  // repeats index 0 for its first data chunk, so index alone cannot mark the
  // start. An upload still marked active with bytes written is a transfer that
  // was cut off before its final chunk — drop it rather than write into it.
  //
  // "Active with nothing written" cannot outlive a request: the reader emits the
  // start marker and the first data chunk back to back in one callback, with no
  // recv or yield between them, so there is no window to abort in. Identifying
  // the owning request would make that independent of the reader, but it cannot
  // be done by pointer — AsyncWebServerRequest is a stack local in the httpd
  // handler, so every upload sees the same address.
  if (index == 0 && (!this->upload_active_ || this->upload_written_ > 0)) {
    this->discard_upload_();
    this->upload_error_.clear();
    this->upload_active_ = true;

    // Get path from query string (includes subdirectory), fallback to filename
    if (request->hasParam("path")) {
      this->upload_path_ = this->resolve_path_(request->getParam("path")->value());
    } else {
      this->upload_path_ = this->resolve_path_(filename);
    }
    ESP_LOGD(TAG, "Starting upload to: %s", this->upload_path_.c_str());

    // Both failure branches below clear upload_path_ BEFORE the reset: nothing was
    // opened yet, so discard_upload_()'s remove() would delete something this
    // upload never created — a path the validator just rejected, or the existing
    // empty directory that made fopen() fail with EISDIR.
    if (!this->is_valid_path_(this->upload_path_)) {
      ESP_LOGE(TAG, "Invalid upload path: %s", this->upload_path_.c_str());
      this->upload_error_ = "Invalid path";
      this->upload_path_.clear();
      // Full reset: leaving upload_active_ set would make a later index-0 chunk
      // look like a live transfer and skip opening the file.
      this->discard_upload_();
      return;
    }

    // Only a file this upload brought into being may be removed when it fails.
    // Overwriting an existing one truncates it at fopen("wb"), so deleting it
    // afterwards would turn a partial overwrite into no file at all — the same
    // rule handle_write_request_ follows.
    FILE *existing = fopen(this->upload_path_.c_str(), "rb");
    this->upload_created_ = existing == nullptr;
    if (existing != nullptr) {
      fclose(existing);
    }

    this->upload_file_ = fopen(this->upload_path_.c_str(), "wb");
    if (this->upload_file_ == nullptr) {
      ESP_LOGE(TAG, "Failed to open file for writing: %s", this->upload_path_.c_str());
      this->upload_error_ = "Failed to open file for writing";
      this->upload_path_.clear();
      this->discard_upload_();
      return;
    }
  }

  if (this->upload_file_ != nullptr && len > 0) {
    size_t written = fwrite(data, 1, len, this->upload_file_);
    this->upload_written_ += written;
    if (written != len) {
      ESP_LOGE(TAG, "Failed to write data to file: %s", this->upload_path_.c_str());
      this->upload_error_ = "Failed to write file";
      this->discard_upload_();
      return;
    }
  }

  if (final) {
    if (this->upload_file_ != nullptr) {
      // close is part of the write: LittleFS commits the tail of the file cache
      // and the metadata entry here, so a full filesystem surfaces at fclose and
      // not at any fwrite we checked. Dropping this would report success for a
      // truncated file — the exact lie this endpoint stopped telling.
      bool closed = fclose(this->upload_file_) == 0;
      this->upload_file_ = nullptr;
      if (!closed) {
        ESP_LOGE(TAG, "Failed to flush upload '%s': errno=%d (%s)", this->upload_path_.c_str(), errno, strerror(errno));
        this->upload_error_ = "Failed to write file";
        if (this->upload_created_) {
          remove(this->upload_path_.c_str());
        }
      } else {
        ESP_LOGD(TAG, "Upload complete");
      }
    }
    this->upload_path_.clear();
    this->upload_active_ = false;
    this->upload_written_ = 0;
    this->upload_created_ = false;
    // Response is sent by handle_upload_request_ which is called after handleUpload
  }
}

// Only /write carries a raw body; every other POST is form-encoded and lands in
// the request parameters. The chunks go straight into the file: a text edit can
// be up to 1 MB and buffering it would not fit next to the rest of the heap.
void WebFileBrowser::handleBody(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
#ifdef USE_ESP32
  const Route *route = route_for(this->url_(request), this->url_prefix_);
  if (route == nullptr || route->id != RouteId::WRITE)
    return;
  if (index == 0) {
    this->write_seen_ = true;
    this->write_error_.clear();
    if (this->write_file_ != nullptr) {
      fclose(this->write_file_);
      this->write_file_ = nullptr;
    }
    if (!request->hasParam("path")) {
      this->write_error_ = "Missing path parameter";
      return;
    }
    std::string full_path = this->resolve_path_(request->getParam("path")->value());
    if (!this->is_valid_path_(full_path)) {
      this->write_error_ = "Invalid path";
      return;
    }
    this->write_file_ = fopen(full_path.c_str(), "w");
    if (this->write_file_ == nullptr) {
      this->write_error_ = "Failed to open file for writing";
      return;
    }
  }
  if (this->write_file_ == nullptr)
    return;
  if (fwrite(data, 1, len, this->write_file_) != len)
    this->write_error_ = "Failed to write file";
  if (index + len >= total) {
    // LittleFS commits at close, so a full filesystem is reported here.
    if (fclose(this->write_file_) != 0 && this->write_error_.empty())
      this->write_error_ = "Failed to write file";
    this->write_file_ = nullptr;
  }
#endif
}

void WebFileBrowser::handle_list_request_(AsyncWebServerRequest *request) {
#ifdef USE_ESP32
  std::string path = "/";
  if (request->hasParam("path")) {
    path = request->getParam("path")->value();
  }

  std::string full_path = this->resolve_path_(path);

  if (!this->is_valid_path_(full_path)) {
    this->send_json_error_(request, "Invalid path");
    return;
  }

  DIR *dir = opendir(full_path.c_str());
  if (!dir) {
    this->send_json_error_(request, "Failed to open directory");
    return;
  }

  // Fixed buffer, not a string that grows per entry: the partition holds far more
  // empty files than the heap holds JSON, and a failed allocation aborts the
  // firmware rather than the request. Allocated before any header is set.
  static const size_t OUT_CHUNK = 4096;
  auto buffer = std::unique_ptr<char[]>(new (std::nothrow) char[OUT_CHUNK + 1]);
  if (buffer == nullptr) {
    ESP_LOGE(TAG, "Out of memory listing '%s'", full_path.c_str());
    closedir(dir);
    this->send_json_error_(request, "Out of memory", 500);
    return;
  }
  char *out = buffer.get();

  httpd_req_t *req = *request;
  httpd_resp_set_type(req, "application/json");

  size_t used = 0;
  bool sent = true;
  // Nothing reaches the wire until the buffer first overflows, so every listing
  // that fits in it can still be answered with the error envelope.
  bool streamed = false;

  auto put = [&](const char *data, size_t len) {
    while (sent && len > 0) {
      size_t room = std::min(OUT_CHUNK - used, len);
      memcpy(out + used, data, room);
      used += room;
      data += room;
      len -= room;
      if (used == OUT_CHUNK) {
        sent = httpd_resp_send_chunk(req, out, used) == ESP_OK;
        streamed = true;
        used = 0;
        // Yield to prevent watchdog timeout on large directories
        vTaskDelay(1);
      }
    }
  };

  put("[", 1);

  bool first = true;
  struct dirent *entry;

  // Same readdir() caveat as the recursive helpers: nullptr means both
  // end-of-directory and read error, and only errno tells them apart.
  errno = 0;
  while (sent && (entry = readdir(dir)) != nullptr) {
    // Skip . and ..
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
      errno = 0;
      continue;
    }

    if (!first) {
      put(",", 1);
    }
    first = false;

    std::string entry_path = full_path + "/" + entry->d_name;
    struct stat st;
    bool is_dir = false;
    size_t size = 0;
    time_t mtime = 0;

    if (stat(entry_path.c_str(), &st) == 0) {
      is_dir = S_ISDIR(st.st_mode);
      size = st.st_size;
      mtime = st.st_mtime;
    }

    // Room for the widest size_t and time_t the format can produce.
    char meta[128];
    snprintf(meta, sizeof(meta), R"(","type":"%s","size":%zu,"mtime":%lld})", is_dir ? "directory" : "file", size,
             static_cast<long long>(mtime));
    static const char NAME[] = R"({"name":")";
    put(NAME, sizeof(NAME) - 1);
    char esc[JSON_ESCAPE_MAX];
    for (const char *c = entry->d_name; *c != '\0'; c++) {
      put(esc, json_escape_char(esc, *c));
    }
    put(meta, strlen(meta));
    // Last in the body: put() sends, and a socket call of its own sets errno.
    errno = 0;
  }

  int read_errno = errno;
  closedir(dir);

  if (!sent) {
    httpd_resp_send_chunk(req, nullptr, 0);
    return;
  }

  if (read_errno != 0) {
    ESP_LOGE(TAG, "Failed to read directory '%s': errno=%d (%s)", full_path.c_str(), read_errno, strerror(read_errno));
    if (!streamed) {
      this->send_json_error_(request, "Failed to read directory");
      return;
    }
    // Once a chunk has gone out the envelope cannot be retracted, so the array is
    // left unclosed: an unparseable body beats a short listing that parses.
    httpd_resp_send_chunk(req, nullptr, 0);
    return;
  }

  put("]", 1);

  if (!streamed) {
    // No embedded NUL to lose to strlen: json_escape_char turns control bytes
    // into \u00XX and a directory entry cannot carry one anyway.
    out[used] = '\0';
    request->send(200, "application/json", out);
    return;
  }
  if (used > 0) {
    httpd_resp_send_chunk(req, out, used);
  }
  httpd_resp_send_chunk(req, nullptr, 0);
#else
  this->send_json_error_(request, "Not supported on this platform");
#endif
}

// The name comes off a writable filesystem: a quote would end the quoted string
// early, and esp_http_server drops any header value holding CR or LF. So the real
// name goes out percent-encoded (RFC 6266), with a scrubbed ASCII copy beside it.
static std::string content_disposition(const std::string &filename) {
  static const char HEX[] = "0123456789ABCDEF";
  std::string ascii;
  std::string encoded;
  for (char c : filename) {
    uint8_t byte = static_cast<uint8_t>(c);
    bool unreserved = (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') || (byte >= '0' && byte <= '9') ||
                      byte == '-' || byte == '.' || byte == '_' || byte == '~';
    if (unreserved) {
      ascii += c;
      encoded += c;
    } else {
      ascii += '_';
      encoded += '%';
      encoded += HEX[byte >> 4];
      encoded += HEX[byte & 0x0F];
    }
  }
  return "attachment; filename=\"" + ascii + "\"; filename*=UTF-8''" + encoded;
}

void WebFileBrowser::handle_download_request_(AsyncWebServerRequest *request) {
#ifdef USE_ESP32
  if (!request->hasParam("path")) {
    this->send_json_error_(request, "Missing path parameter");
    return;
  }

  std::string path = request->getParam("path")->value();
  std::string full_path = this->resolve_path_(path);

  if (!this->is_valid_path_(full_path)) {
    this->send_json_error_(request, "Invalid path");
    return;
  }

  FILE *file = fopen(full_path.c_str(), "rb");
  if (!file) {
    this->send_json_error_(request, "Failed to open file", 404);
    return;
  }

  // From the resolved path, so a trailing slash on the request cannot leave it empty.
  std::string filename = full_path.substr(full_path.find_last_of('/') + 1);

  // Send file in chunks. Allocated before any header is set, so a failure can
  // still be answered as an error instead of half a response.
  static const size_t CHUNK_SIZE = 4096;  // 4KB chunks
  // Exceptions are disabled in this build, so a throwing new would abort the
  // whole firmware on a heap that cannot spare 4 KB. Fail the request instead.
  auto buffer = std::unique_ptr<uint8_t[]>(new (std::nothrow) uint8_t[CHUNK_SIZE]);
  if (buffer == nullptr) {
    ESP_LOGE(TAG, "Out of memory serving '%s'", full_path.c_str());
    fclose(file);
    this->send_json_error_(request, "Out of memory", 500);
    return;
  }

  // Stream file in chunks to avoid memory issues with large files
  httpd_req_t *req = *request;

  // Set headers
  httpd_resp_set_type(req, "application/octet-stream");
  std::string disposition = content_disposition(filename);
  httpd_resp_set_hdr(req, "Content-Disposition", disposition.c_str());

  while (true) {
    size_t read_bytes = fread(buffer.get(), 1, CHUNK_SIZE, file);
    if (read_bytes == 0) {
      // A read error leaves feof() clear and the position indeterminate, so
      // looping on feof() alone would spin here forever.
      if (ferror(file) != 0) {
        ESP_LOGE(TAG, "Failed to read '%s' while downloading", full_path.c_str());
      }
      break;
    }
    if (httpd_resp_send_chunk(req, reinterpret_cast<const char *>(buffer.get()), read_bytes) != ESP_OK) {
      ESP_LOGE(TAG, "Failed to send chunk");
      break;
    }
    // Yield to prevent watchdog timeout on large files
    vTaskDelay(1);
  }

  // Send empty chunk to signal end
  httpd_resp_send_chunk(req, nullptr, 0);

  fclose(file);
#else
  this->send_json_error_(request, "Not supported on this platform");
#endif
}

void WebFileBrowser::handle_upload_request_(AsyncWebServerRequest *request) {
  // Upload itself is handled in the handleUpload callback, which has already run
  // for every chunk by the time we get here.
  std::string error = this->upload_error_;
  bool seen = this->upload_seen_;
  this->upload_error_.clear();
  this->upload_seen_ = false;
  // Nothing normally left to discard; guards against a transfer that never saw
  // its final chunk poisoning the next upload.
  this->discard_upload_();

  if (!error.empty()) {
    this->send_json_error_(request, error);
    return;
  }

  // The multipart reader skips zero-length parts outright, so handleUpload never
  // ran and nothing was written. Reporting success here would lose the file in
  // silence — which is exactly what this endpoint stopped doing. Clients create
  // an empty file with /write instead.
  if (!seen) {
    this->send_json_error_(request, "No file received");
    return;
  }

  this->send_json_success_(request, "Upload complete");
}

// GET /read?path=: the file streams out as the "content" string of the JSON
// envelope, escaped a chunk at a time. Buffering it whole is not an option here:
// AsyncResponseStream is a std::string, not a stream, so the file, its escaped
// copy and the response would be live at once, in internal DRAM (the psram
// component never sets CONFIG_SPIRAM_USE_MALLOC), and std::string's throwing
// allocation would abort the firmware rather than fail the request.
void WebFileBrowser::handle_read_request_(AsyncWebServerRequest *request) {
#ifdef USE_ESP32
  if (!request->hasParam("path")) {
    this->send_json_error_(request, "Missing path parameter");
    return;
  }

  std::string path = request->getParam("path")->value();
  std::string full_path = this->resolve_path_(path);

  if (!this->is_valid_path_(full_path)) {
    this->send_json_error_(request, "Invalid path");
    return;
  }

  FILE *file = fopen(full_path.c_str(), "rb");
  if (!file) {
    this->send_json_error_(request, "Failed to open file", 404);
    return;
  }

  // Not a memory bound any more — an editor that has to load the answer is the
  // limit. Keep it in step with README.md.
  static const long MAX_READ_SIZE = 1024 * 1024;
  fseek(file, 0, SEEK_END);
  long file_size = ftell(file);
  fseek(file, 0, SEEK_SET);
  if (file_size < 0) {
    fclose(file);
    this->send_json_error_(request, "Failed to read file");
    return;
  }
  if (file_size > MAX_READ_SIZE) {
    fclose(file);
    this->send_json_error_(request, "File too large to edit");
    return;
  }

  // One allocation for both halves: raw bytes in, escaped bytes out. Nothing
  // else allocates for the rest of the response, whatever the file's size.
  static const size_t READ_CHUNK = 4096;
  static const size_t OUT_CHUNK = 4096;
  // Exceptions are disabled in this build, so a throwing new would abort the
  // whole firmware instead of failing the request. Allocated before any header
  // is set, so a failure is still a clean error envelope.
  auto buffer = std::unique_ptr<char[]>(new (std::nothrow) char[READ_CHUNK + OUT_CHUNK]);
  if (buffer == nullptr) {
    ESP_LOGE(TAG, "Out of memory reading '%s'", full_path.c_str());
    fclose(file);
    this->send_json_error_(request, "Out of memory", 500);
    return;
  }
  char *in = buffer.get();
  char *out = in + READ_CHUNK;

  httpd_req_t *req = *request;
  httpd_resp_set_type(req, "application/json");

  auto send_chunk = [req](const char *data, size_t len) {
    if (httpd_resp_send_chunk(req, data, len) != ESP_OK) {
      ESP_LOGE(TAG, "Failed to send chunk");
      return false;
    }
    return true;
  };

  static const char PREFIX[] = R"({"success":true,"content":")";
  bool sent = send_chunk(PREFIX, sizeof(PREFIX) - 1);

  size_t used = 0;
  while (sent) {
    size_t read_bytes = fread(in, 1, READ_CHUNK, file);
    if (read_bytes == 0) {
      // A read error leaves feof() clear and the position indeterminate, so
      // looping on feof() alone would spin here forever.
      if (ferror(file) != 0) {
        ESP_LOGE(TAG, "Failed to read '%s'", full_path.c_str());
      }
      break;
    }
    sent = json_escape_chunk(in, read_bytes, out, OUT_CHUNK, used, send_chunk);
    // Yield to prevent watchdog timeout on large files
    vTaskDelay(1);
  }

  if (sent && used > 0) {
    sent = send_chunk(out, used);
  }
  if (sent) {
    send_chunk("\"}", 2);
  }

  // Ends the chunked response either way; a truncated body is all a client can
  // be told once the first chunk has gone out.
  httpd_resp_send_chunk(req, nullptr, 0);

  fclose(file);
#else
  this->send_json_error_(request, "Not supported on this platform");
#endif
}

// POST /write?path=: the body already went into the file (handleBody); an empty
// body never reaches handleBody, so create the empty file here.
void WebFileBrowser::handle_write_request_(AsyncWebServerRequest *request) {
#ifdef USE_ESP32
  std::string error = this->write_error_;
  bool seen = this->write_seen_;
  this->write_error_.clear();
  this->write_seen_ = false;
  if (this->write_file_ != nullptr) {
    fclose(this->write_file_);
    this->write_file_ = nullptr;
  }
  if (!error.empty()) {
    this->send_json_error_(request, error);
    return;
  }
  if (!seen) {
    if (!request->hasParam("path")) {
      this->send_json_error_(request, "Missing path parameter");
      return;
    }
    std::string full_path = this->resolve_path_(request->getParam("path")->value());
    if (!this->is_valid_path_(full_path)) {
      this->send_json_error_(request, "Invalid path");
      return;
    }
    FILE *file = fopen(full_path.c_str(), "w");
    if (file == nullptr || fclose(file) != 0) {
      this->send_json_error_(request, "Failed to open file for writing");
      return;
    }
  }
  this->send_json_success_(request, "File written");
#else
  this->send_json_error_(request, "Not supported on this platform");
#endif
}

void WebFileBrowser::handle_delete_request_(AsyncWebServerRequest *request) {
#ifdef USE_ESP32
  if (!request->hasParam("path")) {
    this->send_json_error_(request, "Missing path parameter");
    return;
  }

  std::string path = request->getParam("path")->value();
  std::string full_path = this->resolve_path_(path);

  ESP_LOGD(TAG, "Delete request: path='%s' full_path='%s'", path.c_str(), full_path.c_str());

  if (!this->is_valid_path_(full_path)) {
    this->send_json_error_(request, "Invalid path");
    return;
  }

  // The mount point cannot be removed, and an empty path parameter resolves
  // straight to it — emptying the partition is not something to do by accident.
  if (full_path == this->get_base_path_()) {
    this->send_json_error_(request, "Cannot delete the mount root");
    return;
  }

  // Check if path exists
  struct stat st;
  if (stat(full_path.c_str(), &st) != 0) {
    ESP_LOGW(TAG, "File not found for deletion: '%s' errno=%d", full_path.c_str(), errno);
    this->send_json_error_(request, "File not found", 404);
    return;
  }

  // Use recursive deletion for directories, simple remove for files
  bool success;
  if (S_ISDIR(st.st_mode)) {
    success = this->delete_recursive_(full_path);
  } else {
    int result = remove(full_path.c_str());
    if (result != 0) {
      ESP_LOGE(TAG, "Failed to delete file '%s': errno=%d (%s)", full_path.c_str(), errno, strerror(errno));
    }
    success = (result == 0);
  }

  if (!success) {
    this->send_json_error_(request, "Failed to delete");
    return;
  }

  this->send_json_success_(request, "Deleted successfully");
#else
  this->send_json_error_(request, "Not supported on this platform");
#endif
}

void WebFileBrowser::handle_mkdir_request_(AsyncWebServerRequest *request) {
#ifdef USE_ESP32
  if (!request->hasParam("path")) {
    this->send_json_error_(request, "Missing path parameter");
    return;
  }

  std::string path = request->getParam("path")->value();
  std::string full_path = this->resolve_path_(path);

  if (!this->is_valid_path_(full_path)) {
    this->send_json_error_(request, "Invalid path");
    return;
  }

  if (mkdir(full_path.c_str(), 0755) != 0) {
    // Idempotent: mkdir() on the device is not recursive, so a folder upload
    // walks the chain level by level and re-creates parents it already made.
    struct stat st;
    if (stat(full_path.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
      this->send_json_success_(request, "Directory already exists");
      return;
    }
    this->send_json_error_(request, "Failed to create directory");
    return;
  }

  this->send_json_success_(request, "Directory created successfully");
#else
  this->send_json_error_(request, "Not supported on this platform");
#endif
}

void WebFileBrowser::handle_rename_request_(AsyncWebServerRequest *request) {
#ifdef USE_ESP32
  if (!request->hasParam("old_path")) {
    this->send_json_error_(request, "Missing old_path parameter");
    return;
  }

  if (!request->hasParam("new_path")) {
    this->send_json_error_(request, "Missing new_path parameter");
    return;
  }

  std::string old_path = request->getParam("old_path")->value();
  std::string new_path = request->getParam("new_path")->value();

  std::string full_old_path = this->resolve_path_(old_path);
  std::string full_new_path = this->resolve_path_(new_path);

  ESP_LOGD(TAG, "Rename request: '%s' -> '%s'", full_old_path.c_str(), full_new_path.c_str());

  if (!this->is_valid_path_(full_old_path)) {
    this->send_json_error_(request, "Invalid source path");
    return;
  }

  if (!this->is_valid_path_(full_new_path)) {
    this->send_json_error_(request, "Invalid destination path");
    return;
  }

  // Check if source exists
  struct stat st;
  if (stat(full_old_path.c_str(), &st) != 0) {
    this->send_json_error_(request, "Source file not found", 404);
    return;
  }

  // Check if destination already exists
  if (stat(full_new_path.c_str(), &st) == 0) {
    this->send_json_error_(request, "Destination already exists");
    return;
  }

  // Perform rename
  if (rename(full_old_path.c_str(), full_new_path.c_str()) != 0) {
    ESP_LOGE(TAG, "Failed to rename '%s' to '%s': errno=%d (%s)", full_old_path.c_str(), full_new_path.c_str(), errno,
             strerror(errno));
    this->send_json_error_(request, "Failed to rename");
    return;
  }

  this->send_json_success_(request, "Renamed successfully");
#else
  this->send_json_error_(request, "Not supported on this platform");
#endif
}

void WebFileBrowser::handle_copy_request_(AsyncWebServerRequest *request) {
#ifdef USE_ESP32
  if (!request->hasParam("old_path")) {
    this->send_json_error_(request, "Missing old_path parameter");
    return;
  }

  if (!request->hasParam("new_path")) {
    this->send_json_error_(request, "Missing new_path parameter");
    return;
  }

  std::string old_path = request->getParam("old_path")->value();
  std::string new_path = request->getParam("new_path")->value();

  std::string full_old_path = this->resolve_path_(old_path);
  std::string full_new_path = this->resolve_path_(new_path);

  ESP_LOGD(TAG, "Copy request: '%s' -> '%s'", full_old_path.c_str(), full_new_path.c_str());

  if (!this->is_valid_path_(full_old_path)) {
    this->send_json_error_(request, "Invalid source path");
    return;
  }

  if (!this->is_valid_path_(full_new_path)) {
    this->send_json_error_(request, "Invalid destination path");
    return;
  }

  // Check if source exists
  struct stat st;
  if (stat(full_old_path.c_str(), &st) != 0) {
    this->send_json_error_(request, "Source not found", 404);
    return;
  }

  // Checked before the destination: a recursive copy into its own subtree would
  // walk into what it is writing and fill the flash instead of terminating.
  if (full_new_path == full_old_path || full_new_path.starts_with(full_old_path + "/")) {
    this->send_json_error_(request, "Cannot copy into itself");
    return;
  }

  // Check if destination already exists
  struct stat dst_st;
  if (stat(full_new_path.c_str(), &dst_st) == 0) {
    this->send_json_error_(request, "Destination already exists");
    return;
  }

  bool success;
  if (S_ISDIR(st.st_mode)) {
    success = this->copy_recursive_(full_old_path, full_new_path);
  } else {
    success = this->copy_file_(full_old_path, full_new_path);
  }

  if (!success) {
    this->send_json_error_(request, "Failed to copy");
    return;
  }

  this->send_json_success_(request, "Copied successfully");
#else
  this->send_json_error_(request, "Not supported on this platform");
#endif
}

void WebFileBrowser::handle_info_request_(AsyncWebServerRequest *request) {
  auto info = this->storage_->get_storage_info();

  AsyncResponseStream *response = request->beginResponseStream("application/json");
  response->print("{");
  response->printf("\"valid\":%s,", info.valid ? "true" : "false");
  response->printf("\"total\":%zu,", info.total_bytes);
  response->printf("\"used\":%zu,", info.used_bytes);
  response->printf("\"free\":%zu,", info.free_bytes);
  response->printf(R"("filesystem":"%s")", this->storage_->get_filesystem_type());
  response->print("}");
  request->send(response);
}

// Helper methods

std::string WebFileBrowser::get_base_path_() const {
  // Get base path from storage component
  return this->storage_->get_base_path();
}

std::string WebFileBrowser::resolve_path_(const std::string &path) const {
  std::string base = this->get_base_path_();

  std::string full;
  if (path == base || path.starts_with(base + "/")) {
    // Already absolute and inside the mount. The boundary matters: without it a
    // sibling mount like "/littlefs-backup" would be passed through untouched
    // instead of being resolved beneath the mount as a client-supplied name.
    full = path;
  } else if (!path.empty() && path[0] == '/') {
    full = base + path;
  } else {
    full = base + "/" + path;
  }

  // Rebuild from segments, dropping empty ones (duplicate or trailing separators)
  // and "." ones, so that comparing two paths as strings means what it looks like.
  // Without this a source sent as "/dir/" or "/dir/." makes the copy subtree guard
  // test a prefix nothing can match, and the directory copy then walks into the
  // destination it just created — recursing until the flash is full.
  // ".." is deliberately NOT resolved here: is_valid_path_ rejects any path still
  // containing it, and quietly collapsing it would defeat that check.
  std::string out;
  out.reserve(full.size());
  size_t i = 0;
  while (i < full.size()) {
    while (i < full.size() && full[i] == '/') {
      i++;
    }
    size_t start = i;
    while (i < full.size() && full[i] != '/') {
      i++;
    }
    std::string segment = full.substr(start, i - start);
    if (segment.empty() || segment == ".") {
      continue;
    }
    out += '/';
    out += segment;
  }

  return out.empty() ? "/" : out;
}

bool WebFileBrowser::is_valid_path_(const std::string &path) const {
  std::string base = this->get_base_path_();

  // Path must be the mount itself or sit beneath it. A bare prefix test is not
  // enough: with base "/littlefs" it also accepts the sibling "/littlefs-backup",
  // which is a different VFS mount entirely.
  if (path != base && !path.starts_with(base + "/")) {
    return false;
  }

  // Check for directory traversal attempts
  if (path.find("..") != std::string::npos) {
    return false;
  }

  return true;
}

bool WebFileBrowser::delete_recursive_(const std::string &path, unsigned depth) {
#ifdef USE_ESP32
  if (depth > MAX_RECURSION_DEPTH) {
    ESP_LOGE(TAG, "Directory tree deeper than %u levels, refusing to delete '%s'", MAX_RECURSION_DEPTH, path.c_str());
    return false;
  }

  DIR *dir = opendir(path.c_str());
  if (!dir) {
    ESP_LOGE(TAG, "Failed to open directory for deletion: %s", path.c_str());
    return false;
  }

  struct dirent *entry;
  bool success = true;

  // Delete all contents first. Same readdir() caveat as copy_recursive_: only
  // errno separates end-of-directory from a read error, and this is also the
  // rollback path for a failed copy, where a silent partial delete would leave
  // the wreck it exists to clear away.
  errno = 0;
  while ((entry = readdir(dir)) != nullptr) {
    // Skip . and ..
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
      errno = 0;
      continue;
    }

    std::string entry_path = path + "/" + entry->d_name;
    struct stat st;

    if (stat(entry_path.c_str(), &st) == 0) {
      if (S_ISDIR(st.st_mode)) {
        // Recursively delete subdirectory
        if (!this->delete_recursive_(entry_path, depth + 1)) {
          ESP_LOGE(TAG, "Failed to delete subdirectory: %s", entry_path.c_str());
          success = false;
          break;
        }
      } else {
        // Delete file
        if (remove(entry_path.c_str()) != 0) {
          ESP_LOGE(TAG, "Failed to delete file: %s", entry_path.c_str());
          success = false;
          break;
        }
      }
    }
    errno = 0;
  }

  if (success && errno != 0) {
    ESP_LOGE(TAG, "Failed to read directory '%s' while deleting: errno=%d (%s)", path.c_str(), errno, strerror(errno));
    success = false;
  }

  closedir(dir);

  // Now delete the empty directory
  if (success) {
    if (rmdir(path.c_str()) != 0) {
      ESP_LOGE(TAG, "Failed to delete directory: %s", path.c_str());
      return false;
    }
  }

  return success;
#else
  return false;
#endif
}

bool WebFileBrowser::copy_file_(const std::string &src, const std::string &dst) {
#ifdef USE_ESP32
  FILE *in = fopen(src.c_str(), "rb");
  if (in == nullptr) {
    ESP_LOGE(TAG, "Failed to open source file for copy: %s", src.c_str());
    return false;
  }

  FILE *out = fopen(dst.c_str(), "wb");
  if (out == nullptr) {
    ESP_LOGE(TAG, "Failed to open destination file for copy: %s", dst.c_str());
    fclose(in);
    return false;
  }

  // Copy in chunks to avoid memory issues with large files
  static const size_t CHUNK_SIZE = 4096;  // 4KB chunks
  // Exceptions are disabled in this build, so make_unique's throwing new would
  // abort the firmware on a heap that cannot spare 4 KB — after the destination
  // was already created. Fail the copy and roll it back instead.
  auto buffer = std::unique_ptr<uint8_t[]>(new (std::nothrow) uint8_t[CHUNK_SIZE]);
  if (buffer == nullptr) {
    ESP_LOGE(TAG, "Out of memory copying '%s'", src.c_str());
    fclose(in);
    fclose(out);
    remove(dst.c_str());
    return false;
  }
  bool success = true;

  while (true) {
    size_t read_bytes = fread(buffer.get(), 1, CHUNK_SIZE, in);

    if (read_bytes > 0 && fwrite(buffer.get(), 1, read_bytes, out) != read_bytes) {
      ESP_LOGE(TAG, "Failed to write copy '%s': errno=%d (%s)", dst.c_str(), errno, strerror(errno));
      success = false;
      break;
    }

    if (read_bytes < CHUNK_SIZE) {
      if (ferror(in) != 0) {
        ESP_LOGE(TAG, "Failed to read '%s' while copying", src.c_str());
        success = false;
      }
      break;
    }

    // Yield to prevent watchdog timeout on large files
    vTaskDelay(1);
  }

  fclose(in);
  // close is part of the write: LittleFS commits the tail of the file cache and
  // the metadata entry here, so ENOSPC — the likely failure, since a copy doubles
  // the space used — surfaces at fclose rather than at any fwrite checked above.
  if (fclose(out) != 0) {
    ESP_LOGE(TAG, "Failed to flush copy '%s': errno=%d (%s)", dst.c_str(), errno, strerror(errno));
    success = false;
  }

  // Never leave a truncated copy behind
  if (!success) {
    remove(dst.c_str());
  }

  return success;
#else
  return false;
#endif
}

bool WebFileBrowser::copy_recursive_(const std::string &src, const std::string &dst, unsigned depth) {
#ifdef USE_ESP32
  if (depth > MAX_RECURSION_DEPTH) {
    ESP_LOGE(TAG, "Directory tree deeper than %u levels, refusing to copy '%s'", MAX_RECURSION_DEPTH, src.c_str());
    return false;
  }

  if (mkdir(dst.c_str(), 0755) != 0) {
    ESP_LOGE(TAG, "Failed to create directory for copy: %s", dst.c_str());
    return false;
  }

  DIR *dir = opendir(src.c_str());
  if (dir == nullptr) {
    ESP_LOGE(TAG, "Failed to open directory for copy: %s", src.c_str());
    rmdir(dst.c_str());
    return false;
  }

  struct dirent *entry;
  bool success = true;

  // readdir() returns nullptr both at end-of-directory and on a read error, and
  // only errno tells them apart. Without this a failed read would end the loop
  // quietly and report a half-copied tree as "Copied successfully".
  errno = 0;
  while ((entry = readdir(dir)) != nullptr) {
    // Skip . and ..
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
      errno = 0;
      continue;
    }

    std::string src_entry = src + "/" + entry->d_name;
    std::string dst_entry = dst + "/" + entry->d_name;
    struct stat st;

    if (stat(src_entry.c_str(), &st) != 0) {
      ESP_LOGE(TAG, "Failed to stat entry for copy: %s", src_entry.c_str());
      success = false;
      break;
    }

    if (S_ISDIR(st.st_mode)) {
      success = this->copy_recursive_(src_entry, dst_entry, depth + 1);
    } else {
      success = this->copy_file_(src_entry, dst_entry);
    }

    if (!success) {
      break;
    }

    // Small entries never reach the chunk yield in copy_file_
    vTaskDelay(1);
    errno = 0;
  }

  if (success && errno != 0) {
    ESP_LOGE(TAG, "Failed to read directory '%s' while copying: errno=%d (%s)", src.c_str(), errno, strerror(errno));
    success = false;
  }

  closedir(dir);

  // Roll the partial tree back: it is usually a full filesystem that stopped us,
  // and a half-copied tree would also block the retry as "already exists". The
  // rollback runs on top of the copy frames, so it continues this depth rather
  // than restarting the budget at zero.
  if (!success) {
    this->delete_recursive_(dst, depth);
  }

  return success;
#else
  return false;
#endif
}

void WebFileBrowser::discard_upload_() {
  if (this->upload_file_ != nullptr) {
    fclose(this->upload_file_);
    this->upload_file_ = nullptr;
  }

  // Only what this upload created: an overwrite already truncated the original
  // at fopen(), so removing it would turn a partial overwrite into no file.
  if (!this->upload_path_.empty()) {
    if (this->upload_created_) {
      remove(this->upload_path_.c_str());
    }
    this->upload_path_.clear();
  }

  this->upload_active_ = false;
  this->upload_written_ = 0;
  this->upload_created_ = false;
}

void WebFileBrowser::send_json_error_(AsyncWebServerRequest *request, const std::string &message, int code) {
  std::string json = R"({"success":false,"error":")" + this->json_escape_(message) + "\"}";
  request->send(code, "application/json", json.c_str());
}

void WebFileBrowser::send_json_success_(AsyncWebServerRequest *request, const std::string &message) {
  std::string json = R"({"success":true,"message":")" + this->json_escape_(message) + "\"}";
  request->send(200, "application/json", json.c_str());
}

std::string WebFileBrowser::json_escape_(const std::string &str) const {
  std::string escaped;
  escaped.reserve(str.length());

  char buf[JSON_ESCAPE_MAX];
  for (char c : str) {
    escaped.append(buf, json_escape_char(buf, c));
  }

  return escaped;
}

}  // namespace web_file_browser
}  // namespace esphome
