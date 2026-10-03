# dextro-iot-c

Cliente **C11** do protocolo Dextro IoT para microcontroladores (ESP32, STM32),
sem `malloc`, com buffers e tabelas estáticas. É o binding MCU da spec (§14):
fala com o backend `dextrolabs-device` por MQTT 3.1.1 + mTLS.

- **Protocolo implementado:** [`docs/PROTOCOL.md`](docs/PROTOCOL.md) (v0.5, com
  selo da versão do backend).
- **Plano de construção:** seção [Roteiro](#roteiro).

## Estado

| Fase | Conteúdo | Situação |
|---|---|---|
| 1 | Esqueleto, build host + ESP-IDF, CI, JSON estático | **este PR** |
| 2 | Envelope, dispatch de device-procedures, ops do core | a fazer |
| 3 | Sessão MQTT (coreMQTT), reconexão, presença/LWT, keepalive, port POSIX | a fazer |
| 4 | Outbox, inbox, events, settings, OTA; device POSIX e integração com o backend real | a fazer |
| 5 | Port ESP-IDF (esp-tls, NVS) e exemplo na placa | a fazer |
| 6 | `certificate-request` / `certificate-install` (CSR gerada no device) | a fazer |

A API antiga (`iot_connect`/`iot_push`) foi removida. Ela não falava MQTT e
publicava em tópicos que não existem no backend.

## Estrutura

```
include/dextro-iot/   API pública (iot_*.h)
src/                  implementação, um diretório por módulo (json/, common/, …)
port/                 vtables preenchidas por plataforma (posix/, esp-idf/)   [fases 3 e 5]
external/             submódulos: coreMQTT v2.3.1 (MQTT 3.1.1), jsmn v1.1.0, Unity v2.6.1
examples/             device POSIX e app ESP32 mínimo                         [fases 4 e 5]
tests/unit/           Unity + ctest, no host
cmake/                lista de fontes compartilhada entre host e ESP-IDF
docs/                 PROTOCOL.md
```

## Build e testes (host)

```bash
git submodule update --init --recursive
cmake -S . -B build -G Ninja -DDEXTRO_IOT_BUILD_TESTS=ON
cmake --build build -j4
ctest --test-dir build -j2 --output-on-failure
```

| Opção | Padrão | Para quê |
|---|---|---|
| `DEXTRO_IOT_BUILD_TESTS` | OFF | testes unitários (Unity) |
| `DEXTRO_IOT_BUILD_EXAMPLES` | OFF | exemplos |
| `DEXTRO_IOT_HOST_BUILD` | ON se for o projeto raiz | port POSIX |

Os testes e exemplos ficam desligados por padrão: quem consome por
`add_subdirectory` recebe só o alvo `dextro::iot_c`.

## Uso como componente ESP-IDF

Coloque o repositório como submódulo em `components/dextro-iot-c` do firmware.
O `CMakeLists.txt` raiz detecta `ESP_PLATFORM` e registra o componente.
Testado com ESP-IDF v5.5.3.

```cmake
idf_component_register(SRCS "main.c" REQUIRES dextro-iot-c)
```

## Limites de compilação

Ficam em [`include/dextro-iot/iot_config.h`](include/dextro-iot/iot_config.h).
Cada um pode ser sobrescrito por `-D` ou por um header próprio
(`-DIOT_USER_CONFIG_FILE="\"meu_config.h\""`).

| Macro | Padrão | Efeito |
|---|---|---|
| `IOT_MAX_PAYLOAD` | 8192 | maior mensagem aceita; acima disso, rescode 413 |
| `IOT_JSON_MAX_TOKENS` | 256 | tokens JSON por mensagem |
| `IOT_JSON_MAX_DEPTH` | 16 | aninhamento máximo (parser e escritor) |

## JSON (`iot_json.h`)

O leitor é tokenizado (jsmn) e valida a gramática inteira antes de liberar o
documento: separadores, primitivos, strings sem controle cru, profundidade e
limites. Payload malformado retorna `IOT_ERR_PARSE` e nunca derruba o device.
O escritor monta direto num buffer do chamador. Qualquer estouro ou uso fora de
ordem fica registrado e aparece uma vez só, no `iot_jw_finish`.

```c
iot_json_tok_t toks[IOT_JSON_MAX_TOKENS];
iot_json_doc_t doc;
if (iot_json_parse(&doc, msg, len, toks, IOT_JSON_MAX_TOKENS) == IOT_OK) {
    iot_json_ref_t op = iot_json_get(&doc, iot_json_root(&doc), "op");
    if (iot_json_str_eq(&doc, op, "get-status")) { /* ... */ }
}

char out[256];
iot_json_writer_t w;
iot_jw_init(&w, out, sizeof(out));
iot_jw_obj_begin(&w);
iot_jw_key(&w, "uptimeS");
iot_jw_i64(&w, 42);
iot_jw_obj_end(&w);
if (iot_jw_finish(&w, NULL) != IOT_OK) { /* não coube */ }
```

## Roteiro

O desenho completo das fases 2 a 6 está na proposta aprovada:

- **API:** `iot_init` / `iot_step` (não bloqueante) / `iot_stop`.
  `iot_call_service` e `iot_inbox_write` funcionam por callback. Procedures
  ficam numa tabela estática.
- **Ports:** transporte, relógio, RNG, storage, crypto e log, cada um por vtable.
- **Testes:** a sessão é exercitada por um transporte falso com roteiro de
  pacotes, com quedas e conexão meio-aberta. A integração usa um device POSIX
  contra o `dextrolabs-device` real.
