#ifndef DEXTRO_IOT_CORE_MQTT_CONFIG_H
#define DEXTRO_IOT_CORE_MQTT_CONFIG_H

/* Configuração do coreMQTT para a dextro-iot-c. O transporte da lib não
 * bloqueia: recv devolve 0 sem dados, então o loop do coreMQTT volta rápido
 * e quem dita o ritmo é o iot_step da aplicação. */

#include <stddef.h>
#include <stdint.h>

/* coreMQTT espera a struct do transporte definida pelo usuário */
struct NetworkContext;

#define MQTT_RECV_POLLING_TIMEOUT_MS 0U
#define MQTT_SEND_TIMEOUT_MS 5000U
#define MQTT_PINGRESP_TIMEOUT_MS 5000U
/* Sem retentativa de ACK infinita: queda é tratada pela sessão. */
#define MQTT_MAX_CONNACK_RECEIVE_RETRY_COUNT 0U

#define LogError(message)
#define LogWarn(message)
#define LogInfo(message)
#define LogDebug(message)

#endif /* DEXTRO_IOT_CORE_MQTT_CONFIG_H */
