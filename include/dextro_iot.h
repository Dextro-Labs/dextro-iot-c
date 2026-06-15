#ifndef DEXTRO_IOT_C_H
#define DEXTRO_IOT_C_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/**
 * @brief OSAL para C (Static Allocation focus).
 */
typedef struct {
    void* (*mutex_create_static)(void* static_mem);
    bool (*mutex_lock)(void* mutex, uint32_t timeout_ms);
    void (*mutex_unlock)(void* mutex);
    
    void (*delay_ms)(uint32_t ms);
    uint64_t (*get_time_ms)(void);
} iot_os_api_t;

/**
 * @brief HAL de Rede para C.
 */
typedef struct {
    int (*connect)(const char* host, uint16_t port);
    void (*disconnect)(int socket_fd);
    int (*send)(int socket_fd, const uint8_t* data, size_t len);
    int (*recv)(int socket_fd, uint8_t* buf, size_t len, uint32_t timeout_ms);
} iot_net_api_t;

typedef struct {
    const char* client_id;
    iot_os_api_t os;
    iot_net_api_t net;
    
    // Callbacks da aplicação
    void (*on_procedure)(const char* name, const uint8_t* payload, size_t len);
} iot_config_t;

/**
 * @brief Contexto do cliente (sem alocação dinâmica).
 */
typedef struct {
    iot_config_t config;
    int socket_fd;
    bool is_connected;
    uint64_t last_heartbeat;
} iot_context_t;

// API Pública
void iot_init(iot_context_t* ctx, const iot_config_t* config);
int iot_connect(iot_context_t* ctx, const char* host, uint16_t port);
void iot_process(iot_context_t* ctx);
void iot_send_event(iot_context_t* ctx, const char* type, const uint8_t* data, size_t len);

#endif // DEXTRO_IOT_C_H
