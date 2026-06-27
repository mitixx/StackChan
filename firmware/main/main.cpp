/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include <smooth_ui_toolkit.hpp>
#include <uitk/short_namespace.hpp>
#include <mooncake_log.h>
#include <mooncake.h>
#include <apps/apps.h>
#include <hal/hal.h>
#include "esp_http_server.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "lwip/ip_addr.h"
#include "assets/assets.h"
#include <hal/board/hal_bridge.h>
#include "cJSON.h"

using namespace mooncake;
using namespace smooth_ui_toolkit;

static httpd_handle_t _server = NULL;

// HTTP GET /hello のリクエストを処理するハンドラ
static esp_err_t hello_get_handler(httpd_req_t *req)
{
    mclog::info("Received /hello request!");
    hal_bridge::app_play_sound(OGG_HELLO);
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// HTTP POST /github — GitHub webhook ハンドラ
static esp_err_t github_webhook_handler(httpd_req_t *req)
{
    // X-GitHub-Event ヘッダーを取得
    char event[64] = "unknown";
    httpd_req_get_hdr_value_str(req, "X-GitHub-Event", event, sizeof(event));

    // ボディを読み込む（最大 4KB）
    const size_t BUF_SIZE = 4096;
    char *buf = (char *)malloc(BUF_SIZE);
    if (!buf) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    int received = 0;
    int remaining = req->content_len < (int)BUF_SIZE - 1 ? req->content_len : (int)BUF_SIZE - 1;
    while (remaining > 0) {
        int ret = httpd_req_recv(req, buf + received, remaining);
        if (ret <= 0) break;
        received += ret;
        remaining -= ret;
    }
    buf[received] = '\0';

    // JSON からリポジトリ名・送信者を抽出
    const char *repo   = "unknown";
    const char *sender = "unknown";
    cJSON *root = cJSON_Parse(buf);
    if (root) {
        cJSON *repo_obj   = cJSON_GetObjectItem(root, "repository");
        cJSON *sender_obj = cJSON_GetObjectItem(root, "sender");
        if (repo_obj)   repo   = cJSON_GetStringValue(cJSON_GetObjectItem(repo_obj,   "full_name")) ?: repo;
        if (sender_obj) sender = cJSON_GetStringValue(cJSON_GetObjectItem(sender_obj, "login"))     ?: sender;
    }

    mclog::info("GitHub event: {} | repo: {} | sender: {}", event, repo, sender);

    // GitHub 通知音声を再生
    hal_bridge::app_play_sound(OGG_GITHUB);

    if (root) cJSON_Delete(root);
    free(buf);

    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static const httpd_uri_t hello_uri = {
    .uri      = "/hello",
    .method   = HTTP_GET,
    .handler  = hello_get_handler,
    .user_ctx = NULL
};

static const httpd_uri_t github_uri = {
    .uri      = "/github",
    .method   = HTTP_POST,
    .handler  = github_webhook_handler,
    .user_ctx = NULL
};

// HTTPサーバーを起動する関数
static void start_webserver(void)
{
    if (_server != NULL) return;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;

    mclog::info("Starting httpd server on port: '{}'", config.server_port);
    if (httpd_start(&_server, &config) == ESP_OK) {
        mclog::info("Registering URI handlers");
        httpd_register_uri_handler(_server, &hello_uri);
        httpd_register_uri_handler(_server, &github_uri);
    } else {
        mclog::error("Error starting server!");
    }
}

// WiFi IP取得イベントでHTTPサーバーを起動
static void on_got_ip(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    start_webserver();
}

extern "C" void app_main(void)
{
    // Setup logger
    mclog::set_level(mclog::level_info);
    mclog::set_time_format(mclog::time_format_unix_milliseconds);

    // デフォルトイベントループを作成してハンドラを登録（HAL init前に行う）
    esp_event_loop_create_default();
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &on_got_ip, NULL);

    // HAL init
    GetHAL().init();

    // Setup ui hal
    ui_hal::on_delay([](uint32_t ms) { GetHAL().delay(ms); });
    ui_hal::on_get_tick([]() { return GetHAL().millis(); });

    const bool skip_mooncake =
        GetHAL().getXiaozhiConfig().startAiAgentOnBoot && GetHAL().getWarmRebootTarget() < 0;

    if (!skip_mooncake) {
        // Install apps
        GetMooncake().installApp(std::make_unique<AppLauncher>());
        GetMooncake().installApp(std::make_unique<AppAiAgent>());
        GetMooncake().installApp(std::make_unique<AppAvatar>());
        GetMooncake().installApp(std::make_unique<AppEspnowControl>());
        GetMooncake().installApp(std::make_unique<AppAppCenter>());
        GetMooncake().installApp(std::make_unique<AppEzdata>());
        GetMooncake().installApp(std::make_unique<AppDance>());
        GetMooncake().installApp(std::make_unique<AppSetup>());

        // Main loop
        while (1) {
            GetHAL().feedTheDog();
            GetHAL().updateHeapStatusLog();

            GetMooncake().update();

            if (GetHAL().isXiaozhiStartRequested()) {
                break;
            }
        }

        // Uninstall all apps and destroy mooncake
        GetMooncake().uninstallAllApps();
        DestroyMooncake();
    }

    // Start xiaozhi, never returns
    GetHAL().startXiaozhi();
}
