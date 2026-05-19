# Alterações SD × WiFi × FTP — ESPectrum_wifi

Documento de registo das alterações implementadas no firmware para resolver o conflito entre cartão SD e WiFi, estabilizar a rede e permitir download FTP com gravação no SD e carregamento de jogos.

**Data de referência:** 2026-05-18  
**Build alvo:** ambiente PlatformIO `psram`  
**Documentos relacionados:**

- [conflito-sd-wifi.md](./conflito-sd-wifi.md) — análise técnica do problema
- [plano-acao-sd-wifi.md](./plano-acao-sd-wifi.md) — checklist de implementação e testes

---

## Resumo executivo

O problema **não era** o WiFi partilhar o barramento SPI do SD (o rádio é interno ao ESP32). O conflito real vinha de:

1. **Uso simultâneo** de SD e WiFi no mesmo momento (CPU, SPI, alimentação).
2. **Bugs no stack WiFi** (init repetido, handlers duplicados, SSID corrompido na NVS).
3. **Política incorreta** ao gravar ficheiros FTP (WiFi religado antes do `fwrite`).
4. **Limitações de pilha** (buffer de 32 KB na task `main`, 8 KB de stack).
5. **API errada** para carregar `.tap` após download (`LoadSnapshot` não suporta fitas).

A solução adoptada na **Fase 1** separa temporalmente rede e SD: desmonta o SD ao entrar no menu rede, descarrega para RAM com WiFi ligado, grava no SD com WiFi parado, e volta a expor o menu ou carrega o jogo.

---

## Ficheiros modificados

| Ficheiro | Tipo de alteração |
|----------|-------------------|
| `src/NetworkMenu.cpp` | WiFi, FTP, download, carregamento de jogos |
| `include/NetworkMenu.h` | API pública do menu rede |
| `src/FileUtils.cpp` | Montagem SD, pausa WiFi, `force_wifi_pause` |
| `include/FileUtils.h` | Assinatura `initFileSystem` / `mountSDCard` |
| `src/OSDMain.cpp` | Integração menu rede (opt 7), saída do OSD após carregar jogo |
| `src/OSDFile.cpp` | Pausa WiFi no browser de ficheiros local |
| `sdkconfig.psram` | Buffers WiFi, optimizações IRAM, afinidade lwIP |
| `include/messages_pt.h` | Novas strings PT |
| `include/messages_en.h` | Novas strings EN |
| `include/messages_es.h` | Novas strings ES |
| `include/messages.h` | Arrays de mensagens de rede |
| `docs/conflito-sd-wifi.md` | Análise (criado anteriormente) |
| `docs/plano-acao-sd-wifi.md` | Plano de acção (criado anteriormente) |

---

## 1. WiFi — inicialização e conexão

### 1.1 `wifiInitOnce()`

**O quê:** `esp_netif_init`, event loop, criação única da netif STA, `esp_wifi_init`, registo de handlers e `WIFI_MODE_STA` executados **uma vez**.

**Porquê:** O código original chamava `esp_wifi_init` em cada `wifiConnect`, sem `esp_wifi_deinit`, causando `ESP_ERR_INVALID_STATE`, handlers duplicados e comportamento imprevisível.

### 1.2 Netif STA criada só uma vez

**O quê:** Flag `s_netif_sta_ready`; `esp_netif_create_default_wifi_sta()` só se `WIFI_STA_DEF` ainda não existir.

**Porquê:** Evita erro de netif duplicada ao reconectar.

### 1.3 Handler `WIFI_EVENT_STA_DISCONNECTED` com retry

**O quê:** Flag `s_wifi_awaiting_ip`. Enquanto se espera IP, disconnect **não** marca falha; chama `esp_wifi_connect()` de novo. `WIFI_FAIL_BIT` só fora desta fase.

**Porquê:** Após pausa do SD ou beacon timeout (`reason=39`), o primeiro disconnect não deve abortar a ligação com falha imediata.

### 1.4 `wifiConnect()` — connect explícito e timeout

**O quê:**

- `esp_wifi_set_config` + `esp_wifi_start` + `esp_wifi_connect()` explícito (não depender só de `STA_START` após resume).
- Espera só `WIFI_CONN_BIT` (25 s), não `WIFI_FAIL_BIT`.
- Verificação final com `wifiHasIp()`.
- `esp_wifi_set_ps(WIFI_PS_NONE)` durante ligação.

**Porquê:** Após `wifiResumeAfterSd()`, `STA_START` pode não voltar a disparar; o timeout antigo de 20 s e a falha imediata em disconnect impediam obter IP.

### 1.5 Sanitização NVS (SSID, senha, host)

**O quê:** Funções `sanitizeWifiText`, `sanitizeHost`; correcção da leitura `nvsGetStr` (tamanho do buffer); gravação só de texto ASCII válido.

**Porquê:** SSID/senha corrompidos na NVS geravam SSIDs inválidos e falhas de associação.

### 1.6 Overlays WiFi (`showWifiStatusOverlay` / `hideWifiStatusOverlay`)

**O quê:** Guardar e restaurar backbuffer ao mostrar “Conectando…” / “Listando…”.

**Porquê:** A mensagem ficava permanentemente no menu sem restaurar o fundo.

---

## 2. Política SD ↔ WiFi (exclusão temporal)

### 2.1 `wifiPauseForSd()` / `wifiResumeAfterSd()`

**O quê:** Contador aninhado `s_wifi_pause_depth`; na primeira pausa chama `esp_wifi_stop()`, no último resume chama `esp_wifi_start()`.

**Porquê:** Centralizar parar/arrancar o rádio em vez de espalhar chamadas; suportar pausas aninhadas (browser SD + mount).

### 2.2 `prepareForNetwork()` / `restoreAfterNetwork()`

**O quê:**

- Ao entrar no menu rede (opt 7): `s_network_session = true`, zera contador de pausa, **desmonta o SD** se montado, `vTaskDelay(100 ms)`.
- Ao sair: remonta o SD se tinha sido libertado para a rede.

**Porquê:** Com o jogo já em RAM, libertar o SPI do SD durante WiFi/FTP reduz interferência e falhas de associação. Log esperado: `Desmontando SD para libertar SPI (WiFi/rede)`.

### 2.3 Browser de ficheiros local (`OSDFile.cpp`)

**O quê:** `wifiPauseForSd()` no início de `fileDialog`; `wifiResumeAfterSd()` em `fileDialogEnd()` (todos os caminhos de saída).

**Porquê:** `DirToFile` e I/O SD pesado não devem correr com WiFi activo se o utilizador tinha rede ligada.

### 2.4 `mountSDCard` e `isNetworkSession()`

**O quê:** Em `mountSDCard`, `pause_wifi = force_wifi_pause || !isNetworkSession()`. Durante sessão de rede, montagem normal **não** pausa WiFi; gravação FTP usa `force_wifi_pause = true`.

**Porquê:** Separar “montar para browser com WiFi possivelmente ligado antes” de “montar só para gravar com WiFi parado”.

### 2.5 `mountSDCard` — não religar WiFi com `force_wifi_pause` (**correcção crítica**)

**O quê:** No fim de `mountSDCard`, `wifiResumeAfterSd()` só se `pause_wifi && !force_wifi_pause`.

**Porquê:** O bug fazia o WiFi voltar **imediatamente após montar o SD** e **antes** do `fwrite` do FTP, causando `sdmmc_write_blocks failed (257)` e `Download concluido: N bytes (falhou)` com download de rede completo.

### 2.6 `spi_bus_initialize` e `isMountedSDCard`

**O quê:** Aceitar `ESP_ERR_INVALID_STATE` no init do barramento; 3 tentativas em `isMountedSDCard` antes de falhar.

**Porquê:** Remount menos agressivo; evitar desmontar ao primeiro `EIO` transitório.

---

## 3. Cliente FTP

### 3.1 Ligação e DNS

**O quê:** Logs detalhados; `getaddrinfo` com fallback; `ftpTcpConnectTimeout` com `select` e socket non-blocking; timeouts de socket (`SO_RCVTIMEO` / `SO_SNDTIMEO`).

**Porquê:** Servidores por nome (ex. Locaweb) e redes lentas; evitar bloqueio indefinido.

### 3.2 Modo PASV e NAT

**O quê:** Se o IP PASV for privado (10.x, 172.16–31, 192.168.x), usar o IP público do servidor de controlo.

**Porquê:** Hospedagem partilhada devolve IP interno no `227`; o ESP32 não alcança esse IP na LAN do datacenter.

### 3.3 Listagem (`ftpList`)

**O quê:** Logs de PASV, NLST e contagem de entradas; overlay “Listando…”.

**Porquê:** Diagnóstico quando o browser ficava em “Listando…” sem feedback.

### 3.4 Porta FTP na NVS

**O quê:** Chave `ftp_port` em `loadConfig` / `saveConfig` (default 21). Sem campo no menu ainda.

**Porquê:** Preparar servidores em portas não standard.

---

## 4. Download FTP e gravação no SD

### 4.1 Separação rede → RAM → SD

**O quê:** Durante sessão de rede (`isNetworkSession()`):

1. **Recepção** com WiFi ligado e SD desmontado.
2. **Gravação** com `netFlushToSd()`: pausa WiFi, monta SD (`initFileSystem(true)`), `fwrite`, desmonta SD, resume WiFi.

**Porquê:** Nunca aceder ao SPI do SD e ao rádio em simultâneo; padrão que elimina travamentos e resets.

### 4.2 Modos de download

| Modo | Condição | Comportamento |
|------|----------|----------------|
| Buffer RAM | Tamanho conhecido e ≤ limite (2 MB com PSRAM, 192 KB sem) | `heap_caps_malloc` / `malloc`, `recv` completo, uma gravação SD |
| Blocos | Sessão rede e ficheiro grande ou SIZE indisponível | Buffer 8 KB em heap/PSRAM; flush a cada 8 KB |
| Directo | Fora da sessão rede | `fopen` + `recv`/`fwrite` intercalados (comportamento clássico) |

**Porquê:** Ficheiros `.tap` de dezenas de KB cabem em RAM; ficheiros grandes não estouram a pilha.

### 4.3 Buffers em heap, não na pilha

**O quê:** `netAllocBuf` / `netFreeBuf`; buffer de flush 8 KB (`FTP_SD_FLUSH`), nunca 32 KB na stack da `main`.

**Porquê:** `CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192`; `uint8_t accum[32768]` causava **stack overflow** e reboot com mensagem `A stack overflow in task main has been detected`.

### 4.4 `netFlushToSd()` — escrita robusta

**O quê:**

- Pausa WiFi + atrasos (50 ms / 30 ms) antes de abrir ficheiro.
- `fwrite` em blocos de 4 KB com log de erro (`errno`).
- Log de sucesso: `Gravado no SD: N bytes -> caminho`.
- Desmonta SD ao fim se sessão rede; resume WiFi uma vez.

**Porquê:** Estabilizar hardware após `esp_wifi_stop`; detectar falhas parciais de escrita.

### 4.5 Validação de download completo

**O quê:** Se `SIZE` conhecido, exige `received == total`; log `[incompleto]` se diferente.

**Porquê:** Evitar marcar sucesso com ficheiro truncado.

### 4.6 Diálogo de progresso

**O quê:** `progressDialog` com acção 0 (mostrar), 1 (actualizar %), 2 (fechar) no fim.

**Porquê:** Evitar overlay de progresso preso no ecrã.

### 4.7 Cedência de CPU

**O quê:** `vTaskDelay(1)` nos loops de `recv` e gravação; yield em `DirToFile` (64 entradas).

**Porquê:** Teclado e vídeo continuam responsivos durante operações longas.

---

## 5. Destino dos ficheiros e carregamento de jogos

### 5.1 `netFtpDestPath()`

**O quê:** Destino conforme extensão:

- `.tap` / `.tzx` → `TAP_Path`
- `.dsk` → `DSK_Path`
- resto (`.z80`, `.sna`, etc.) → `SNA_Path`

**Porquê:** Ficheiros eram gravados em `SNA_Path` (`/sd/ficheiro.tap`) mas `Tape::LoadTape` procura em `TAP_Path`.

### 5.2 `netLoadDownloadedFile()`

**O quê:**

| Extensão | API |
|----------|-----|
| `.z80`, `.sna`, `.sp`, `.p`, `.esp` | `LoadSnapshot()` + `Config::ram_file` |
| `.tap`, `.tzx` | `Tape::LoadTape("S" + rel)` ou `"R"` se flashload activo |

**Porquê:** `LoadSnapshot` **não** suporta `.tap`; o emulador usa o fluxo da tecla F4 (`Tape::LoadTape`).

### 5.3 Diálogo “Carregar jogo?”

**O quê:** Após download OK, `msgDialog` com `NET_DLG_LOAD_GAME`; se Sim → `restoreAfterNetwork()`, carrega ficheiro, `s_exit_osd_after_load = true`.

**Porquê:** O utilizador quer jogar logo após baixar, não ficar no browser FTP.

### 5.4 Saída do menu OSD

**O quê:** `shouldExitOsdAfterLoad()` / `clearExitOsdAfterLoad()`; em `OSDMain.cpp`, após `ftpBrowser()` ou ao sair do loop rede, `return` do handler OSD (como ao carregar snapshot no menu principal).

**Porquê:** Mesmo respondendo “Sim”, o jogo não aparecia porque o menu F1 continuava activo por cima do emulador.

---

## 6. Configuração de build (`sdkconfig.psram`)

**O quê (resumo):**

- `CONFIG_ESP32_WIFI_IRAM_OPT` e `CONFIG_ESP32_WIFI_RX_IRAM_OPT` activados.
- Buffers WiFi estáticos reduzidos (ex. `STATIC_RX_BUFFER_NUM=6`) para caber na RAM interna.
- `CONFIG_LWIP_TCPIP_TASK_AFFINITY_CPU0=y`.
- PSRAM para malloc WiFi/LwIP onde aplicável.

**Porquê:** Falhas `malloc` do stack WiFi com framebuffer grande; melhor convivência com tarefas no core 0.

---

## 7. Mensagens internacionalizadas

**Novas chaves:**

| Chave | Uso |
|-------|-----|
| `NET_DLG_LOAD_GAME` | “Carregar jogo?” após download |
| `NET_MSG_FILE_TOO_LARGE` | Reservado para limite de tamanho |

**Ficheiros:** `messages_pt.h`, `messages_en.h`, `messages_es.h`, índices em `messages.h`.

---

## 8. Tabela de problemas → soluções

| Sintoma | Causa identificada | Solução |
|---------|-------------------|---------|
| WiFi não ligava após jogo no SD | SD + WiFi activos; init WiFi repetido | `prepareForNetwork`, `wifiInitOnce`, retry em disconnect |
| SSID estranho / corrupto | NVS sem sanitização | `sanitizeWifiText`, fix `nvsGetStr` |
| `WiFi falhou (desconectado)` imediato | `WIFI_FAIL_BIT` em todo disconnect | `s_wifi_awaiting_ip` + só esperar `WIFI_CONN_BIT` |
| FTP “Listando…” infinito | PASV com IP privado | Substituir IP PASV pelo IP do servidor |
| Reset ao baixar ficheiro | 32 KB na pilha da `main` | Buffers em PSRAM/heap |
| Teclado trava no download | SD montado com WiFi activo | Download para RAM, gravação com WiFi parado |
| `sdmmc_write_blocks failed (257)` | WiFi religado após mount, antes do `fwrite` | `force_wifi_pause` sem resume no fim do mount |
| `Download concluido (falhou)` com bytes correctos | `fwrite` falhou com WiFi ligado | Correcção acima + `netFlushToSd` |
| “Carregar jogo?” não faz nada | `LoadSnapshot` em `.tap`; menu OSD aberto | `Tape::LoadTape`, `netFtpDestPath`, saída OSD |

---

## 9. Fluxo actual (menu rede → FTP → jogo)

```
F1 → Acesso Rede
  → prepareForNetwork()     [desmonta SD]
  → WiFi / FTP browser
  → Seleccionar ficheiro → Download
       → recv → RAM (WiFi ON)
       → netFlushToSd()     [WiFi OFF → mount → fwrite → unmount → WiFi ON]
  → “Download OK!”
  → “Carregar jogo?”
       → Sim: restoreAfterNetwork() [remonta SD]
              → netLoadDownloadedFile()
              → sair do OSD (emulador)
       → Não: continuar no browser FTP
  → ESC → restoreAfterNetwork()
```

---

## 10. Limitações e trabalho futuro (Fase 2)

- **Task SD dedicada no core 1** — ainda não implementada; I/O SD continua na `main` (core 0).
- **Campo “Porta FTP” no menu** — porta só na NVS, sem UI.
- **Ficheiros muito grandes** — limite ~2 MB em buffer único com PSRAM; acima disso, modo blocos (mais lento, mais ciclos mount/write).
- **Reconexão WiFi** após cada gravação — esperado (`reason=8` ao parar WiFi); o browser FTP reconecta na operação seguinte.
- **Testes formais** da Fase 1 no `plano-acao-sd-wifi.md` — parcialmente feitos em hardware; checklist ainda com itens por marcar.

---

## 11. Como compilar e gravar

```bash
cd /caminho/para/ESPectrum_wifi
pio run -e psram -t upload
```

Monitor serial recomendado para validar:

- `Desmontando SD para libertar SPI`
- `GOT_IP: …`
- `Gravado no SD: N bytes -> /sd/...`
- `Download concluido: N bytes (OK)`
- `Fita carregada` ou `Snapshot carregado`

---

## 12. Registo de progresso sugerido

| Data | Teste | Resultado |
|------|-------|-----------|
| 2026-05-18 | Build `psram` | OK |
| | WiFi + FTP Locaweb | OK (após correcções PASV/NVS) |
| | Download `.tap` + gravação SD | OK (após `force_wifi_pause`) |
| | Carregar jogo após FTP | A validar pelo utilizador |

*Actualizar esta tabela após testes na placa.*

---

*Documento gerado para consolidar o trabalho da sessão de desenvolvimento SD/WiFi/FTP. Para alterações futuras, actualizar também [plano-acao-sd-wifi.md](./plano-acao-sd-wifi.md).*
