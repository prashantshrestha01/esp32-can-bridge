// ESP32 WiFi <-> CAN bridge — 2011 Ford Fusion.
//
// The ESP32 joins the car's CAN bus via the SN65HVD230 transceiver and serves
// it over its own WiFi: connect your phone to the "FusionCAN" access point,
// open http://192.168.4.1 and you get a live CAN monitor plus a form to
// transmit frames (the replay half of sniff-diff-replay).
//
// Default bus: MS-CAN body bus, 125 kbps, OBD2 pins 3/11 (climate, locks...).
// Set CAN_BITRATE_KBPS to 500 for the HS-CAN powertrain bus (pins 6/14).
// Set CAN_LISTEN_ONLY to 1 for safe sniffing (never transmits).

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "nvs_flash.h"
#include "driver/twai.h"

static const char *TAG = "canbridge";

// ---------------- configuration ----------------
#define WIFI_SSID           "FusionCAN"
#define WIFI_PASS           "fusion1234"    // >= 8 chars
#define TWAI_TX_GPIO        19              // J4 pad 7 -> transceiver TXD
#define TWAI_RX_GPIO        20              // J4 pad 8 -> transceiver RXD
#define CAN_BITRATE_KBPS    125             // 125 = MS-CAN (pins 3/11), 500 = HS-CAN (pins 6/14)
#define CAN_LISTEN_ONLY     0               // 1 = never transmit (safe sniffing)

#define MAX_WS_CLIENTS      4
#define MAX_IDS             256

// ---------------- state ----------------
static httpd_handle_t server = NULL;
static int ws_clients[MAX_WS_CLIENTS];
static SemaphoreHandle_t clients_lock;
static uint32_t seen_ids[MAX_IDS];
static int seen_count = 0;
static uint32_t total_frames = 0;

static bool note_id(uint32_t id)
{
    for (int i = 0; i < seen_count; i++) {
        if (seen_ids[i] == id) {
            return false;
        }
    }
    if (seen_count < MAX_IDS) {
        seen_ids[seen_count++] = id;
    }
    return true;
}

static void clients_add(int fd)
{
    xSemaphoreTake(clients_lock, portMAX_DELAY);
    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
        if (ws_clients[i] < 0) {
            ws_clients[i] = fd;
            break;
        }
    }
    xSemaphoreGive(clients_lock);
}

static void clients_remove(int fd)
{
    xSemaphoreTake(clients_lock, portMAX_DELAY);
    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
        if (ws_clients[i] == fd) {
            ws_clients[i] = -1;
        }
    }
    xSemaphoreGive(clients_lock);
}

// ---------------- web page ----------------
static const char index_html[] =
"<!DOCTYPE html><html><head><meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>FusionCAN bridge</title>"
"<style>"
"body{background:#111;color:#0f0;font-family:monospace;margin:0;padding:12px}"
"h1{font-size:18px}#status{color:#ff0}"
"table{border-collapse:collapse;width:100%;font-size:12px}"
"td,th{border:1px solid #333;padding:3px 6px;text-align:left}"
"tr.new td{color:#ff0}"
"input,button{background:#222;color:#0f0;border:1px solid #0f0;padding:8px;font-family:monospace;font-size:14px}"
"button{margin-left:6px}"
"#send{margin-top:14px;line-height:2.2}"
".note{color:#888;font-size:12px}"
"</style></head><body>"
"<h1>FusionCAN bridge <span id='status'>connecting...</span></h1>"
"<div id='stats'></div>"
"<h3>Live frames</h3>"
"<table><thead><tr><th>time</th><th>ID</th><th>DLC</th><th>data</th></tr></thead>"
"<tbody id='frames'></tbody></table>"
"<h3>Send frame</h3>"
"<div id='send'>"
"ID (hex): <input id='fid' size='8' placeholder='2F0'><br>"
"Data (hex bytes): <input id='fdata' size='28' placeholder='01 02 A3'><br>"
"<button onclick='sendFrame()'>Send on CAN bus</button> <span id='sendstat'></span>"
"<div class='note'>Transmits one CAN frame with the ID and data above. "
"Only works when the bridge is not in listen-only mode.</div>"
"</div>"
"<script>"
"var ws=new WebSocket('ws://'+location.host+'/ws');"
"var tbody=document.getElementById('frames');"
"ws.onopen=function(){document.getElementById('status').textContent='connected';};"
"ws.onclose=function(){document.getElementById('status').textContent='DISCONNECTED';};"
"ws.onmessage=function(e){"
"  var m=JSON.parse(e.data);"
"  if(m.type==='stats'){document.getElementById('stats').textContent=m.text;return;}"
"  var tr=document.createElement('tr');"
"  if(m.is_new)tr.className='new';"
"  tr.innerHTML='<td>'+m.t+'</td><td>0x'+m.id+'</td><td>'+m.dlc+'</td><td>'+m.data+'</td>';"
"  tbody.prepend(tr);"
"  while(tbody.rows.length>60)tbody.deleteRow(-1);"
"};"
"function sendFrame(){"
"  var id=document.getElementById('fid').value;"
"  var data=document.getElementById('fdata').value;"
"  ws.send(JSON.stringify({cmd:'send',id:id,data:data}));"
"  document.getElementById('sendstat').textContent='sent';"
"  setTimeout(function(){document.getElementById('sendstat').textContent='';},1500);"
"}"
"</script></body></html>";

static esp_err_t root_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, index_html, HTTPD_RESP_USE_STRLEN);
}

// ---------------- websocket ----------------

// Parse {"cmd":"send","id":"2F0","data":"01 02 A3"} and transmit it.
static void handle_ws_command(const char *json)
{
    if (strstr(json, "\"send\"") == NULL) {
        return;
    }
#if CAN_LISTEN_ONLY
    ESP_LOGW(TAG, "transmit refused: bridge is in listen-only mode");
    return;
#endif
    const char *p = strstr(json, "\"id\"");
    if (!p) {
        return;
    }
    p = strchr(p, ':');
    if (!p) {
        return;
    }
    uint32_t id = strtoul(p + 1, NULL, 16);

    uint8_t data[8];
    int dlc = 0;
    p = strstr(json, "\"data\"");
    if (p) {
        p = strchr(p, ':');
        if (p) {
            p++; // skip ':'
            while (*p && dlc < 8) {
                while (*p == ' ' || *p == '"' || *p == ',') {
                    p++;
                }
                if (*p == '"' || *p == '\0' || *p == '}') {
                    break;
                }
                unsigned int b;
                if (sscanf(p, "%2x", &b) == 1) {
                    data[dlc++] = (uint8_t)b;
                    p += 2;
                } else {
                    break;
                }
            }
        }
    }

    twai_message_t tx;
    memset(&tx, 0, sizeof(tx));
    tx.identifier = id;
    tx.data_length_code = dlc;
    memcpy(tx.data, data, dlc);
    if (twai_transmit(&tx, pdMS_TO_TICKS(100)) == ESP_OK) {
        ESP_LOGI(TAG, "TX ID=0x%03" PRIx32 " DLC=%d", id, dlc);
    } else {
        ESP_LOGW(TAG, "TX failed ID=0x%03" PRIx32, id);
    }
}

static esp_err_t ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        // handshake
        int fd = httpd_req_to_sockfd(req);
        ESP_LOGI(TAG, "ws client connected, fd=%d", fd);
        clients_add(fd);
        return ESP_OK;
    }

    httpd_ws_frame_t ws_pkt;
    memset(&ws_pkt, 0, sizeof(ws_pkt));
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;
    esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
    if (ret != ESP_OK) {
        clients_remove(httpd_req_to_sockfd(req));
        return ret;
    }
    if (ws_pkt.len > 0 && ws_pkt.len < 512) {
        uint8_t *buf = malloc(ws_pkt.len + 1);
        if (buf) {
            ws_pkt.payload = buf;
            ret = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
            if (ret == ESP_OK && ws_pkt.type == HTTPD_WS_TYPE_TEXT) {
                buf[ws_pkt.len] = '\0';
                handle_ws_command((const char *)buf);
            }
            free(buf);
        }
    }
    if (ws_pkt.type == HTTPD_WS_TYPE_CLOSE) {
        clients_remove(httpd_req_to_sockfd(req));
    }
    return ESP_OK;
}

// ---------------- CAN task ----------------

static void ws_send_all(const char *json)
{
    httpd_ws_frame_t frame = {
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)json,
        .len = strlen(json),
    };
    int fds[MAX_WS_CLIENTS];
    xSemaphoreTake(clients_lock, portMAX_DELAY);
    memcpy(fds, ws_clients, sizeof(fds));
    xSemaphoreGive(clients_lock);
    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
        if (fds[i] >= 0 && server) {
            httpd_ws_send_frame_async(server, fds[i], &frame);
        }
    }
}

static void can_task(void *arg)
{
    (void)arg;
    twai_message_t rx;
    char json[220];
    char datastr[32];
    uint32_t interval_frames = 0;
    int64_t last_stats = esp_timer_get_time();

    while (1) {
        if (twai_receive(&rx, pdMS_TO_TICKS(200)) == ESP_OK) {
            total_frames++;
            interval_frames++;
            int64_t ms = esp_timer_get_time() / 1000;
            bool is_new = note_id(rx.identifier);
            bool extd = (rx.flags & TWAI_MSG_FLAG_EXTD) != 0;

            int o = 0;
            for (int i = 0; i < rx.data_length_code && o < (int)sizeof(datastr) - 4; i++) {
                o += snprintf(datastr + o, sizeof(datastr) - o, "%02X ", rx.data[i]);
            }
            if (o > 0) {
                datastr[o - 1] = '\0';
            } else {
                datastr[0] = '\0';
            }

            if (extd) {
                snprintf(json, sizeof(json),
                         "{\"type\":\"frame\",\"t\":%" PRId64 ",\"id\":\"%08" PRIx32 "\","
                         "\"dlc\":%d,\"data\":\"%s\",\"is_new\":%d}",
                         ms, rx.identifier, rx.data_length_code, datastr, is_new);
            } else {
                snprintf(json, sizeof(json),
                         "{\"type\":\"frame\",\"t\":%" PRId64 ",\"id\":\"%03" PRIx32 "\","
                         "\"dlc\":%d,\"data\":\"%s\",\"is_new\":%d}",
                         ms, rx.identifier, rx.data_length_code, datastr, is_new);
            }
            ws_send_all(json);

            // serial mirror (handy when USB is plugged in)
            printf("[%" PRId64 "ms]%s ID:0x%03" PRIx32 " DLC:%d %s\n",
                   ms, is_new ? " NEW" : "", rx.identifier, rx.data_length_code, datastr);
        }

        int64_t now = esp_timer_get_time();
        if (now - last_stats > 5000000LL) {
            uint32_t fps = interval_frames / 5;
            snprintf(json, sizeof(json),
                     "{\"type\":\"stats\",\"text\":\"%" PRIu32 " frames | %d unique IDs | ~%"
                     PRIu32 " fps | %s\"}",
                     total_frames, seen_count, fps,
                     CAN_LISTEN_ONLY ? "LISTEN ONLY" : "TX ENABLED");
            ws_send_all(json);
            printf("--- stats: %" PRIu32 " frames, %d unique IDs, ~%" PRIu32 " fps ---\n",
                   total_frames, seen_count, fps);
            interval_frames = 0;
            last_stats = now;
        }
    }
}

// ---------------- wifi ----------------

static void wifi_init_ap(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t ap_cfg = { 0 };
    strcpy((char *)ap_cfg.ap.ssid, WIFI_SSID);
    strcpy((char *)ap_cfg.ap.password, WIFI_PASS);
    ap_cfg.ap.max_connection = MAX_WS_CLIENTS;
    ap_cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ap_cfg.ap.channel = 1;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "WiFi AP '%s' started, open http://192.168.4.1", WIFI_SSID);
}

// ---------------- http server ----------------

static void http_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 8;

    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed");
        return;
    }

    httpd_uri_t root = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = root_handler,
    };
    httpd_register_uri_handler(server, &root);

    httpd_uri_t ws = {
        .uri = "/ws",
        .method = HTTP_GET,
        .handler = ws_handler,
        .is_websocket = true,
    };
    httpd_register_uri_handler(server, &ws);
    ESP_LOGI(TAG, "HTTP server started");
}

// ---------------- main ----------------

void app_main(void)
{
    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
        ws_clients[i] = -1;
    }
    clients_lock = xSemaphoreCreateMutex();

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    printf("\n=== ESP32 WiFi <-> CAN bridge ===\n");
    printf("bus: %d kbps | TWAI TX=%d RX=%d | %s\n", CAN_BITRATE_KBPS,
           TWAI_TX_GPIO, TWAI_RX_GPIO,
           CAN_LISTEN_ONLY ? "LISTEN ONLY (no transmit)" : "TX ENABLED");

    twai_general_config_t g_config = TWAI_GENERAL_CONFIG_DEFAULT(
        TWAI_TX_GPIO, TWAI_RX_GPIO,
        CAN_LISTEN_ONLY ? TWAI_MODE_LISTEN_ONLY : TWAI_MODE_NORMAL);

#if CAN_BITRATE_KBPS == 125
    twai_timing_config_t t_config = TWAI_TIMING_CONFIG_125KBITS();
#elif CAN_BITRATE_KBPS == 500
    twai_timing_config_t t_config = TWAI_TIMING_CONFIG_500KBITS();
#else
#error "Unsupported CAN_BITRATE_KBPS (use 125 or 500)"
#endif

    twai_filter_config_t f_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();
    ESP_ERROR_CHECK(twai_driver_install(&g_config, &t_config, &f_config));
    ESP_ERROR_CHECK(twai_start());
    ESP_LOGI(TAG, "TWAI started");

    wifi_init_ap();
    http_start();

    xTaskCreate(can_task, "can_task", 6144, NULL, 5, NULL);
    ESP_LOGI(TAG, "bridge running - connect to '%s' and open http://192.168.4.1", WIFI_SSID);
}
