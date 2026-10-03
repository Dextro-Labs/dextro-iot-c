# Fontes e includes da lib, compartilhados pelo build host (CMake puro) e pelo
# componente ESP-IDF. Cada fase acrescenta seus módulos aqui, num lugar só.
set(DEXTRO_IOT_ROOT ${CMAKE_CURRENT_LIST_DIR}/..)

set(DEXTRO_IOT_CORE_SOURCES
    ${DEXTRO_IOT_ROOT}/src/common/iot_types.c
    ${DEXTRO_IOT_ROOT}/src/json/iot_json.c
    ${DEXTRO_IOT_ROOT}/src/json/jsmn_impl.c
)

set(DEXTRO_IOT_PUBLIC_INCLUDES
    ${DEXTRO_IOT_ROOT}/include
    ${DEXTRO_IOT_ROOT}/external/jsmn
)

set(DEXTRO_IOT_PRIVATE_INCLUDES
    ${DEXTRO_IOT_ROOT}/src
)

# O jsmn é header-only e muda de comportamento com JSMN_STRICT: a definição
# precisa valer igual na lib e em quem inclui iot_json.h.
set(DEXTRO_IOT_PUBLIC_DEFINITIONS JSMN_STRICT)
