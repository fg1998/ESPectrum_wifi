# Plano de ação — Conflito SD × WiFi

Plano para eliminar o conflito entre cartão SD e WiFi no firmware ESPectrum_wifi.

**Documento de referência:** [conflito-sd-wifi.md](./conflito-sd-wifi.md)

**Como usar:** marque `[x]` à medida que cada item for concluído e testado. Adicione data/notas na secção [Registo de progresso](#registo-de-progresso) no final.

---

## Legenda de prioridade

| Símbolo | Significado |
|---------|-------------|
| 🔴 | Crítico — fazer primeiro |
| 🟡 | Importante — alto impacto |
| 🟢 | Complementar — refinamento / hardware |

---

## Fase 1 — Correções rápidas (baixo risco)

Objetivo: estabilizar WiFi e reduzir contenção sem refactor grande.

### 1.1 WiFi — inicialização única 🔴

- [x] Criar `NetworkMenu::wifiInitOnce()` (ou equivalente): `esp_netif_init`, event loop, `esp_wifi_init`, handlers **uma vez**
- [x] Alterar `wifiConnect()` para só `set_config` + `connect` (sem `esp_wifi_init` repetido)
- [x] Garantir que handlers não são registados múltiplas vezes
- [ ] Testar: 3+ reconexões WiFi seguidas sem reboot → sem `ESP_ERR_INVALID_STATE` no serial
- [ ] Testar: menu rede → configurar WiFi → browser FTP → voltar e reconectar

**Ficheiros:** `src/NetworkMenu.cpp`, `include/NetworkMenu.h`

---

### 1.2 Política SD ↔ WiFi (exclusão temporal) 🔴

- [x] Adicionar `NetworkMenu::wifiPauseForSd()` / `wifiResumeAfterSd()` centralizados (contador aninhado)
- [x] Browser de ficheiros: pausa no início de `fileDialog`, resume em `fileDialogEnd`
- [x] Montagem SD: pausa em `mountSDCard` (pares pause/resume)
- [ ] Testar: WiFi ligado → abrir browser SNA/TAP → sem desconexão nem `unmount` espúrio
- [ ] Testar: browser SD → depois menu FTP → WiFi reconecta normalmente

**Ficheiros:** `src/OSDFile.cpp`, `src/FileUtils.cpp`, `src/NetworkMenu.cpp`

---

### 1.3 Cedência de CPU nos loops bloqueantes 🟡

- [x] Em `ftpDownload`: `vTaskDelay(1)` a cada ~32 KB
- [x] Em `DirToFile`: `vTaskDelay(1)` a cada 64 entradas processadas
- [ ] Em `ftpList` / `ftpReadLine` se ainda bloquear muito: yield ocasional (adiado)
- [ ] Testar: download FTP grande com WiFi estável até ao fim
- [ ] Testar: pasta SD com muitos ficheiros — indexação completa sem perder WiFi

**Ficheiros:** `src/NetworkMenu.cpp`, `src/FileUtils.cpp`

---

### 1.4 sdkconfig (build PSRAM) 🟡

- [x] `sdkconfig.psram`: ativar `CONFIG_ESP32_WIFI_IRAM_OPT`
- [x] `sdkconfig.psram`: ativar `CONFIG_ESP32_WIFI_RX_IRAM_OPT`
- [x] `sdkconfig.psram`: `CONFIG_LWIP_TCPIP_TASK_AFFINITY_CPU0=y` (desligar NO_AFFINITY)
- [x] Rebuild env `psram` — compilação OK (2026-05-18)
- [ ] Gravar firmware e repetir testes da Fase 1.2 e 1.3 no hardware real

**Ficheiros:** `sdkconfig.psram` (e `sdkconfig.nopsram` se usar esse build também)

---

### 1.5 SD — remount menos agressivo 🟢

- [x] `spi_bus_initialize`: aceitar `ESP_ERR_INVALID_STATE` (padrão fabGL)
- [x] `isMountedSDCard`: retry (3×) antes de reportar falha
- [ ] Testar: stress WiFi + listagem SD — cartão não desmonta ao primeiro `EIO`

**Ficheiros:** `src/FileUtils.cpp`

---

### ✅ Critério de conclusão da Fase 1

- [ ] WiFi reconecta 5× seguidas sem falha
- [ ] Browser de ficheiros com WiFi previamente ligado funciona
- [ ] Download FTP > 100 KB completa com sucesso
- [ ] Sem regressão visível em vídeo/áudio (smoke test 2 min de emulação)

---

## Fase 2 — Separação por core (solução estrutural)

Objetivo: I/O SD pesado fora do core 0 (WiFi + app_main).

### 2.1 Desenho da task SD 🟡

- [ ] Definir API: fila + `sdIOTask` no **core 1**, prioridade **2–3** (abaixo de `videoTask` = 5)
- [ ] Operações na fila: mount, unmount, `DirToFile`, gravação FTP (ou só as bloqueantes)
- [ ] Menu / `app_main` no core 0 envia pedido e bloqueia em semáforo/event group até concluir
- [ ] Confirmar: **não** mover WiFi para core 1 nem `app_main` para core 1

**Ficheiros novos/alterados:** `src/FileUtils.cpp`, `include/FileUtils.h` (ou `SdIOTask.cpp`)

---

### 2.2 Implementação 🟡

- [ ] `xTaskCreatePinnedToCore(sdIOTask, ..., core=1, prio=2 ou 3)`
- [ ] Migrar `mountSDCard` / `unmountSDCard` para execução na task SD
- [ ] Migrar `DirToFile` para a task SD
- [ ] `ftpDownload`: `fwrite` via task SD ou buffer + pedido único de escrita
- [ ] Proteger acesso concorrente com mutex se alguma chamada restar síncrona

---

### 2.3 Testes da Fase 2 🟡

- [ ] WiFi ligado + indexação SD em pasta grande — WiFi mantém-se
- [ ] Download FTP durante emulação — sem crash, sem flicker VGA grave
- [ ] Medir: sem watchdog reset durante `DirToFile` (se WDT for reativado)

---

### ✅ Critério de conclusão da Fase 2

- [ ] Core 0: WiFi + menus; core 1: vídeo + áudio + SD I/O
- [ ] Operações SD longas não bloqueiam a task WiFi
- [ ] Fase 1 continua válida (regressão zero)

---

## Fase 3 — Opcional / hardware / documentação

### 3.1 Watchdog e diagnóstico 🟢

- [ ] Avaliar reativar `CONFIG_ESP_TASK_WDT` com timeout adequado
- [ ] Logs de debug opcionais: estado WiFi, `SDReady`, heap livre antes/depois de indexação

---

### 3.2 Hardware (se software não bastar) 🟢

- [ ] Testar com fonte 5 V estável (≥ 500 mA)
- [ ] Cartão SD de qualidade, cabo curto
- [ ] Placa TTGO: verificar MISO GPIO2 / pull-ups
- [ ] Registar resultado no [Registo de progresso](#registo-de-progresso)

---

### 3.3 Documentação 🟢

- [ ] Atualizar [conflito-sd-wifi.md](./conflito-sd-wifi.md) com solução implementada
- [ ] Notas de build (`psram` vs `nopsram`) no README ou em `docs/`

---

## Resumo visual do estado

| Fase | Descrição | Itens | Concluídos |
|------|-----------|-------|------------|
| 1 | Correções rápidas | 5 blocos | 5 / 5 (código); testes HW pendentes |
| 2 | Task SD no core 1 | 3 blocos | 0 / 3 |
| 3 | Opcional | 3 blocos | 0 / 3 |

*Atualizar a tabela manualmente ao marcar checkboxes.*

---

## Registo de progresso

| Data | Fase / item | O que foi feito | Resultado do teste |
|------|-------------|-----------------|-------------------|
| 2026-05-18 | Fase 1 (código) | WiFi init único, pause/resume SD, yields, sdkconfig, SD retry | Build `psram` OK; testes na placa pendentes |
| 2026-05-18 | Hotfix WiFi | Buffers reduzidos (RAM interna ~58KB); netif duplicado corrigido | Aguarda reflash e teste |
| | | | |
| | | | |

---

## Notas

- **Placa de teste:** _________________ (TTGO / ESPectrum / Olimex)
- **Build:** _________________ (`psram` / `nopsram`)
- **Commit / branch quando Fase 1 fechar:** _________________

---

*Criado a partir da análise em `conflito-sd-wifi.md`. Marque os itens com `[x]` conforme avançar.*
