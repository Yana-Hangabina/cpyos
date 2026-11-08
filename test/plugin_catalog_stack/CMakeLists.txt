add_executable(PluginCatalogStackTest
  FirmwareIdentityTest.cpp
)

target_link_libraries(PluginCatalogStackTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(PluginCatalogStackTest)
