/*

ESPectrum Fork — NetworkMenu.h
Menu de Acesso à Rede: WiFi + cliente FTP browser.
Configurações salvas em NVS, namespace "storage" (igual ao ESPConfig).

Coloque em: include/NetworkMenu.h

*/

#ifndef ESPECTRUM_NETWORKMENU_H
#define ESPECTRUM_NETWORKMENU_H

#include <string>
#include <vector>
using namespace std;

class NetworkMenu {
public:
    // Chamados pelo dispatcher em OSDMain.cpp (opt == 7 do menu principal)
    static void wifiConfig();
    static void ftpConfig();
    static void ftpBrowser();

    static bool wifi_connected; // exposto para o event handler

private:
    // ── Config NVS ──────────────────────────────────────────────────────────
    struct NetConfig {
        string wifi_ssid = "";
        string wifi_pass = "";
        string ftp_host  = "";
        string ftp_user  = "anonymous";
        string ftp_pass  = "guest";
        string ftp_path  = "/";
        int    ftp_port  = 21;
    };

    static NetConfig loadConfig();
    static void      saveConfig(const NetConfig &cfg);

    // ── WiFi ─────────────────────────────────────────────────────────────────
    static bool wifiConnect(const string &ssid, const string &pass);

    // ── FTP Client ───────────────────────────────────────────────────────────
    static int  ftpConnect(const string &host, int port,
                           const string &user, const string &pass);
    static void ftpDisconnect(int sock);
    static int  ftpPasv(int ctrl_sock);
    static vector<string> ftpList(int ctrl_sock, const string &path);
    static bool ftpDownload(int ctrl_sock, const string &remote,
                            const string &local);
    static bool ftpReadLine(int sock, char *buf, int maxlen);
    static int  ftpGetResponse(int sock, char *rbuf, int rlen);
    static bool ftpSendCmd(int sock, const char *cmd);
};

#endif // ESPECTRUM_NETWORKMENU_H
