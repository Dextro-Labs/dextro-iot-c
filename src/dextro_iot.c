#include <dextro_iot.h>
#include <string.h>

void iot_init(iot_context_t* ctx, const iot_config_t* config) {
    memset(ctx, 0, sizeof(iot_context_t));
    memcpy(&ctx->config, config, sizeof(iot_config_t));
    ctx->socket_fd = -1;
}

int iot_connect(iot_context_t* ctx, const char* host, uint16_t port) {
    ctx->socket_fd = ctx->config.net.connect(host, port);
    if (ctx->socket_fd >= 0) {
        ctx->is_connected = true;
        ctx->last_heartbeat = ctx->config.os.get_time_ms();
        return 0;
    }
    return -1;
}

void iot_process(iot_context_t* ctx) {
    if (!ctx->is_connected) return;

    uint64_t now = ctx->config.os.get_time_ms();

    // Heartbeat síncrono simplificado
    if ((now - ctx->last_heartbeat) > 30000) {
        // Envia heartbeat via MQTT
        ctx->last_heartbeat = now;
    }

    // Pump de rede
    uint8_t buffer[256];
    int bytes = ctx->config.net.recv(ctx->socket_fd, buffer, sizeof(buffer), 10);
    if (bytes > 0) {
        // Parser MQTT aqui
    }
}

void iot_push(iot_context_t* ctx, const char* procedure_name, const uint8_t* payload, size_t len) {
    // Implementar publicação no tópico dextro/<id>/req/<procedure_name>
}

void iot_inbox_push(iot_context_t* ctx, const char* type, const uint8_t* data, size_t len) {
    // Implementar lógica Mailbox
}
