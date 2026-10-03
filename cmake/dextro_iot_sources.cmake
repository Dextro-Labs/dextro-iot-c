# Fontes e includes da lib, compartilhados pelo build host (CMake puro) e pelo
# componente ESP-IDF. Cada fase acrescenta seus módulos aqui, num lugar só.
set(DEXTRO_IOT_ROOT ${CMAKE_CURRENT_LIST_DIR}/..)
set(DEXTRO_IOT_COREMQTT ${DEXTRO_IOT_ROOT}/external/coreMQTT/source)

set(DEXTRO_IOT_CORE_SOURCES
    ${DEXTRO_IOT_ROOT}/src/common/iot_types.c
    ${DEXTRO_IOT_ROOT}/src/json/iot_json.c
    ${DEXTRO_IOT_ROOT}/src/json/jsmn_impl.c
    ${DEXTRO_IOT_ROOT}/src/session/iot_backoff.c
    ${DEXTRO_IOT_ROOT}/src/envelope/iot_envelope.c
    ${DEXTRO_IOT_ROOT}/src/client/iot_client.c
)

# coreMQTT v2.3.1 (MQTT 3.1.1), código de terceiro: compila sem as nossas
# exigências de aviso.
set(DEXTRO_IOT_THIRD_PARTY_SOURCES
    ${DEXTRO_IOT_ROOT}/src/json/jsmn_impl.c
    ${DEXTRO_IOT_COREMQTT}/core_mqtt.c
    ${DEXTRO_IOT_COREMQTT}/core_mqtt_serializer.c
    ${DEXTRO_IOT_COREMQTT}/core_mqtt_state.c
)
list(APPEND DEXTRO_IOT_CORE_SOURCES
    ${DEXTRO_IOT_COREMQTT}/core_mqtt.c
    ${DEXTRO_IOT_COREMQTT}/core_mqtt_serializer.c
    ${DEXTRO_IOT_COREMQTT}/core_mqtt_state.c
)

# Port ESP-IDF (transporte TLS por esp-tls, relógio, RNG, log): só no
# componente IDF.
set(DEXTRO_IOT_ESP_SOURCES ${DEXTRO_IOT_ROOT}/port/esp-idf/iot_port_esp.c)

set(DEXTRO_IOT_PUBLIC_INCLUDES
    ${DEXTRO_IOT_ROOT}/include
    ${DEXTRO_IOT_ROOT}/external/jsmn
    ${DEXTRO_IOT_COREMQTT}/include
    ${DEXTRO_IOT_COREMQTT}/interface
    # core_mqtt_config.h: o coreMQTT o inclui pelo nome
    ${DEXTRO_IOT_ROOT}/src/mqtt
)

set(DEXTRO_IOT_PRIVATE_INCLUDES
    ${DEXTRO_IOT_ROOT}/src
)

# O jsmn é header-only e muda de comportamento com JSMN_STRICT: a definição
# precisa valer igual na lib e em quem inclui iot_json.h.
set(DEXTRO_IOT_PUBLIC_DEFINITIONS JSMN_STRICT)
