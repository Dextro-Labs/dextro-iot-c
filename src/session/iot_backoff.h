#ifndef DEXTRO_IOT_IOT_BACKOFF_H
#define DEXTRO_IOT_IOT_BACKOFF_H

#include <stdint.h>

/* Backoff exponencial com jitter (spec §8): a espera sorteada fica em
 * [atual/2, atual] e o atual dobra até o teto. `rnd` é um número aleatório
 * qualquer (do RNG do port). Devolve a espera; atualiza *current. */
uint32_t iot_backoff_next(uint32_t *current, uint32_t min_ms, uint32_t max_ms, uint32_t rnd);

#endif
