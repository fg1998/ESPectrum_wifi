/*

ESPectrum Fork — NetworkMenu.cpp
Menu de Acesso à Rede: WiFi + cliente FTP browser.

Configurações salvas em NVS namespace "storage",
o mesmo usado pelo ESPConfig.cpp do projeto.

Coloque em: src/NetworkMenu.cpp
Inclua em OSDMain.cpp: #include "NetworkMenu.h"

*/

#include "NetworkMenu.h"
#include "OSDMain.h"
#include "ESPConfig.h"
#include "FileUtils.h"
#include "Snapshot.h"
#include "Tape.h"
#include "Video.h"
#include "messages.h"

#include "esp_heap_caps.h"

#include "nvs.h"
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "esp_netif_ip_addr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include <fcntl.h>
#include <sys/select.h>

#include <stdio.h>
#include <string.h>
#include <cctype>
#include <cerrno>
#include <string>
#include <vector>

#include <dirent.h>
using namespace std;

// ─────────────────────────────────────────────────────────────────────────────
// Chaves NVS — namespace "storage" (mesmo do ESPConfig)
// ─────────────────────────────────────────────────────────────────────────────
#define NVS_NS        "storage"
#define NVS_WIFI_SSID "net_ssid"
#define NVS_WIFI_PASS "net_pass"
#define NVS_FTP_HOST  "ftp_host"
#define NVS_FTP_USER  "ftp_user"
#define NVS_FTP_PASS  "ftp_pass"
#define NVS_FTP_PATH  "ftp_path"
#define NVS_FTP_PORT  "ftp_port"

// ─────────────────────────────────────────────────────────────────────────────
// Statics
// ─────────────────────────────────────────────────────────────────────────────
bool NetworkMenu::wifi_connected = false;

static EventGroupHandle_t s_wifi_eg = nullptr;
#define WIFI_CONN_BIT BIT0
#define WIFI_FAIL_BIT BIT1

static char ftp_resp[512];

static bool s_wifi_stack_ready = false;
static bool s_netif_sta_ready = false;
static bool s_wifi_handlers_registered = false;
static int  s_wifi_pause_depth = 0;
static bool s_wifi_stopped_for_sd = false;
static bool s_sd_released_for_net = false;
static bool s_network_session = false;
static bool s_wifi_awaiting_ip = false;
static bool s_exit_osd_after_load = false;

static const size_t FTP_SD_FLUSH = 8192; // heap/PSRAM — nunca na pilha da task main (8KB)
static const size_t FTP_MAX_RAM_DL_PSRAM = 2 * 1024 * 1024;
static const size_t FTP_MAX_RAM_DL_NOPS = 192 * 1024;

static uint8_t *netAllocBuf(size_t sz) {
    uint8_t *p = (uint8_t *)heap_caps_malloc(sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) p = (uint8_t *)malloc(sz);
    return p;
}

static void netFreeBuf(void *p) {
    if (p) heap_caps_free(p);
}

static bool netFileIsGame(const string &path) {
    const string ext = FileUtils::getLCaseExt(path);
    return ext == "z80" || ext == "sna" || ext == "tap" || ext == "tzx" || ext == "p" || ext == "sp";
}

static string netFtpDestPath(const string &filename) {
    const string ext = FileUtils::getLCaseExt(filename);
    string sub = FileUtils::SNA_Path;
    if (ext == "tap" || ext == "tzx") sub = FileUtils::TAP_Path;
    else if (ext == "dsk") sub = FileUtils::DSK_Path;
    string dest = FileUtils::MountPoint + sub;
    if (dest.back() != '/') dest += '/';
    dest += filename;
    return dest;
}

static bool netLoadDownloadedFile(const string &dest) {
    const string ext = FileUtils::getLCaseExt(dest);

    if (ext == "tap" || ext == "tzx") {
        string tapBase = FileUtils::MountPoint + FileUtils::TAP_Path;
        if (tapBase.back() != '/') tapBase += '/';

        if (dest.compare(0, tapBase.size(), tapBase) != 0) {
            ESP_LOGE("FTP", "Fita fora de TAP_Path (esperado sob %s): %s", tapBase.c_str(), dest.c_str());
            return false;
        }
        const string rel = dest.substr(tapBase.size());

        const bool canFlash = (ext == "tap") && Config::flashload
            && (Config::romSet != "ZX81+") && (Config::romSet != "48Kcs")
            && (Config::romSet != "128Kcs") && (Config::romSet != "TKcs");
        const string prefix = canFlash ? "R" : "S";
        Tape::LoadTape(prefix + rel);
        ESP_LOGI("FTP", "Fita carregada (%s): %s%s", prefix.c_str(), tapBase.c_str(), rel.c_str());
        return true;
    }

    if (ext == "sna" || ext == "z80" || ext == "sp" || ext == "p" || ext == "esp") {
        const bool ok = LoadSnapshot(dest, "", "", 0xff);
        if (ok) {
            Config::ram_file = dest;
            Config::last_ram_file = dest;
            ESP_LOGI("FTP", "Snapshot carregado: %s", dest.c_str());
        } else {
            ESP_LOGE("FTP", "LoadSnapshot falhou: %s", dest.c_str());
        }
        return ok;
    }

    ESP_LOGW("FTP", "Extensao nao suportada para carregar: %s", ext.c_str());
    return false;
}

bool NetworkMenu::shouldExitOsdAfterLoad() {
    return s_exit_osd_after_load;
}

void NetworkMenu::clearExitOsdAfterLoad() {
    s_exit_osd_after_load = false;
}

static size_t netMaxFtpBuffer() {
    return Config::psram_size ? FTP_MAX_RAM_DL_PSRAM : FTP_MAX_RAM_DL_NOPS;
}

// Grava no SD com WiFi parado durante toda a escrita (WiFi nao pode voltar apos mount).
static bool netFlushToSd(const string &local, const uint8_t *data, size_t len, bool &first) {
    if (len == 0) return true;

    NetworkMenu::wifiPauseForSd();
    vTaskDelay(pdMS_TO_TICKS(50));

    if (!FileUtils::SDReady) {
        FileUtils::initFileSystem(true);
    }
    if (!FileUtils::SDReady) {
        ESP_LOGE("FTP", "SD indisponivel para escrita");
        NetworkMenu::wifiResumeAfterSd();
        return false;
    }

    vTaskDelay(pdMS_TO_TICKS(30));

    FILE *f = fopen(local.c_str(), first ? "wb" : "ab");
    if (!f) {
        ESP_LOGE("FTP", "fopen falhou: %s (errno %d)", local.c_str(), errno);
        if (NetworkMenu::isNetworkSession()) FileUtils::unmountSDCard();
        NetworkMenu::wifiResumeAfterSd();
        return false;
    }

    size_t written = 0;
    const size_t chunk = 4096;
    while (written < len) {
        size_t n = len - written;
        if (n > chunk) n = chunk;
        size_t w = fwrite(data + written, 1, n, f);
        written += w;
        if (w != n) {
            ESP_LOGE("FTP", "fwrite falhou em %u/%u (errno %d)", (unsigned)written, (unsigned)len, errno);
            fclose(f);
            if (NetworkMenu::isNetworkSession()) FileUtils::unmountSDCard();
            NetworkMenu::wifiResumeAfterSd();
            return false;
        }
        vTaskDelay(1);
    }
    fflush(f);
    fclose(f);
    first = false;

    ESP_LOGI("FTP", "Gravado no SD: %u bytes -> %s", (unsigned)len, local.c_str());

    if (NetworkMenu::isNetworkSession()) {
        FileUtils::unmountSDCard();
    }
    vTaskDelay(pdMS_TO_TICKS(20));
    NetworkMenu::wifiResumeAfterSd();
    return true;
}

// ═════════════════════════════════════════════════════════════════════════════
// NVS helpers
// ═════════════════════════════════════════════════════════════════════════════

static string sanitizeWifiText(const string &s, size_t maxLen) {
    string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        if (c >= 32 && c <= 126) {
            out += (char)c;
            if (out.size() >= maxLen) break;
        }
    }
    return out;
}

static string sanitizeHost(const string &s) {
    string out;
    for (unsigned char c : s) {
        if (isalnum(c) || c == '.' || c == '-' || c == '_') {
            out += (char)c;
            if (out.size() >= 64) break;
        }
    }
    return out;
}

static bool wifiHasIp() {
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!netif) return false;
    esp_netif_ip_info_t ip;
    if (esp_netif_get_ip_info(netif, &ip) != ESP_OK) return false;
    return ip.ip.addr != 0;
}

static string nvsGetStr(nvs_handle_t h, const char *key, const char *def = "") {
    size_t len = 0;
    if (nvs_get_str(h, key, nullptr, &len) != ESP_OK || len == 0) return def;
    char *buf = new char[len];
    if (!buf) return def;
    size_t buflen = len;
    if (nvs_get_str(h, key, buf, &buflen) != ESP_OK) {
        delete[] buf;
        return def;
    }
    string s(buf);
    delete[] buf;
    return s;
}

// ═════════════════════════════════════════════════════════════════════════════
// Config — NVS load / save
// ═════════════════════════════════════════════════════════════════════════════

NetworkMenu::NetConfig NetworkMenu::loadConfig() {
    NetConfig cfg;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return cfg;

    cfg.wifi_ssid = sanitizeWifiText(nvsGetStr(h, NVS_WIFI_SSID, ""), 32);
    cfg.wifi_pass = sanitizeWifiText(nvsGetStr(h, NVS_WIFI_PASS, ""), 64);
    cfg.ftp_host  = sanitizeHost(nvsGetStr(h, NVS_FTP_HOST,  ""));
    cfg.ftp_user  = nvsGetStr(h, NVS_FTP_USER,  "anonymous");
    cfg.ftp_pass  = nvsGetStr(h, NVS_FTP_PASS,  "guest");
    cfg.ftp_path  = nvsGetStr(h, NVS_FTP_PATH,  "/");

    uint32_t port = 21;
    nvs_get_u32(h, NVS_FTP_PORT, &port);
    cfg.ftp_port = (int)port;

    nvs_close(h);
    return cfg;
}

void NetworkMenu::saveConfig(const NetConfig &cfg) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;

    string ssid = sanitizeWifiText(cfg.wifi_ssid, 32);
    string pass = sanitizeWifiText(cfg.wifi_pass, 64);
    nvs_set_str(h, NVS_WIFI_SSID, ssid.c_str());
    nvs_set_str(h, NVS_WIFI_PASS, pass.c_str());
    nvs_set_str(h, NVS_FTP_HOST,  sanitizeHost(cfg.ftp_host).c_str());
    nvs_set_str(h, NVS_FTP_USER,  cfg.ftp_user.c_str());
    nvs_set_str(h, NVS_FTP_PASS,  cfg.ftp_pass.c_str());
    nvs_set_str(h, NVS_FTP_PATH,  cfg.ftp_path.c_str());
    nvs_set_u32(h, NVS_FTP_PORT,  (uint32_t)cfg.ftp_port);

    nvs_commit(h);
    nvs_close(h);
}

// ═════════════════════════════════════════════════════════════════════════════
// WiFi
// ═════════════════════════════════════════════════════════════════════════════

static bool s_wifi_msg_overlay_saved = false;

static void showWifiStatusOverlay(const string &msg, uint8_t level) {
    const unsigned short h = OSD_FONT_H * 3;
    const unsigned short y = OSD::scrAlignCenterY(h);
    string m = msg;
    if (m.length() > (OSD::scrW / 6) - 10) m = m.substr(0, (OSD::scrW / 6) - 10);
    const unsigned short w = (m.length() + 2) * OSD_FONT_W;
    const unsigned short x = OSD::scrAlignCenterX(w);
    OSD::saveBackbufferData(x, y, w, h, true);
    s_wifi_msg_overlay_saved = true;
    OSD::osdCenteredMsg(msg, level, 0);
}

static void hideWifiStatusOverlay() {
    if (s_wifi_msg_overlay_saved) {
        OSD::restoreBackbufferData(true);
        s_wifi_msg_overlay_saved = false;
    }
}

static void wifi_evt_handler(void *, esp_event_base_t base, int32_t id, void *data) {
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_START:
            ESP_LOGI("NET", "WIFI_EVENT_STA_START");
            esp_wifi_connect();
            break;
        case WIFI_EVENT_STA_CONNECTED:
            ESP_LOGI("NET", "WIFI_EVENT_STA_CONNECTED");
            break;
        case WIFI_EVENT_STA_DISCONNECTED: {
            NetworkMenu::wifi_connected = false;
            auto *ev = (wifi_event_sta_disconnected_t *)data;
            int reason = ev ? ev->reason : -1;
            ESP_LOGW("NET", "WIFI_EVENT_STA_DISCONNECTED reason=%d", reason);
            if (s_wifi_awaiting_ip) {
                esp_wifi_connect();
                break;
            }
            if (s_wifi_eg) xEventGroupSetBits(s_wifi_eg, WIFI_FAIL_BIT);
            break;
        }
        default:
            break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        auto *ev = (ip_event_got_ip_t *)data;
        if (ev) {
            ESP_LOGI("NET", "GOT_IP: " IPSTR, IP2STR(&ev->ip_info.ip));
        }
        NetworkMenu::wifi_connected = true;
        if (s_wifi_eg) xEventGroupSetBits(s_wifi_eg, WIFI_CONN_BIT);
    }
}

bool NetworkMenu::wifiInitOnce() {
    if (s_wifi_stack_ready) return true;

    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE("NET", "esp_netif_init: %s", esp_err_to_name(err));
        return false;
    }

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE("NET", "esp_event_loop_create_default: %s", esp_err_to_name(err));
        return false;
    }

    if (!s_netif_sta_ready) {
        if (esp_netif_get_handle_from_ifkey("WIFI_STA_DEF") == nullptr) {
            if (esp_netif_create_default_wifi_sta() == nullptr) {
                ESP_LOGE("NET", "esp_netif_create_default_wifi_sta failed");
                return false;
            }
        }
        s_netif_sta_ready = true;
    }

    wifi_init_config_t icfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&icfg);
    if (err == ESP_ERR_INVALID_STATE) {
        ESP_LOGW("NET", "esp_wifi_init: already initialized");
    } else if (err != ESP_OK) {
        ESP_LOGE("NET", "esp_wifi_init: %s (reduza buffers WiFi no sdkconfig se malloc falhar)", esp_err_to_name(err));
        return false;
    }

    if (!s_wifi_handlers_registered) {
        esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,    wifi_evt_handler, nullptr);
        esp_event_handler_register(IP_EVENT,   IP_EVENT_STA_GOT_IP, wifi_evt_handler, nullptr);
        s_wifi_handlers_registered = true;
    }

    esp_wifi_set_mode(WIFI_MODE_STA);
    s_wifi_stack_ready = true;
    return true;
}

void NetworkMenu::wifiPauseForSd() {
    if (s_wifi_pause_depth == 0) {
        if (s_wifi_stack_ready) {
            esp_err_t err = esp_wifi_stop();
            if (err == ESP_OK) {
                s_wifi_stopped_for_sd = true;
                wifi_connected = false;
            } else if (err == ESP_ERR_WIFI_NOT_STARTED) {
                s_wifi_stopped_for_sd = false;
            }
        }
    }
    s_wifi_pause_depth++;
}

void NetworkMenu::wifiResumeAfterSd() {
    if (s_wifi_pause_depth <= 0) return;
    s_wifi_pause_depth--;
    if (s_wifi_pause_depth > 0) return;
    if (!s_wifi_stopped_for_sd) return;

    s_wifi_stopped_for_sd = false;
    esp_wifi_start();
    /* connect em WIFI_EVENT_STA_START */
}

bool NetworkMenu::isNetworkSession() {
    return s_network_session;
}

void NetworkMenu::prepareForNetwork() {
    s_network_session = true;
    s_wifi_pause_depth = 0;
    s_wifi_stopped_for_sd = false;
    if (FileUtils::SDReady) {
        ESP_LOGI("NET", "Desmontando SD para libertar SPI (WiFi/rede)");
        FileUtils::unmountSDCard();
        s_sd_released_for_net = true;
        vTaskDelay(pdMS_TO_TICKS(100));
    } else {
        ESP_LOGI("NET", "Menu rede (SD ja desmontado)");
    }
}

/*void NetworkMenu::restoreAfterNetwork() {
    s_network_session = false;
    if (!s_sd_released_for_net) return;
    s_sd_released_for_net = false;
    ESP_LOGI("NET", "Remontando SD apos menu rede");
    FileUtils::initFileSystem();
}*/

void NetworkMenu::restoreAfterNetwork() {
    ESP_LOGI("NET", "restoreAfterNetwork: s_sd_released=%d s_network=%d SDReady=%d",
        s_sd_released_for_net ? 1 : 0,
        s_network_session ? 1 : 0,
        FileUtils::SDReady ? 1 : 0);
    s_network_session = false;
    if (!s_sd_released_for_net) return;
    s_sd_released_for_net = false;
    ESP_LOGI("NET", "Remontando SD...");
    FileUtils::initFileSystem();
    ESP_LOGI("NET", "SDReady apos remonte: %d", FileUtils::SDReady ? 1 : 0);
}

bool NetworkMenu::wifiConnect(const string &ssid, const string &pass) {
    if (wifi_connected && wifiHasIp()) return true;

    if (FileUtils::SDReady && !s_network_session) {
        prepareForNetwork();
    }

    const string clean_ssid = sanitizeWifiText(ssid, 32);
    const string clean_pass = sanitizeWifiText(pass, 64);
    if (clean_ssid.empty()) {
        ESP_LOGE("NET", "SSID vazio ou invalido");
        return false;
    }
    if (!wifiInitOnce()) return false;

    showWifiStatusOverlay(NET_MSG_WIFI_CONNECTING[Config::lang], LEVEL_INFO);

    s_wifi_eg = xEventGroupCreate();
    if (!s_wifi_eg) {
        ESP_LOGE("NET", "EventGroup create failed");
        hideWifiStatusOverlay();
        return false;
    }
    xEventGroupClearBits(s_wifi_eg, WIFI_CONN_BIT | WIFI_FAIL_BIT);
    s_wifi_awaiting_ip = true;

    wifi_config_t wcfg = {};
    memset(&wcfg, 0, sizeof(wcfg));
    memcpy(wcfg.sta.ssid, clean_ssid.c_str(), clean_ssid.length());
    memcpy(wcfg.sta.password, clean_pass.c_str(), clean_pass.length());
    if (clean_pass.empty()) {
        wcfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
    } else {
        wcfg.sta.threshold.authmode = WIFI_AUTH_WPA_PSK;
    }

    esp_wifi_set_ps(WIFI_PS_NONE);

    ESP_LOGI("NET", "Conectando SSID='%s' len=%u (pass %s)",
        clean_ssid.c_str(), (unsigned)clean_ssid.length(),
        clean_pass.empty() ? "vazia" : "ok");

    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &wcfg);
    if (err != ESP_OK) {
        ESP_LOGE("NET", "esp_wifi_set_config: %s", esp_err_to_name(err));
        s_wifi_awaiting_ip = false;
        vEventGroupDelete(s_wifi_eg);
        s_wifi_eg = nullptr;
        hideWifiStatusOverlay();
        return false;
    }

    err = esp_wifi_start();
    if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
        ESP_LOGE("NET", "esp_wifi_start: %s", esp_err_to_name(err));
        s_wifi_awaiting_ip = false;
        vEventGroupDelete(s_wifi_eg);
        s_wifi_eg = nullptr;
        hideWifiStatusOverlay();
        return false;
    }

    vTaskDelay(pdMS_TO_TICKS(100));
    esp_wifi_connect();

    EventBits_t bits = xEventGroupWaitBits(s_wifi_eg,
        WIFI_CONN_BIT,
        pdTRUE, pdFALSE,
        pdMS_TO_TICKS(25000));

    s_wifi_awaiting_ip = false;
    vEventGroupDelete(s_wifi_eg);
    s_wifi_eg = nullptr;

    hideWifiStatusOverlay();

    if ((bits & WIFI_CONN_BIT) || wifiHasIp()) {
        wifi_connected = true;
        ESP_LOGI("NET", "WiFi conectado com IP");
        return true;
    }

    ESP_LOGW("NET", "WiFi timeout (25s) sem IP");
    return false;
}

bool NetworkMenu::ensureWifiReady(const string &ssid, const string &pass) {
    if (wifi_connected && wifiHasIp()) {
        esp_netif_ip_info_t ip;
        esp_netif_get_ip_info(esp_netif_get_handle_from_ifkey("WIFI_STA_DEF"), &ip);
        ESP_LOGI("NET", "WiFi ativo " IPSTR, IP2STR(&ip.ip));
        return true;
    }
    if (wifi_connected && !wifiHasIp()) {
        ESP_LOGW("NET", "WiFi sem IP, reconectando");
        wifi_connected = false;
    }
    return wifiConnect(ssid, pass);
}

// ═════════════════════════════════════════════════════════════════════════════
// FTP Client (modo passivo)
// ═════════════════════════════════════════════════════════════════════════════

static void ftpSetSocketTimeouts(int sock, int sec) {
    struct timeval tv = { .tv_sec = sec, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

static bool ftpIsPrivateIPv4(uint32_t addr) {
    uint8_t a = addr & 0xff;
    uint8_t b = (addr >> 8) & 0xff;
    if (a == 10) return true;
    if (a == 172 && b >= 16 && b <= 31) return true;
    if (a == 192 && b == 168) return true;
    return false;
}

static bool ftpTcpConnectTimeout(int sock, const struct sockaddr_in *sa, int timeout_sec) {
    int flags = fcntl(sock, F_GETFL, 0);
    if (flags >= 0) fcntl(sock, F_SETFL, flags | O_NONBLOCK);

    int r = connect(sock, (struct sockaddr *)sa, sizeof(*sa));
    if (r == 0) {
        if (flags >= 0) fcntl(sock, F_SETFL, flags);
        return true;
    }
    if (errno != EINPROGRESS) {
        if (flags >= 0) fcntl(sock, F_SETFL, flags);
        return false;
    }

    fd_set wfds;
    FD_ZERO(&wfds);
    FD_SET(sock, &wfds);
    struct timeval tv = { .tv_sec = timeout_sec, .tv_usec = 0 };
    r = select(sock + 1, nullptr, &wfds, nullptr, &tv);
    if (r <= 0) {
        if (flags >= 0) fcntl(sock, F_SETFL, flags);
        return false;
    }

    int err = 0;
    socklen_t elen = sizeof(err);
    getsockopt(sock, SOL_SOCKET, SO_ERROR, &err, &elen);
    if (flags >= 0) fcntl(sock, F_SETFL, flags);
    return err == 0;
}

bool NetworkMenu::ftpReadLine(int sock, char *buf, int maxlen) {
    int pos = 0; char c;
    while (pos < maxlen - 1) {
        if (recv(sock, &c, 1, 0) <= 0) return false;
        if (c == '\n') break;
        if (c != '\r') buf[pos++] = c;
    }
    buf[pos] = 0;
    return true;
}

int NetworkMenu::ftpGetResponse(int sock, char *rbuf, int rlen) {
    int code = 0;
    while (true) {
        if (!ftpReadLine(sock, rbuf, rlen)) return -1;
        if ((int)strlen(rbuf) >= 4 && rbuf[3] == ' ') {
            code = atoi(rbuf);
            break;
        }
    }
    return code;
}

bool NetworkMenu::ftpSendCmd(int sock, const char *cmd) {
    char buf[300];
    snprintf(buf, sizeof(buf), "%s\r\n", cmd);
    return send(sock, buf, strlen(buf), 0) > 0;
}

static bool ftpResolveHost(const string &host, int port, struct sockaddr_in *sa) {
    memset(sa, 0, sizeof(*sa));
    sa->sin_family = AF_INET;
    sa->sin_port = htons((uint16_t)port);

    if (inet_aton(host.c_str(), &sa->sin_addr)) {
        ESP_LOGI("FTP", "Host e IP: %s", host.c_str());
        return true;
    }

    struct addrinfo hints = {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    char portstr[8];
    snprintf(portstr, sizeof(portstr), "%d", port);

    ESP_LOGI("FTP", "DNS: resolvendo '%s'...", host.c_str());
    vTaskDelay(1);

    struct addrinfo *res = nullptr;
    int err = getaddrinfo(host.c_str(), portstr, &hints, &res);
    if (err != 0 || !res) {
        ESP_LOGE("FTP", "DNS falhou err=%d host='%s'", err, host.c_str());
        return false;
    }

    struct sockaddr_in *addr = (struct sockaddr_in *)res->ai_addr;
    sa->sin_addr = addr->sin_addr;
    ESP_LOGI("FTP", "DNS OK -> %s", inet_ntoa(sa->sin_addr));
    freeaddrinfo(res);
    return true;
}

int NetworkMenu::ftpConnect(const string &host, int port,
                            const string &user, const string &pass) {
    const string h = sanitizeHost(host);
    if (h.empty()) {
        ESP_LOGE("FTP", "Host FTP vazio");
        return -1;
    }

    ESP_LOGI("FTP", "Ligando a %s:%d user='%s'", h.c_str(), port, user.c_str());

    struct sockaddr_in sa = {};
    if (!ftpResolveHost(h, port, &sa)) return -1;

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        ESP_LOGE("FTP", "socket() errno=%d", errno);
        return -1;
    }

    ftpSetSocketTimeouts(sock, 30);

    ESP_LOGI("FTP", "TCP connect %s:%d ...", inet_ntoa(sa.sin_addr), port);
    vTaskDelay(pdMS_TO_TICKS(200));

    if (!ftpTcpConnectTimeout(sock, &sa, 20)) {
        int e = errno;
        ESP_LOGE("FTP", "TCP connect errno=%d (%s)", e, strerror(e));
        if (e == EHOSTUNREACH || e == ETIMEDOUT || e == 113 || e == 110) {
            ESP_LOGE("FTP", "Sem rota ate o servidor (FTP externo/porta 21 bloqueada?)");
        }
        close(sock);
        return -1;
    }
    ESP_LOGI("FTP", "TCP OK, aguardando banner 220");

    int code = ftpGetResponse(sock, ftp_resp, sizeof(ftp_resp));
    ESP_LOGI("FTP", "Banner: %d %s", code, ftp_resp);
    if (code != 220) {
        close(sock);
        return -1;
    }

    char cmd[128];
    snprintf(cmd, sizeof(cmd), "USER %s", user.c_str());
    ftpSendCmd(sock, cmd);
    code = ftpGetResponse(sock, ftp_resp, sizeof(ftp_resp));
    ESP_LOGI("FTP", "USER: %d %s", code, ftp_resp);

    if (code == 331) {
        snprintf(cmd, sizeof(cmd), "PASS %s", pass.c_str());
        ftpSendCmd(sock, cmd);
        code = ftpGetResponse(sock, ftp_resp, sizeof(ftp_resp));
        ESP_LOGI("FTP", "PASS: %d %s", code, ftp_resp);
    }
    if (code != 230) {
        ESP_LOGE("FTP", "Login falhou codigo %d", code);
        close(sock);
        return -1;
    }

    ftpSendCmd(sock, "TYPE I");
    ftpGetResponse(sock, ftp_resp, sizeof(ftp_resp));
    ESP_LOGI("FTP", "Sessao FTP aberta");
    return sock;
}

void NetworkMenu::ftpDisconnect(int sock) {
    if (sock >= 0) {
        ftpSendCmd(sock, "QUIT");
        ftpGetResponse(sock, ftp_resp, sizeof(ftp_resp));
        close(sock);
    }
}

int NetworkMenu::ftpPasv(int ctrl_sock) {
    ftpSendCmd(ctrl_sock, "PASV");
    int code = ftpGetResponse(ctrl_sock, ftp_resp, sizeof(ftp_resp));
    ESP_LOGI("FTP", "PASV: %d %s", code, ftp_resp);
    if (code != 227) return -1;

    char *p = strchr(ftp_resp, '(');
    if (!p) return -1;
    int h1, h2, h3, h4, p1, p2;
    if (sscanf(p, "(%d,%d,%d,%d,%d,%d)", &h1, &h2, &h3, &h4, &p1, &p2) != 6) return -1;

    char pasv_ip[32];
    snprintf(pasv_ip, sizeof(pasv_ip), "%d.%d.%d.%d", h1, h2, h3, h4);

    struct sockaddr_in da = {};
    da.sin_family = AF_INET;
    da.sin_port = htons(p1 * 256 + p2);
    inet_aton(pasv_ip, &da.sin_addr);

    struct sockaddr_in peer = {};
    socklen_t plen = sizeof(peer);
    if (getpeername(ctrl_sock, (struct sockaddr *)&peer, &plen) == 0) {
        if (ftpIsPrivateIPv4(da.sin_addr.s_addr)) {
            ESP_LOGW("FTP", "PASV IP privado %d.%d.%d.%d -> usa servidor %s",
                h1, h2, h3, h4, inet_ntoa(peer.sin_addr));
            da.sin_addr = peer.sin_addr;
        }
    }

    int dsock = socket(AF_INET, SOCK_STREAM, 0);
    if (dsock < 0) return -1;

    ftpSetSocketTimeouts(dsock, 30);

    ESP_LOGI("FTP", "PASV data connect %s:%d", inet_ntoa(da.sin_addr), ntohs(da.sin_port));
    if (!ftpTcpConnectTimeout(dsock, &da, 15)) {
        ESP_LOGE("FTP", "PASV data connect falhou errno=%d", errno);
        close(dsock);
        return -1;
    }
    ESP_LOGI("FTP", "PASV data channel OK");
    return dsock;
}

vector<string> NetworkMenu::ftpList(int ctrl_sock, const string &path) {
    vector<string> entries;

    ESP_LOGI("FTP", "Listar path='%s'", path.c_str());

    char cmd[512];
    snprintf(cmd, sizeof(cmd), "CWD %s", path.c_str());
    ftpSendCmd(ctrl_sock, cmd);
    int code = ftpGetResponse(ctrl_sock, ftp_resp, sizeof(ftp_resp));
    ESP_LOGI("FTP", "CWD: %d %s", code, ftp_resp);
    if (code != 250 && code != 200) return entries;

    int dsock = ftpPasv(ctrl_sock);
    if (dsock < 0) return entries;

    ftpSendCmd(ctrl_sock, "NLST");
    code = ftpGetResponse(ctrl_sock, ftp_resp, sizeof(ftp_resp));
    ESP_LOGI("FTP", "NLST: %d %s", code, ftp_resp);
    if (code != 125 && code != 150) {
        close(dsock);
        return entries;
    }

    char lbuf[512];
    int pos = 0;
    char c;
    int lines = 0;
    while (true) {
        if (recv(dsock, &c, 1, 0) <= 0) break;
        if (c == '\n') {
            if (pos > 0 && lbuf[pos - 1] == '\r') pos--;
            lbuf[pos] = 0;
            if (pos > 0) {
                string entry(lbuf);
                size_t sl = entry.rfind('/');
                if (sl != string::npos) entry = entry.substr(sl + 1);
                if (!entry.empty()) entries.push_back(entry);
                lines++;
            }
            pos = 0;
            if ((lines & 0x1f) == 0) vTaskDelay(1);
        } else {
            if (pos < 511) lbuf[pos++] = c;
        }
    }
    close(dsock);
    code = ftpGetResponse(ctrl_sock, ftp_resp, sizeof(ftp_resp));
    ESP_LOGI("FTP", "NLST fim: %d %s (%u entradas)", code, ftp_resp, (unsigned)entries.size());
    return entries;
}

bool NetworkMenu::ftpDownload(int ctrl_sock, const string &remote, const string &local) {
    const string fname = local.substr(local.rfind('/') + 1);
    OSD::progressDialog("Download", fname, 0, 0);

    ESP_LOGI("FTP", "Download remoto: %s", remote.c_str());
    ESP_LOGI("FTP", "Destino local:   %s", local.c_str());

    long total = 0;
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "SIZE %s", remote.c_str());
    ftpSendCmd(ctrl_sock, cmd);
    int code = ftpGetResponse(ctrl_sock, ftp_resp, sizeof(ftp_resp));
    ESP_LOGI("FTP", "SIZE resp: %d | %s", code, ftp_resp);
    if (code == 213) {
        total = atol(ftp_resp + 4);
        ESP_LOGI("FTP", "Tamanho remoto: %ld bytes", total);
    } else {
        ESP_LOGW("FTP", "SIZE indisponivel — download por blocos");
    }

    if (total > (long)netMaxFtpBuffer()) {
        ESP_LOGW("FTP", "Arquivo grande (%ld bytes), gravacao por blocos", total);
    }

    int dsock = ftpPasv(ctrl_sock);
    if (dsock < 0) {
        ESP_LOGE("FTP", "PASV falhou para RETR");
        OSD::progressDialog("Download", "", 0, 2);
        return false;
    }
    ftpSetSocketTimeouts(dsock, 30);

    snprintf(cmd, sizeof(cmd), "RETR %s", remote.c_str());
    ftpSendCmd(ctrl_sock, cmd);
    code = ftpGetResponse(ctrl_sock, ftp_resp, sizeof(ftp_resp));
    ESP_LOGI("FTP", "RETR resp: %d | %s", code, ftp_resp);
    if (code != 125 && code != 150) {
        ESP_LOGE("FTP", "RETR rejeitado: %d", code);
        close(dsock);
        OSD::progressDialog("Download", "", 0, 2);
        return false;
    }

    const bool net_sess = isNetworkSession();
    const bool use_ram_buf = net_sess && total > 0 && (size_t)total <= netMaxFtpBuffer();

    long received = 0;
    bool ok = false;

    if (use_ram_buf) {
        uint8_t *buf = netAllocBuf((size_t)total);
        if (!buf) {
            ESP_LOGE("FTP", "Sem memoria para %ld bytes", total);
            close(dsock);
            OSD::progressDialog("Download", "", 0, 2);
            return false;
        }

        uint8_t chunk[512];
        int n;
        while ((n = recv(dsock, chunk, sizeof(chunk), 0)) > 0) {
            memcpy(buf + received, chunk, n);
            received += n;
            vTaskDelay(1);
            int pct = (int)((received * 100) / total);
            OSD::progressDialog("Download", fname, pct, 1);
        }
        close(dsock);
        ftpGetResponse(ctrl_sock, ftp_resp, sizeof(ftp_resp));

        if (total > 0 && received != total) {
            ESP_LOGW("FTP", "Download incompleto: %ld/%ld bytes", received, total);
        } else if (received > 0) {
            bool first = true;
            ok = netFlushToSd(local, buf, (size_t)received, first);
        }
        netFreeBuf(buf);
    } else if (net_sess) {
        uint8_t *accum = netAllocBuf(FTP_SD_FLUSH);
        if (!accum) {
            ESP_LOGE("FTP", "Sem memoria para buffer de gravacao");
            close(dsock);
            OSD::progressDialog("Download", "", 0, 2);
            return false;
        }

        uint8_t chunk[512];
        size_t acc_len = 0;
        bool first = true;
        int n;

        while ((n = recv(dsock, chunk, sizeof(chunk), 0)) > 0) {
            size_t off = 0;
            while (off < (size_t)n) {
                size_t space = FTP_SD_FLUSH - acc_len;
                size_t copy = (size_t)n - off;
                if (copy > space) copy = space;
                memcpy(accum + acc_len, chunk + off, copy);
                acc_len += copy;
                off += copy;
                if (acc_len >= FTP_SD_FLUSH) {
                    if (!netFlushToSd(local, accum, acc_len, first)) {
                        netFreeBuf(accum);
                        close(dsock);
                        OSD::progressDialog("Download", "", 0, 2);
                        return false;
                    }
                    acc_len = 0;
                }
            }
            received += n;
            vTaskDelay(1);
            if (total > 0) {
                int pct = (int)((received * 100) / total);
                OSD::progressDialog("Download", fname, pct, 1);
            }
        }
        close(dsock);
        ftpGetResponse(ctrl_sock, ftp_resp, sizeof(ftp_resp));
        if (acc_len > 0) ok = netFlushToSd(local, accum, acc_len, first);
        else ok = (received > 0 && !first);
        netFreeBuf(accum);
    } else {
        if (!FileUtils::isSDReady()) {
            ESP_LOGE("FTP", "SD nao disponivel para escrita");
            close(dsock);
            OSD::progressDialog("Download", "", 0, 2);
            return false;
        }

        FILE *f = fopen(local.c_str(), "wb");
        if (!f) {
            ESP_LOGE("FTP", "fopen falhou: %s (errno %d)", local.c_str(), errno);
            close(dsock);
            OSD::progressDialog("Download", "", 0, 2);
            return false;
        }

        uint8_t buf[512];
        int n;
        const long yield_interval = 32768;
        while ((n = recv(dsock, buf, sizeof(buf), 0)) > 0) {
            fwrite(buf, 1, n, f);
            received += n;
            if ((received % yield_interval) < n) vTaskDelay(1);
            if (total > 0) {
                int pct = (int)((received * 100) / total);
                OSD::progressDialog("Download", fname, pct, 1);
            }
        }
        fflush(f);
        fclose(f);
        close(dsock);
        ftpGetResponse(ctrl_sock, ftp_resp, sizeof(ftp_resp));
        ok = (received > 0);
    }

    const bool complete = (total <= 0) || (received == total);
    ESP_LOGI("FTP", "Download concluido: %ld bytes (%s)%s",
        received, ok ? "OK" : "falhou", complete ? "" : " [incompleto]");
    if (ok) OSD::progressDialog("Download", fname, 100, 1);
    OSD::progressDialog("Download", "", 0, 2);
    return ok && received > 0 && complete;
}

// ═════════════════════════════════════════════════════════════════════════════
// Menu: Configurar WiFi
// ═════════════════════════════════════════════════════════════════════════════

void NetworkMenu::wifiConfig() {
    NetConfig cfg = loadConfig();

    string new_ssid = OSD::input(3, 3,
        NET_LBL_SSID[Config::lang], cfg.wifi_ssid,
        6, 32, zxColor(7,1), zxColor(1,0), false);
    if (new_ssid.empty()) return; // ESC
    cfg.wifi_ssid = sanitizeWifiText(new_ssid, 32);
    if (cfg.wifi_ssid.empty()) return;

    string new_pass = OSD::input(3, 4,
        NET_LBL_PASS[Config::lang], cfg.wifi_pass,
        6, 64, zxColor(7,1), zxColor(1,0), false);
    cfg.wifi_pass = sanitizeWifiText(new_pass, 64); // pode ser vazia (rede aberta)

    saveConfig(cfg);
    OSD::osdCenteredMsg(NET_MSG_WIFI_SAVED[Config::lang], LEVEL_OK, 1000);

    if (wifiConnect(cfg.wifi_ssid, cfg.wifi_pass))
        OSD::osdCenteredMsg(NET_MSG_WIFI_OK[Config::lang],   LEVEL_OK,    1500);
    else
        OSD::osdCenteredMsg(NET_MSG_WIFI_FAIL[Config::lang], LEVEL_ERROR, 2000);
}

// ═════════════════════════════════════════════════════════════════════════════
// Menu: Configurar FTP
// ═════════════════════════════════════════════════════════════════════════════

void NetworkMenu::ftpConfig() {
    NetConfig cfg = loadConfig();

    string host = OSD::input(3, 3, NET_LBL_HOST[Config::lang],
        cfg.ftp_host, 20, 64, zxColor(7,1), zxColor(1,0), false);
    if (host.empty()) return; // ESC
    cfg.ftp_host = sanitizeHost(host);
    if (cfg.ftp_host.empty()) return;

    string user = OSD::input(3, 4, NET_LBL_USER[Config::lang],
        cfg.ftp_user, 20, 32, zxColor(7,1), zxColor(1,0), false);
    if (!user.empty()) cfg.ftp_user = user;

    string pass = OSD::input(3, 5, NET_LBL_FTPPASS[Config::lang],
        cfg.ftp_pass, 20, 64, zxColor(7,1), zxColor(1,0), false);
    cfg.ftp_pass = pass;

    string path = OSD::input(3, 6, NET_LBL_PATH[Config::lang],
        cfg.ftp_path, 20, 128, zxColor(7,1), zxColor(1,0), false);
    if (!path.empty()) cfg.ftp_path = path;

    saveConfig(cfg);
    OSD::osdCenteredMsg(NET_MSG_FTP_SAVED[Config::lang], LEVEL_OK, 1000);
}

// ═════════════════════════════════════════════════════════════════════════════
// Menu: Browser FTP
// ═════════════════════════════════════════════════════════════════════════════

void NetworkMenu::ftpBrowser() {
    NetConfig cfg = loadConfig();

    ESP_LOGI("FTP", "ftpBrowser host='%s' port=%d", cfg.ftp_host.c_str(), cfg.ftp_port);

    if (cfg.ftp_host.empty()) {
        OSD::osdCenteredMsg(NET_MSG_FTP_NOCONFIG[Config::lang], LEVEL_WARN, 2000);
        return;
    }

    if (!ensureWifiReady(cfg.wifi_ssid, cfg.wifi_pass)) {
        ESP_LOGE("FTP", "WiFi indisponivel");
        OSD::osdCenteredMsg(NET_MSG_FTP_NOWIFI[Config::lang], LEVEL_ERROR, 2000);
        return;
    }

    vTaskDelay(pdMS_TO_TICKS(300));

    showWifiStatusOverlay(NET_MSG_FTP_CONNECTING[Config::lang], LEVEL_INFO);

    int ctrl = ftpConnect(cfg.ftp_host, cfg.ftp_port, cfg.ftp_user, cfg.ftp_pass);

    hideWifiStatusOverlay();

    if (ctrl < 0) {
        ESP_LOGE("FTP", "ftpConnect falhou");
        OSD::osdCenteredMsg(NET_MSG_FTP_FAIL[Config::lang], LEVEL_ERROR, 2000);
        return;
    }

    string cur_path = cfg.ftp_path;
    // Remove trailing slash (exceto raiz)
    while (cur_path.size() > 1 && cur_path.back() == '/') cur_path.pop_back();

    while (true) {
        showWifiStatusOverlay(NET_MSG_FTP_LISTING[Config::lang], LEVEL_INFO);

        ESP_LOGI("FTP", "Heap livre antes menuRun: %d bytes", 
        heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));

        vector<string> entries = ftpList(ctrl, cur_path);

        hideWifiStatusOverlay();

        if (entries.empty()) {
            OSD::osdCenteredMsg(NET_MSG_FTP_EMPTYDIR[Config::lang], LEVEL_WARN, 1500);
            // Sobe um nível automaticamente
            size_t sl = cur_path.rfind('/');
            if (sl == 0 || sl == string::npos) break;
            cur_path = cur_path.substr(0, sl);
            continue;
        }

        // Monta string do menu — título + entradas
        string title = cur_path;
        if (title.size() > 36) title = ".." + title.substr(title.size() - 34);
        string ftp_menu = title + "\n";

        bool has_parent = (cur_path.size() > 1 && cur_path != cfg.ftp_path);
        if (has_parent) ftp_menu += "../\n";

        for (const string &e : entries)
            ftp_menu += e.substr(0, 38) + "\n";

        OSD::menu_level    = 1;
        OSD::menu_curopt   = 1;
        OSD::menu_saverect = true;

        uint8_t sel = OSD::menuRun(ftp_menu);
        if (sel == 0) break; // ESC sai do browser

        int idx = sel - 1;

        if (has_parent) {
            if (idx == 0) {
                // Sobe um nível
                size_t sl = cur_path.rfind('/');
                cur_path = (sl == 0) ? "/" : cur_path.substr(0, sl);
                continue;
            }
            idx--;
        }

        if (idx < 0 || idx >= (int)entries.size()) continue;

        string selected  = entries[idx];
        string full_path = cur_path + (cur_path.back() == '/' ? "" : "/") + selected;

        // Tenta entrar como diretório (CWD)
        char cmd[512];
        snprintf(cmd, sizeof(cmd), "CWD %s", full_path.c_str());
        ftpSendCmd(ctrl, cmd);
        int code = ftpGetResponse(ctrl, ftp_resp, sizeof(ftp_resp));

        if (code == 250) {
            // É um diretório — navega
            cur_path = full_path;
        } else {
            // É arquivo — pergunta se quer baixar
            uint8_t res = OSD::msgDialog(
                NET_DLG_DOWNLOAD[Config::lang],
                selected.substr(0, 34),
                MSGDIALOG_YESNO
            );
            if (res == DLG_YES) {
                const string dest = netFtpDestPath(selected);

                ESP_LOGI("FTP", "MountPoint: [%s]", FileUtils::MountPoint.c_str());
                ESP_LOGI("FTP", "TAP_Path:   [%s]", FileUtils::TAP_Path.c_str());
                ESP_LOGI("FTP", "SNA_Path:   [%s]", FileUtils::SNA_Path.c_str());
                ESP_LOGI("FTP", "dest final: [%s]", dest.c_str());

                if (ftpDownload(ctrl, full_path, dest)) {
                    OSD::osdCenteredMsg(NET_MSG_DOWNLOAD_OK[Config::lang], LEVEL_OK, 1500);

                    if (netFileIsGame(dest)) {
                        uint8_t load = OSD::msgDialog(
                            NET_DLG_LOAD_GAME[Config::lang],
                            selected.substr(0, 34),
                            MSGDIALOG_YESNO
                        );
                      if (load == DLG_YES) {
                        restoreAfterNetwork();
                        
                        // Debug SD
                        ESP_LOGI("FTP", "SDReady=%d", FileUtils::SDReady ? 1 : 0);
                        struct stat st;
                        if (stat(dest.c_str(), &st) == 0) {
                            ESP_LOGI("FTP", "Arquivo existe: %s (%ld bytes)", dest.c_str(), st.st_size);
                        } else {
                            ESP_LOGE("FTP", "Arquivo NAO existe: %s errno=%d", dest.c_str(), errno);
                        }
                        DIR *dir = opendir("/sd");
                        if (dir) {
                            struct dirent *e;
                            while ((e = readdir(dir)) != NULL)
                                ESP_LOGI("FTP", "SD: %s", e->d_name);
                            closedir(dir);
                        } else {
                            ESP_LOGE("FTP", "opendir /sd falhou errno=%d", errno);
                        }

                        if (netLoadDownloadedFile(dest)) {
                            s_exit_osd_after_load = true;
                            OSD::restoreBackbufferData();
                        } else {
                            OSD::osdCenteredMsg(NET_MSG_DOWNLOAD_FAIL[Config::lang], LEVEL_ERROR, 2000);
                        }
                        ftpDisconnect(ctrl);
                        return;
                        }
                    }
                } else {
                    OSD::osdCenteredMsg(NET_MSG_DOWNLOAD_FAIL[Config::lang], LEVEL_ERROR, 2000);
                }
            }
        }
    }

    ftpDisconnect(ctrl);
}
