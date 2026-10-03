cmake_minimum_required(VERSION 3.16)

include(${CMAKE_CURRENT_LIST_DIR}/cmake/dextro_iot_sources.cmake)

# Consumido como componente ESP-IDF (submódulo em components/ do firmware).
if(ESP_PLATFORM)
    idf_component_register(
        SRCS ${DEXTRO_IOT_CORE_SOURCES} ${DEXTRO_IOT_ESP_SOURCES}
        INCLUDE_DIRS ${DEXTRO_IOT_PUBLIC_INCLUDES} port/esp-idf/include
        PRIV_INCLUDE_DIRS ${DEXTRO_IOT_PRIVATE_INCLUDES}
        PRIV_REQUIRES esp-tls esp_timer esp_hw_support log lwip
    )
    target_compile_definitions(${COMPONENT_LIB} PUBLIC ${DEXTRO_IOT_PUBLIC_DEFINITIONS})
    return()
endif()

project(dextro-iot-c VERSION 0.1.0 LANGUAGES C)

# Testes e exemplos ficam desligados por padrão: quem consome a lib por
# add_subdirectory não precisa contornar nada.
option(DEXTRO_IOT_HOST_BUILD "Compila o port POSIX (host)" ${PROJECT_IS_TOP_LEVEL})
option(DEXTRO_IOT_BUILD_TESTS "Compila os testes unitários (Unity + ctest)" OFF)
option(DEXTRO_IOT_BUILD_EXAMPLES "Compila os exemplos" OFF)

add_library(dextro_iot_c STATIC ${DEXTRO_IOT_CORE_SOURCES})
add_library(dextro::iot_c ALIAS dextro_iot_c)
target_include_directories(dextro_iot_c
    PUBLIC ${DEXTRO_IOT_PUBLIC_INCLUDES}
    PRIVATE ${DEXTRO_IOT_PRIVATE_INCLUDES}
)
target_compile_definitions(dextro_iot_c PUBLIC ${DEXTRO_IOT_PUBLIC_DEFINITIONS})
target_compile_features(dextro_iot_c PUBLIC c_std_11)
set_target_properties(dextro_iot_c PROPERTIES C_EXTENSIONS OFF)

if(PROJECT_IS_TOP_LEVEL)
    target_compile_options(dextro_iot_c PRIVATE -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror)
    # jsmn e coreMQTT são código de terceiro: não passam no -Wconversion e não
    # são nossos para corrigir.
    set_source_files_properties(${DEXTRO_IOT_THIRD_PARTY_SOURCES}
        PROPERTIES COMPILE_OPTIONS "-w")
endif()

if(DEXTRO_IOT_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
