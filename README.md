# Dextro IoT - Embedded C Library

Biblioteca em ANSI C11 focada em microcontroladores (ESP32, STM32) rodando FreeRTOS. Desenvolvida com foco em segurança de memória via **Alocação Estática**.

## 🚀 Filosofia do Protocolo

- **Stateless & RAM Optimized**: Sem uso de `malloc`/`free`.
- **Mailbox Pattern**: Garantia de entrega e deduplicação delegada ao componente NestJS.
- **RPC Mandatory**: Implementação obrigatória de `list-procedures`, `device-status` e `outbox-notify`.

## 🛠️ Camadas de Abstração (HAL/OSAL)

O core é agnóstico. O desenvolvedor deve preencher as vtables de I/O e OS:
- **`iot_os_api_t`**: Mutexes, Delays e Timestamps (Static).
- **`iot_net_api_t`**: Sockets TCP/TLS (mbedTLS/LWIP).

## 💻 Exemplo de Uso

### 1. Preenchimento da HAL (FreeRTOS Example)

```c
void meu_handler(const char* name, const uint8_t* payload, size_t len, const iot_request_context_t* req_ctx) {
    if (strcmp(name, "open-door") == 0) {
        printf("Abrindo porta. CorrID: %s\n", req_ctx->correlation_id);
        if (req_ctx->metadata_len > 0) {
            printf("Metadata recebido: %.*s\n", (int)req_ctx->metadata_len, req_ctx->metadata);
        }
    }
}

iot_os_api_t my_os = {
    .mutex_create_static = xSemaphoreCreateBinaryStatic,
    .mutex_lock = xSemaphoreTake,
    .delay_ms = vTaskDelay,
    .get_time_ms = xTaskGetTickCount
};

iot_config_t config = {
    .client_id = "LOCKER-001",
    .os = my_os,
    .net = my_net_provider,
    .on_procedure = meu_handler
};
```

### 2. Inicialização e Loop Principal

```c
iot_context_t iot_ctx;
iot_init(&iot_ctx, &config);

if (iot_connect(&iot_ctx, "mqtt.dextro.com", 1883) == 0) {
    while(1) {
        iot_process(&iot_ctx); // Bomba de mensagens
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
```

## 🏗️ Build

Pode ser compilada como biblioteca estática via `gcc` ou integrada no `CMake` do ESP-IDF / STM32Cube.
