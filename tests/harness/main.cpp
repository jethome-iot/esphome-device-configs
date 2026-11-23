#include <gtest/gtest.h>

#include <new>
#include <tuple>
#include <type_traits>

#include "esphome/components/logger/logger.h"

/*
This special main.cpp replaces the default one.
It will run all the Google Tests found in all compiled cpp files and then exit with the result
See README.md for more information
*/

// Auto generated code by esphome
// ========== AUTO GENERATED INCLUDE BLOCK BEGIN ===========
// ========== AUTO GENERATED INCLUDE BLOCK END ==========="

void original_setup() {
  // This function won't be run.

  // ========== AUTO GENERATED CODE BEGIN ===========
  // =========== AUTO GENERATED CODE END ============
}

void setup() {
  // Log functions call global_logger->log_vprintf_() without a null check,
  // so we must set up a Logger before any test that triggers logging.
  static esphome::logger::Logger test_logger(0);
  test_logger.set_log_level(ESPHOME_LOG_LEVEL);
  test_logger.pre_setup();

#ifdef USE_CONTROLLER_REGISTRY
  // A state update calls the controllers in the storage original_setup() would construct them in.
  // Construct them, never set up; api::global_api_server stays the cases' to install.
  std::apply([](auto *...controller) { (new (controller) std::remove_pointer_t<decltype(controller)>(), ...); },
             esphome_controllers());
#ifdef USE_API
  esphome::api::global_api_server = nullptr;
#endif
#endif

  ::testing::InitGoogleTest();
  int exit_code = RUN_ALL_TESTS();
  exit(exit_code);
}

void loop() {}
