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
#include "Video.h"
#include "messages.h"

#include "nvs.h"
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"

#include <stdio.h>
#include <string.h>
#include <cerrno>
#include <string>
#include <vector>
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

// ═════════════════════════════════════════════════════════════════════════════
// NVS helpers
// ═════════════════════════════════════════════════════════════════════════════

static string nvsGetStr(nvs_handle_t h, const char *key, const char *def = "") {
    size_t len = 0;
    if (nvs_get_str(h, key, nullptr, &len) != ESP_OK || len == 0) return def;
    char *buf = new char[len];
    nvs_get_str(h, key, buf, &len);
    string s(buf, len - 1); // descarta o null terminator
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

    cfg.wifi_ssid = nvsGetStr(h, NVS_WIFI_SSID, "");
    cfg.wifi_pass = nvsGetStr(h, NVS_WIFI_PASS, "");
    cfg.ftp_host  = nvsGetStr(h, NVS_FTP_HOST,  "");
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

    nvs_set_str(h, NVS_WIFI_SSID, cfg.wifi_ssid.c_str());
    nvs_set_str(h, NVS_WIFI_PASS, cfg.wifi_pass.c_str());
    nvs_set_str(h, NVS_FTP_HOST,  cfg.ftp_host.c_str());
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

static void wifi_evt_handler(void *, esp_event_base_t base, int32_t id, void *) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        NetworkMenu::wifi_connected = false;
        if (s_wifi_eg) xEventGroupSetBits(s_wifi_eg, WIFI_FAIL_BIT);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        NetworkMenu::wifi_connected = true;
        if (s_wifi_eg) xEventGroupSetBits(s_wifi_eg, WIFI_CONN_BIT);
    }
}

bool NetworkMenu::wifiConnect(const string &ssid, const string &pass) {
    if (wifi_connected) return true;

    OSD::osdCenteredMsg(NET_MSG_WIFI_CONNECTING[Config::lang], LEVEL_INFO, 0);

    s_wifi_eg = xEventGroupCreate();

    static bool netif_ready = false;
    if (!netif_ready) {
        esp_netif_init();
        esp_event_loop_create_default();
        esp_netif_create_default_wifi_sta();
        netif_ready = true;
    }

    wifi_init_config_t icfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&icfg);

    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,    wifi_evt_handler, nullptr);
    esp_event_handler_register(IP_EVENT,   IP_EVENT_STA_GOT_IP, wifi_evt_handler, nullptr);

    wifi_config_t wcfg = {};
    strncpy((char *)wcfg.sta.ssid,     ssid.c_str(), 32);
    strncpy((char *)wcfg.sta.password, pass.c_str(), 64);
    wcfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wcfg);
    esp_wifi_start();
    esp_wifi_connect();

    EventBits_t bits = xEventGroupWaitBits(s_wifi_eg,
        WIFI_CONN_BIT | WIFI_FAIL_BIT,
        pdFALSE, pdFALSE,
        pdMS_TO_TICKS(12000));

    vEventGroupDelete(s_wifi_eg);
    s_wifi_eg = nullptr;

    return (bits & WIFI_CONN_BIT) != 0;
}

// ═════════════════════════════════════════════════════════════════════════════
// FTP Client (modo passivo)
// ═════════════════════════════════════════════════════════════════════════════

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

int NetworkMenu::ftpConnect(const string &host, int port,
                            const string &user, const string &pass) {
    struct hostent *he = gethostbyname(host.c_str());
    if (!he) return -1;

    struct sockaddr_in sa = {};
    sa.sin_family = AF_INET;
    sa.sin_port   = htons(port);
    memcpy(&sa.sin_addr, he->h_addr, he->h_length);

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return -1;

    struct timeval tv = { .tv_sec = 15, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    if (connect(sock, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        close(sock); return -1;
    }

    int code = ftpGetResponse(sock, ftp_resp, sizeof(ftp_resp));
    if (code != 220) { close(sock); return -1; }

    char cmd[128];
    snprintf(cmd, sizeof(cmd), "USER %s", user.c_str());
    ftpSendCmd(sock, cmd);
    code = ftpGetResponse(sock, ftp_resp, sizeof(ftp_resp));

    if (code == 331) {
        snprintf(cmd, sizeof(cmd), "PASS %s", pass.c_str());
        ftpSendCmd(sock, cmd);
        code = ftpGetResponse(sock, ftp_resp, sizeof(ftp_resp));
    }
    if (code != 230) { close(sock); return -1; }

    ftpSendCmd(sock, "TYPE I");
    ftpGetResponse(sock, ftp_resp, sizeof(ftp_resp));
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
    if (code != 227) return -1;

    char *p = strchr(ftp_resp, '(');
    if (!p) return -1;
    int h1,h2,h3,h4,p1,p2;
    if (sscanf(p, "(%d,%d,%d,%d,%d,%d)", &h1,&h2,&h3,&h4,&p1,&p2) != 6) return -1;

    char ip[32];
    snprintf(ip, sizeof(ip), "%d.%d.%d.%d", h1,h2,h3,h4);

    struct sockaddr_in da = {};
    da.sin_family = AF_INET;
    da.sin_port   = htons(p1 * 256 + p2);
    inet_aton(ip, &da.sin_addr);

    int dsock = socket(AF_INET, SOCK_STREAM, 0);
    if (dsock < 0) return -1;

    struct timeval tv = { .tv_sec = 30, .tv_usec = 0 };
    setsockopt(dsock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    if (connect(dsock, (struct sockaddr *)&da, sizeof(da)) < 0) {
        close(dsock); return -1;
    }
    return dsock;
}

vector<string> NetworkMenu::ftpList(int ctrl_sock, const string &path) {
    vector<string> entries;

    char cmd[512];
    snprintf(cmd, sizeof(cmd), "CWD %s", path.c_str());
    ftpSendCmd(ctrl_sock, cmd);
    int code = ftpGetResponse(ctrl_sock, ftp_resp, sizeof(ftp_resp));
    if (code != 250 && code != 200) return entries;

    int dsock = ftpPasv(ctrl_sock);
    if (dsock < 0) return entries;

    ftpSendCmd(ctrl_sock, "NLST");
    code = ftpGetResponse(ctrl_sock, ftp_resp, sizeof(ftp_resp));
    if (code != 125 && code != 150) { close(dsock); return entries; }

    char lbuf[512]; int pos = 0; char c;
    while (true) {
        if (recv(dsock, &c, 1, 0) <= 0) break;
        if (c == '\n') {
            if (pos > 0 && lbuf[pos-1] == '\r') pos--;
            lbuf[pos] = 0;
            if (pos > 0) {
                string entry(lbuf);
                size_t sl = entry.rfind('/');
                if (sl != string::npos) entry = entry.substr(sl + 1);
                if (!entry.empty()) entries.push_back(entry);
            }
            pos = 0;
        } else {
            if (pos < 511) lbuf[pos++] = c;
        }
    }
    close(dsock);
    ftpGetResponse(ctrl_sock, ftp_resp, sizeof(ftp_resp)); // 226
    return entries;
}

bool NetworkMenu::ftpDownload(int ctrl_sock, const string &remote, const string &local) {
    OSD::osdCenteredMsg(NET_MSG_DOWNLOAD[Config::lang], LEVEL_INFO, 0);

    ESP_LOGI("FTP", "Download remoto: %s", remote.c_str());
    ESP_LOGI("FTP", "Destino local:   %s", local.c_str());

    long total = 0;
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "SIZE %s", remote.c_str());
    ftpSendCmd(ctrl_sock, cmd);
    int code = ftpGetResponse(ctrl_sock, ftp_resp, sizeof(ftp_resp));
    ESP_LOGI("FTP", "SIZE resp: %d | %s", code, ftp_resp);
    if (code == 213) total = atol(ftp_resp + 4);

    int dsock = ftpPasv(ctrl_sock);
    if (dsock < 0) {
        ESP_LOGE("FTP", "PASV falhou para RETR");
        return false;
    }

    snprintf(cmd, sizeof(cmd), "RETR %s", remote.c_str());
    ftpSendCmd(ctrl_sock, cmd);
    code = ftpGetResponse(ctrl_sock, ftp_resp, sizeof(ftp_resp));
    ESP_LOGI("FTP", "RETR resp: %d | %s", code, ftp_resp);
    if (code != 125 && code != 150) {
        ESP_LOGE("FTP", "RETR rejeitado: %d", code);
        close(dsock);
        return false;
    }

    // Garante que o SD está montado — mesmo padrão usado em todo o OSDMain.cpp
    if (!FileUtils::isSDReady()) {
        ESP_LOGE("FTP", "SD nao disponivel para escrita");
        close(dsock);
        return false;
    }

    FILE *f = fopen(local.c_str(), "wb");
    if (!f) {
        ESP_LOGE("FTP", "fopen falhou: %s (errno %d)", local.c_str(), errno);
        close(dsock);
        return false;
    }

    uint8_t buf[512]; long received = 0; int n;
    while ((n = recv(dsock, buf, sizeof(buf), 0)) > 0) {
        fwrite(buf, 1, n, f);
        received += n;
        if (total > 0) {
            int pct = (int)((received * 100) / total);
            string fname = local.substr(local.rfind('/') + 1);
            OSD::progressDialog("Download", fname, pct, 0);
        }
    }
    fclose(f);
    close(dsock);
    ftpGetResponse(ctrl_sock, ftp_resp, sizeof(ftp_resp)); // 226

    ESP_LOGI("FTP", "Download concluido: %ld bytes", received);
    if (total > 0) OSD::progressDialog("Download", "", 100, 1);
    return (received > 0);
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
    cfg.wifi_ssid = new_ssid;

    string new_pass = OSD::input(3, 4,
        NET_LBL_PASS[Config::lang], cfg.wifi_pass,
        6, 64, zxColor(7,1), zxColor(1,0), false);
    cfg.wifi_pass = new_pass; // pode ser vazia (rede aberta)

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
        cfg.ftp_host, 6, 64, zxColor(7,1), zxColor(1,0), false);
    if (host.empty()) return; // ESC
    cfg.ftp_host = host;

    string user = OSD::input(3, 4, NET_LBL_USER[Config::lang],
        cfg.ftp_user, 6, 32, zxColor(7,1), zxColor(1,0), false);
    if (!user.empty()) cfg.ftp_user = user;

    string pass = OSD::input(3, 5, NET_LBL_FTPPASS[Config::lang],
        cfg.ftp_pass, 6, 64, zxColor(7,1), zxColor(1,0), false);
    cfg.ftp_pass = pass;

    string path = OSD::input(3, 6, NET_LBL_PATH[Config::lang],
        cfg.ftp_path, 6, 128, zxColor(7,1), zxColor(1,0), false);
    if (!path.empty()) cfg.ftp_path = path;

    saveConfig(cfg);
    OSD::osdCenteredMsg(NET_MSG_FTP_SAVED[Config::lang], LEVEL_OK, 1000);
}

// ═════════════════════════════════════════════════════════════════════════════
// Menu: Browser FTP
// ═════════════════════════════════════════════════════════════════════════════

void NetworkMenu::ftpBrowser() {
    NetConfig cfg = loadConfig();

    if (cfg.ftp_host.empty()) {
        OSD::osdCenteredMsg(NET_MSG_FTP_NOCONFIG[Config::lang], LEVEL_WARN, 2000);
        return;
    }

    if (!wifiConnect(cfg.wifi_ssid, cfg.wifi_pass)) {
        OSD::osdCenteredMsg(NET_MSG_FTP_NOWIFI[Config::lang], LEVEL_ERROR, 2000);
        return;
    }

    OSD::osdCenteredMsg(NET_MSG_FTP_CONNECTING[Config::lang], LEVEL_INFO, 0);

    int ctrl = ftpConnect(cfg.ftp_host, cfg.ftp_port, cfg.ftp_user, cfg.ftp_pass);
    if (ctrl < 0) {
        OSD::osdCenteredMsg(NET_MSG_FTP_FAIL[Config::lang], LEVEL_ERROR, 2000);
        return;
    }

    string cur_path = cfg.ftp_path;
    // Remove trailing slash (exceto raiz)
    while (cur_path.size() > 1 && cur_path.back() == '/') cur_path.pop_back();

    while (true) {
        OSD::osdCenteredMsg(NET_MSG_FTP_LISTING[Config::lang], LEVEL_INFO, 0);

        vector<string> entries = ftpList(ctrl, cur_path);

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
                // Salva na pasta de snapshots do SD
                string dest = FileUtils::MountPoint + FileUtils::SNA_Path;
                if (dest.back() != '/') dest += '/';
                dest += selected;

                ESP_LOGI("FTP", "MountPoint: [%s]", FileUtils::MountPoint.c_str());
                ESP_LOGI("FTP", "SNA_Path:   [%s]", FileUtils::SNA_Path.c_str());
                ESP_LOGI("FTP", "dest final: [%s]", dest.c_str());

                if (ftpDownload(ctrl, full_path, dest))
                    OSD::osdCenteredMsg(NET_MSG_DOWNLOAD_OK[Config::lang],   LEVEL_OK,    1500);
                else
                    OSD::osdCenteredMsg(NET_MSG_DOWNLOAD_FAIL[Config::lang], LEVEL_ERROR, 2000);
            }
        }
    }

    ftpDisconnect(ctrl);
}
