# Protocolo Dextro IoT: o que o dextro-iot-c implementa

> **Selo:** protocolo **v0.5**. As fontes são:
> - a spec `dextro-iot/dextro-iot-spec.md` v0.5;
> - o backend `Dextro-Labs/dextrolabs-device` em `origin/main` `9aeacf5` (01/10/2026): `SPEC.md`, `docs/contracts.md` (Parte A), `docs/emqx.md`, `crates/atrio-core/src/{envelope,topics,acl,rescode}.rs`.
>
> Onde a spec e o backend divergem, **vale o backend**: é ele que roda em produção.
> Este arquivo é a cópia vendorizada para o binding C (spec §14). Ao atualizar, troque o selo.

## 1. Modelo

São dois eixos e quatro primitivos, todos sobre o mesmo transporte RPC:

| Primitivo | Sentido | Realizado por |
|---|---|---|
| device-procedure | backend → device | op em `atrio/{sn}/procedure` |
| remote-procedure ("service") | device → backend | op em `atrio/{sn}/service` |
| inbox (confiável) | device → backend | service `inbox-write`, idempotente |
| outbox (confiável) | backend → device | aviso `outbox-notification` + `outbox-read`/`outbox-ack` |

O RPC é transiente: tem TTL e timeout, e comando velho não é executado. A
mailbox é durável e deduplicada.

## 2. Envelope

Tudo é JSON UTF-8.

```jsonc
// request
{ "metadata": {…}, "op": "get-status", "payload": { … } }
// response
{ "metadata": {…}, "op": "get-status", "payload": { … }, "rescode": 200 }
```

- A raiz **não aceita chave extra**. `rescode` dentro de um request já dá 400.
- `payload` é obrigatório (`{}` quando vazio). Em erro, ele é
  `{ "error": "UPPER_SNAKE", "detail"?: "…" }`, com `detail` de no máximo 2048
  caracteres.

`metadata`:

| Campo | Regra |
|---|---|
| `v` | `"0.5"`. O backend aceita `"0.4"`/`"0.5"`; outro valor dá 400 `UNSUPPORTED_VERSION` |
| `messageId` | UUID v4 **novo em toda mensagem** (log/trace, não serve para dedup) |
| `relationId` | id da transação; a resposta **ecoa** o do request |
| `timestamp` | inteiro, epoch ms. Valores `< 1e12` são lidos como segundos (×1000) |
| `serialNumber` | SN do device, que precisa ser igual ao `{sn}` do tópico (senão 403 `SN_MISMATCH`) |
| `instance` | clientId de quem originou o request. Nos requests do device, use o próprio SN |
| `seq` | opcional, monotônico por fluxo |
| `traceparent` | opcional (W3C). O backend manda nos device-procedures e o device DEVERIA ecoar |

Request sem `relationId` legível é descartado, porque não há para onde
responder. Fora esse caso, todo request recebe resposta, inclusive os de erro.

## 3. rescode

| Código | Uso / `payload.error` típico |
|---|---|
| 200 / 202 / 204 | sucesso / aceito assíncrono / sucesso sem corpo (`payload: {}`) |
| 400 | `INVALID_ENVELOPE`, `INVALID_PAYLOAD`, `UNSUPPORTED_VERSION` |
| 403 | `SN_MISMATCH`, `BOOTSTRAP_SCOPE`, `DEVICE_BLOCKED` |
| 404 | `UNKNOWN_OP`, `NOT_FOUND` |
| 409 | `STALE_VERSION`, `SETTINGS_VERSION_CONFLICT` |
| 413 | `PAYLOAD_TOO_LARGE` |
| 429 | `RATE_LIMITED` (20 rps por SN, burst 50) |
| 500 / 502 | `INTERNAL` / `UPSTREAM_ERROR` |
| 503 | `DEVICE_OFFLINE`: o backend responde sozinho quando o device está offline |
| 504 | `TIMEOUT`, `TTL_EXPIRED` |

## 4. Tópicos e ACL

`{sn}` são 12 hex maiúsculos (`^[0-9A-F]{12}$`). Tudo usa QoS 1.

| Tópico | Quem publica | Retido |
|---|---|---|
| `atrio/{sn}/status` | device (LWT e online) | **sim** |
| `atrio/{sn}/procedure` | backend | não |
| `atrio/{sn}/procedure/reply/{instance}` | device; `{instance}` = `metadata.instance` do request | não |
| `atrio/{sn}/service` | device | não |
| `atrio/{sn}/service/reply` | backend | não |

O device **assina exatamente** `atrio/{sn}/procedure` e
`atrio/{sn}/service/reply`, e pode assinar também `atrio/{sn}/status`.
Curinga e `$share` são negados pelo ACL, e o EMQX **desconecta** o cliente em
caso de negação. Por isso nunca assinar `atrio/{sn}/#`.

Certificado bootstrap (`OU=bootstrap`) não tem acesso a `service`: só status e
procedure.

## 5. Sessão MQTT

- MQTT **3.1.1**, clean session. A durabilidade vem da outbox, não do broker.
  Re-subscribe a cada CONNACK.
- **clientId = SN.** Sem usuário e sem senha: o broker usa o CN do certificado
  como username.
- mTLS obrigatório em produção (`mqtt.dextrolabs.com.br:8883`, EMQX 5.8).
  O certificado tem `CN={sn}` e `OU=devices` ou `OU=bootstrap`.
- Keepalive MQTT de **15 s**.
- **LWT** retido em `atrio/{sn}/status` com `{"online":false}`, QoS 1. Depois do
  CONNACK, publica `{"online":true,"ts":<ms>}` retido. Antes de um disconnect
  limpo, publica `{"online":false,"ts":…}` retido, porque o DISCONNECT cancela o
  LWT.
- Reconexão com backoff **exponencial e jitter**. O primeiro dreno depois de
  reconectar também é atrasado aleatoriamente, para evitar estouro da manada.
- Detectar conexão meio-aberta por tempo sem receber nada.
- O broker corta pacotes acima de 512 KB e o backend responde 413 acima de
  256 KiB. O MCU declara o seu `IOT_MAX_PAYLOAD`.

## 6. Regras do lado device

- Request com mais de **60 s** responde 504 `TTL_EXPIRED`, **nunca** é
  descartado em silêncio. O TTL é medido no relógio corrigido pelo `serverTime`
  do `keepalive`. Sem relógio sincronizado, o TTL fica desligado com aviso.
- `op` desconhecida responde 404 `UNKNOWN_OP`.
- Envelope inválido responde 400; acima do limite, 413.
- Op de atuador aceita `payload.idempotencyKey`. O device guarda o resultado por
  chave (janela de pelo menos 30 s) e **não reexecuta**: é at-most-once.
- Parser JSON tokenizado estático com `MAX_TOKENS`/`MAX_DEPTH`/`MAX_PAYLOAD`
  declarados (perfil MCU, spec §12). Payload malformado nunca derruba o device.
- `call_service` é não bloqueante (callback/FSM), para não travar o watchdog.
- UUID v4 vem do RNG de hardware.

## 7. Device-procedures que o device implementa (obrigatórias)

| op | Request | Resposta | Timeout do backend |
|---|---|---|---|
| `list-procedures` | `{}` (nenhuma propriedade) | 200 `{procedures:[{op, version?}], deviceType?, contractVersion?}`: **objetos**, sem chave extra | 30 s |
| `get-status` | `{sections?: string[]}` | 200 com objeto livre. Chaves recomendadas = as do keepalive | 30 s |
| `ota-notification` | `{version, artifactName, artifactUrl(https), sha256, signature?, signingKeyId?, sizeBytes?, artifactFormat?, applyAfter?, campaignItemId, progressEntityId?}` | `202 {}`, ou `200 {alreadyInstalled:true}`, ou 400 (`SIGNATURE_INVALID`, `CHECKSUM_INVALID`…), que vira falha sem retry | 30 s |
| `outbox-notification` | exatamente `{entities: string[], size: int}` | **`204 {}`** e dispara o dreno | 10 s |
| `settings-read` | `{refs?, offset?, limit?}` | 200 `{version, settings}` | 30 s |
| `settings-write` | `{version, settings}`, coleção completa | 200 `{appliedVersion, rejected:[{ref, reason}]}`. Versão menor dá 409 `STALE_VERSION` | 60 s |
| `certificate-request` | `{requestId, reason: initial\|renewal\|revoked, keyType:"EC-P256"}` | 200 `{requestId, csrPem}` (até 8 KB). Par de chaves novo a cada request; CSR com CN=SN, sem SAN, só `keyUsage` + `extendedKeyUsage=clientAuth`; `requestId` repetido devolve o mesmo CSR | 120 s |
| `certificate-install` | `{certificateId, certPem (≤8 KB), chainPem (≤16 KB), notAfter}` | 200 `{installed:true, fingerprint}` e reconecta com o certificado novo | 60 s |

> **Tamanho:** o request de `certificate-install` pode passar de 24 KB. Device
> que renova certificado precisa de `IOT_MAX_PAYLOAD` (e buffer MQTT de
> recepção) ≥ 26 KiB. O padrão de 8 KiB não basta.

Procedures da aplicação seguem o manifesto do **tipo de device** em
`uhura-contracts/contracts/iot-devices/procedures/<tipo>.procedures.ts`
(`docs/contracts.md` Parte C). O exemplo `io-controller` de lá tem
`relay-set`, `relay-pulse` e `inputs-read`. Elas entram em `list-procedures`
com `version`, e o `contractVersion` diz qual versão do manifesto o firmware
implementa.

## 8. Remote-procedures que o device chama

| op | Request | Resposta |
|---|---|---|
| `keepalive` | todos os campos opcionais e **chave desconhecida = 400**: `intervalMs, uptimeS, fwVersion, rssi, freeMemBytes, cpuLoad, tempC, ip, batteryPct, settingsVersion, certFingerprint, custom{}` | 200 `{serverTime, settingsVersion?, settingsResync?, nextIntervalMs?, pendingOutbox?}` |
| `list-services` | `{}` | 200 `{services:[{op, version, schemaRef?, required?}]}` |
| `inbox-write` | `{entity, entityId, payload, version?}` | 200 `{id, duplicate}`. Versão menor que a guardada dá 409 `STALE_VERSION` |
| `outbox-read` | `{limit? 1..100 (padrão 20), entity?}` | 200 `{count, items:[{id, command, entity, entityId, payload, createdAt, leaseUntil}]}` |
| `outbox-ack` | `{ids:[uuid] (≤100), rejects?:[{id, reason}]}` | 200 `{acknowledged, rejected}`, idempotente |
| `events-write` | `{events:[{seq, code, occurredAt, args?}]}` (1..200) | 200 `{accepted, duplicates, rejected, ackSeq}` |
| `settings-mirror-read` / `settings-mirror-save` | ver `contracts.md` A.3.7 e A.3.8 | |

Regras de cada uma:

- **`keepalive`:** a cada 60 s por padrão. A presença vence sem keepalive por
  `max(3×intervalMs, 90 s)`. O primeiro keepalive com um certificado novo leva
  `certFingerprint` (SHA-256 do DER, hex minúsculo), o que ativa o certificado.
- **outbox:**
  - drenar **ao conectar e periodicamente**, com ou sem aviso;
  - o lease dura 60 s; item sem ack volta;
  - depois de 10 tentativas, o item vai para a dead-letter;
  - aplicar de forma idempotente por `id`;
  - um item novo para o mesmo `(entity, entityId)` substitui o pendente.
- **inbox:**
  - dedup por `(device, entity, entityId, version)`;
  - `entityId` e `version` precisam ser **estáveis entre reboots**;
  - reenviar até receber 2xx.
- **`events-write`:** `seq` é monotônico e **persistido**. O reenvio repete o
  mesmo `occurredAt`. Tudo até o `ackSeq` pode sair do buffer local.
