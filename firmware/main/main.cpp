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

// URIとハンドラを紐付ける設定
static const httpd_uri_t hello_uri = {
    .uri      = "/hello",
    .method   = HTTP_GET,
    .handler  = hello_get_handler,
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
