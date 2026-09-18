#pragma once
#include <gtest/gtest.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>
#include "esphome/components/dir_storage/dir_storage.h"
#include "esphome/components/web_file_browser/web_file_browser.h"

namespace esphome::web_file_browser::testing {

// A mount that answers /info with numbers a test can tell apart; the dir_storage backend
// reports the abstract class's empty StorageInfo and calls itself "Directory".
class TestStorage : public dir_storage::DirStorage {
 public:
  filesystem_storage_abstract::StorageInfo get_storage_info() const override { return this->info; }
  const char *get_filesystem_type() const override { return "TestFS"; }

  filesystem_storage_abstract::StorageInfo info{131072, 4096, 126976, true};
};

// One answered request: whether a handler claimed it, and what it said.
struct Reply {
  bool claimed{false};
  int code{0};
  std::string type;
  std::string body;
  int responses{0};
};

// The browser over a temporary directory, reached through the harness's web_server_base
// stand-in exactly as a request from the network would reach it.
class Browser : public ::testing::Test {
 protected:
  void SetUp() override {
    mkdir(".storage", 0755);
    char folder[] = ".storage/XXXXXX";
    ASSERT_NE(mkdtemp(folder), nullptr);
    // Absolute: resolve_path_ builds absolute paths and is_valid_path_ compares them against
    // the mount, so a relative mount would refuse every path. No real mount is relative.
    char cwd[4096];
    ASSERT_NE(getcwd(cwd, sizeof(cwd)), nullptr);
    this->storage.set_base_path(std::string(cwd) + "/" + folder);
    this->storage.setup();
    ASSERT_TRUE(this->storage.is_mounted());
    this->browser = std::make_unique<WebFileBrowser>(&this->base, &this->storage);
    this->browser->set_url_prefix("/files");
    this->browser->setup();
  }

  void TearDown() override {
    for (const std::string &name : this->files())
      remove((this->base_path() + "/" + name).c_str());
    rmdir(this->base_path().c_str());
  }

  const std::string &base_path() const { return this->storage.get_base_path(); }

  std::vector<std::string> files() const {
    std::vector<std::string> names;
    DIR *dir = opendir(this->base_path().c_str());
    if (dir == nullptr)
      return names;
    while (struct dirent *entry = readdir(dir)) {
      if (entry->d_name[0] != '.')
        names.emplace_back(entry->d_name);
    }
    closedir(dir);
    return names;
  }

  bool exists(const std::string &path) const {
    struct stat st;
    return stat(path.c_str(), &st) == 0;
  }

  // A file already on the mount, for a case about what must not happen to it.
  void write_file(const std::string &name, const std::string &content) const {
    FILE *file = fopen((this->base_path() + "/" + name).c_str(), "wb");
    ASSERT_NE(file, nullptr);
    ASSERT_EQ(fwrite(content.data(), 1, content.size(), file), content.size());
    ASSERT_EQ(fclose(file), 0);
  }

  std::string contents(const std::string &path) const {
    std::string out;
    FILE *file = fopen(path.c_str(), "rb");
    if (file == nullptr)
      return out;
    char buf[256];
    while (size_t len = fread(buf, 1, sizeof(buf), file))
      out.append(buf, len);
    fclose(file);
    return out;
  }

  static Reply answer(AsyncWebServerRequest &request, bool claimed) {
    Reply reply;
    reply.claimed = claimed;
    reply.code = request.response_code;
    reply.type = request.response_type;
    reply.body = request.response_body;
    reply.responses = request.responses;
    return reply;
  }

  Reply call(http_method method, const std::string &target, const std::string &body = "",
             const char *origin = nullptr) {
    AsyncWebServerRequest request(method, target, body);
    this->add_origin_(request, origin);
    return answer(request, this->base.get_server()->dispatch(request));
  }
  Reply get(const std::string &target) { return this->call(HTTP_GET, target); }
  Reply post(const std::string &target) { return this->call(HTTP_POST, target); }

  // The multipart reader's one file part, handed over the way web_server_idf hands it over.
  Reply upload(const std::string &target, const std::string &filename, const std::string &content,
               const char *origin = nullptr) {
    AsyncWebServerRequest request(HTTP_POST, target, "", "multipart/form-data");
    request.set_upload(filename, content);
    this->add_origin_(request, origin);
    return answer(request, this->base.get_server()->dispatch(request));
  }

  // The Host every case is addressed to; a request from a page on another site says so by
  // carrying an Origin that is not this.
  static constexpr const char *HOST = "device.local";

  static void add_origin_(AsyncWebServerRequest &request, const char *origin) {
    request.set_header("Host", HOST);
    if (origin != nullptr)
      request.set_header("Origin", origin);
  }

  TestStorage storage;
  web_server_base::WebServerBase base;
  std::unique_ptr<WebFileBrowser> browser;
};

}  // namespace esphome::web_file_browser::testing
