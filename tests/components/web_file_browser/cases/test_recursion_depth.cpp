#include "common.h"
#include <cstring>

namespace esphome::web_file_browser::testing {

// MAX_RECURSION_DEPTH is 3 and counts the directory named in the request as 0, so four nested
// levels are the deepest delete and copy accept.
static constexpr unsigned ACCEPTED_LEVELS = 4;
static constexpr unsigned REFUSED_LEVELS = 5;

class Depth : public Browser {
 protected:
  // The walks are protected, and the handlers that reach them are ESP32-only.
  class Probe : public WebFileBrowser {
   public:
    using WebFileBrowser::WebFileBrowser;
    using WebFileBrowser::copy_recursive_;
    using WebFileBrowser::delete_recursive_;
    using WebFileBrowser::tree_too_deep_;
  };

  void TearDown() override {
    remove_tree(this->level(1));
    remove_tree(this->copy_root());
    Browser::TearDown();
  }

  // Where the copy cases write, kept apart from the tree build_tree() makes.
  std::string copy_root() const { return this->base_path() + "/C0"; }

  // The n-th level of what copy_root() holds after copying level(1) into it.
  std::string copied(unsigned levels) const {
    std::string path = this->copy_root();
    for (unsigned i = 1; i < levels; i++)
      path += "/L" + std::to_string(i);
    return path;
  }

  // The path of the n-th level of the tree build_tree() makes, 1 being its root.
  std::string level(unsigned levels) const {
    std::string path = this->base_path();
    for (unsigned i = 0; i < levels; i++)
      path += "/L" + std::to_string(i);
    return path;
  }

  // One directory and one file per level, so every level has something to remove.
  void build_tree(unsigned levels) {
    for (unsigned i = 1; i <= levels; i++) {
      const std::string dir = this->level(i);
      ASSERT_EQ(mkdir(dir.c_str(), 0755), 0) << dir;
      FILE *file = fopen((dir + "/f.txt").c_str(), "wb");
      ASSERT_NE(file, nullptr) << dir;
      ASSERT_EQ(fputc('q', file), 'q') << dir;
      ASSERT_EQ(fclose(file), 0) << dir;
    }
  }

  static void remove_tree(const std::string &path) {
    DIR *dir = opendir(path.c_str());
    if (dir == nullptr)
      return;
    while (struct dirent *entry = readdir(dir)) {
      if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
        continue;
      const std::string child = path + "/" + entry->d_name;
      struct stat st;
      if (stat(child.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
        remove_tree(child);
      } else {
        remove(child.c_str());
      }
    }
    closedir(dir);
    rmdir(path.c_str());
  }

  Probe probe{&this->base, &this->storage};
};

TEST_F(Depth, RefusesATreeDeeperThanTheCapWithoutTouchingIt) {
  this->build_tree(REFUSED_LEVELS);
  EXPECT_TRUE(this->probe.tree_too_deep_(this->level(1)));
  // This walk is the whole of the guarantee: a delete that it refuses never starts, so the
  // tree is still every bit of itself.
  for (unsigned i = 1; i <= REFUSED_LEVELS; i++) {
    EXPECT_TRUE(this->exists(this->level(i))) << i;
    EXPECT_TRUE(this->exists(this->level(i) + "/f.txt")) << i;
  }
}

TEST_F(Depth, DeletesATreeAtTheCap) {
  this->build_tree(ACCEPTED_LEVELS);
  EXPECT_FALSE(this->probe.tree_too_deep_(this->level(1)));
  EXPECT_TRUE(this->probe.delete_recursive_(this->level(1)));
  EXPECT_FALSE(this->exists(this->level(1)));
}

TEST_F(Depth, CopiesATreeAtTheCap) {
  this->build_tree(ACCEPTED_LEVELS);
  EXPECT_TRUE(this->probe.copy_recursive_(this->level(1), this->copy_root()));
  EXPECT_TRUE(this->exists(this->copied(ACCEPTED_LEVELS)));
  EXPECT_TRUE(this->exists(this->copied(ACCEPTED_LEVELS) + "/f.txt"));
}

TEST_F(Depth, RefusesToCopyPastTheCap) {
  // copy has no pre-walk of its own, so the bound stops it partway — and then every unwinding
  // level rolls its own destination back, so nothing of the partial copy survives at all.
  this->build_tree(REFUSED_LEVELS);
  EXPECT_FALSE(this->probe.copy_recursive_(this->level(1), this->copy_root()));
  EXPECT_TRUE(this->exists(this->level(REFUSED_LEVELS) + "/f.txt"));
  EXPECT_FALSE(this->exists(this->copy_root()));
}

TEST_F(Depth, RefusesToDeletePastTheCapWithoutThePreWalk) {
  // delete_recursive_ carries the same bound, so a caller that skips tree_too_deep_ stops as
  // well — it just stops partway, which is why the pre-walk exists.
  this->build_tree(REFUSED_LEVELS);
  EXPECT_FALSE(this->probe.delete_recursive_(this->level(1)));
  EXPECT_TRUE(this->exists(this->level(REFUSED_LEVELS)));
}

}  // namespace esphome::web_file_browser::testing
