#ifndef DEXTRO_IOT_IOT_CONFIG_H
#define DEXTRO_IOT_IOT_CONFIG_H

/*
 * Limites de compilação da lib. Todos podem ser sobrescritos pelo projeto,
 * por -D na linha de compilação ou num header próprio apontado por
 * IOT_USER_CONFIG_FILE (ex.: -DIOT_USER_CONFIG_FILE="\"iot_user_config.h\"").
 *
 * Nada aqui é alocado dinamicamente: os limites definem o tamanho das tabelas
 * estáticas e o que é recusado no fio (spec §12, perfil MCU).
 */

#ifdef IOT_USER_CONFIG_FILE
#include IOT_USER_CONFIG_FILE
#endif

/* Maior mensagem MQTT aceita/emitida (envelope + payload). Acima disso o
 * request recebe rescode 413. O backend aceita até 256 KiB; o MCU escolhe. */
#ifndef IOT_MAX_PAYLOAD
#define IOT_MAX_PAYLOAD 8192u
#endif

/* Tokens do parser JSON por mensagem (cada chave, valor, objeto e array
 * conta um). */
#ifndef IOT_JSON_MAX_TOKENS
#define IOT_JSON_MAX_TOKENS 256u
#endif

/* Profundidade máxima de aninhamento de objetos/arrays, no parser e no
 * escritor. */
#ifndef IOT_JSON_MAX_DEPTH
#define IOT_JSON_MAX_DEPTH 16u
#endif

#endif /* DEXTRO_IOT_IOT_CONFIG_H */
