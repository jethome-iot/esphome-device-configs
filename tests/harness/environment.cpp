#include <gtest/gtest.h>
#include <new>
#include "esphome/core/application.h"

namespace esphome::automations::testing {

// The generated setup() would construct App in place before anything touches it; main.cpp
// never runs that setup, so the scheduler and the entity lists start as raw storage.
class AppEnvironment : public ::testing::Environment {
 public:
  void SetUp() override { new (&App) Application(); }
};

static ::testing::Environment *const APP_ENVIRONMENT = ::testing::AddGlobalTestEnvironment(new AppEnvironment());

}  // namespace esphome::automations::testing
