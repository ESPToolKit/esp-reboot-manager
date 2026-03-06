add_library(esp_reboot_manager_core STATIC
    ${PROJECT_SOURCE_DIR}/src/esp_reboot_manager/reboot_manager.cpp
)

target_include_directories(esp_reboot_manager_core
    PUBLIC
        ${PROJECT_SOURCE_DIR}/src
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/stubs
)

target_compile_features(esp_reboot_manager_core PUBLIC cxx_std_17)

add_executable(esp_reboot_manager_tests
    test_esp_reboot_manager/test_esp_reboot_manager.cpp
    reboot_manager_test_stubs.cpp
)

target_include_directories(esp_reboot_manager_tests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/stubs
)

target_link_libraries(esp_reboot_manager_tests
    PRIVATE
        esp_reboot_manager_core
)

target_compile_features(esp_reboot_manager_tests PRIVATE cxx_std_17)

add_test(NAME esp_reboot_manager_tests COMMAND esp_reboot_manager_tests)
