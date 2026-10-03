# Testes unitários no host: Unity (submódulo) + ctest, um executável por suíte.
add_library(unity STATIC ${PROJECT_SOURCE_DIR}/external/unity/src/unity.c)
target_include_directories(unity SYSTEM PUBLIC ${PROJECT_SOURCE_DIR}/external/unity/src)
target_compile_definitions(unity PUBLIC UNITY_INCLUDE_DOUBLE UNITY_SUPPORT_64 UNITY_INCLUDE_PRINT_FORMATTED)

function(dextro_iot_add_test name)
    add_executable(${name} ${ARGN})
    target_link_libraries(${name} PRIVATE dextro::iot_c unity)
    target_include_directories(${name} PRIVATE ${PROJECT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_options(${name} PRIVATE -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES LABELS unit)
endfunction()

dextro_iot_add_test(test_json unit/test_json.c)
