# スタックチャン ファームウェア 開発ログ

## プロジェクト概要

| 項目 | 内容 |
|------|------|
| プロジェクト | スタックチャン (StackChan) — M5Stack CoreS3 用ファームウェア |
| 作業ディレクトリ | `/Users/tomomi/Desktop/StackChan/firmware` |
| ビルドシステム | ESP-IDF 5.5.4 / CMake / Ninja |
| ターゲットチップ | ESP32-S3 |
| 書き込みポート | `/dev/cu.usbmodem1201` |
| デバイス IP | `192.168.11.16` (Buffalo-2G-4748 接続時) |

---

## ESP-IDF バージョンの選定

IDF 6.0.1 を試したが、マネージドコンポーネントの多数の Breaking API 変更により断念。プロジェクトは **IDF 5.5.4** を対象としている。

- **IDF パス**: `/Users/tomomi/.espressif/v5.5.4/esp-idf/`
- **ビルド手順**:
  ```sh
  source /Users/tomomi/.espressif/v5.5.4/esp-idf/export.sh
  idf.py build
  ```
- **書き込み手順**:
  ```sh
  idf.py flash -p /dev/cu.usbmodem1201
  ```

---

## ビルド時の修正一覧

### 1. `espressif/cjson` が見つからない

- **対象ファイル**: `firmware/main/idf_component.yml`
- **原因**: `components/json/CMakeLists.txt` が `cjson` を必要とするが、IDF 5.5.4 では内蔵されなくなった
- **修正**: `idf_component.yml` に `espressif/cjson: '*'` を追加

### 2. `protocomm_security1` が未定義

- **対象ファイル**: `firmware/sdkconfig.defaults`
- **原因**: 生成された `sdkconfig` で `CONFIG_ESP_PROTOCOMM_SUPPORT_SECURITY_VERSION_1` が無効になっており、`components/esp-now` の `espnow_security_responder.c:244` でコンパイルエラー
- **修正**: `sdkconfig.defaults` に `CONFIG_ESP_PROTOCOMM_SUPPORT_SECURITY_VERSION_1=y` を追加

### 3. `std::cos` / `std::sin` / `std::atan` が `std` 名前空間に存在しない

- **対象ファイル**:
  - `firmware/managed_components/espressif__esp-dsp/modules/kalman/ekf/common/ekf.cpp`
  - `firmware/managed_components/espressif__esp-dsp/modules/kalman/ekf_imu13states/ekf_imu13states.cpp`
- **原因**: `<math.h>` をインクルードしつつ `std::cos` 等を使用。新しい GCC は C ヘッダのシンボルを `std` 名前空間に注入しない
- **修正**: 両ファイルに `#include <cmath>` を追加

### 4. `struct tm` のサイズが不明 (sdcard.c)

- **対象ファイル**: `firmware/components/esp-now/src/debug/src/sdcard/sdcard.c`
- **原因**: `struct tm` を使用しているが `<time.h>` がインクルードされていない
- **修正**: `#include <time.h>` を追加

### 5. `GetNetwork()->IsConnected()` が存在しない

- **対象ファイル**: `firmware/main/main.cpp`
- **原因**: `Hal` クラスに `GetNetwork()` メソッドが存在しない
- **修正**: `GetHAL().getWifiStatus() != WifiStatus::None` に変更

---

## 追加機能 1: HTTP サーバー (`GET /hello`)

`main.cpp` に HTTP サーバーを追加。`GET /hello` リクエストを受信すると `200 OK` を返す。

### 実装コード

```cpp
#include "esp_http_server.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "assets/assets.h"
#include <hal/board/hal_bridge.h>

static httpd_handle_t _server = NULL;

static esp_err_t hello_get_handler(httpd_req_t *req) {
    mclog::info("Received /hello request!");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static const httpd_uri_t hello_uri = {
    .uri = "/hello", .method = HTTP_GET,
    .handler = hello_get_handler, .user_ctx = NULL
};

static void start_webserver(void) {
    if (_server != NULL) return;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;
    if (httpd_start(&_server, &config) == ESP_OK) {
        httpd_register_uri_handler(_server, &hello_uri);
    }
}

// WiFi の IP 取得イベントでサーバーを起動
static void on_got_ip(void *arg, esp_event_base_t event_base,
                      int32_t event_id, void *event_data) {
    start_webserver();
}
```

### `app_main()` 内の起動シーケンス

```cpp
// デフォルトイベントループを作成してハンドラを登録（HAL init より前に行う）
esp_event_loop_create_default();
esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &on_got_ip, NULL);

// HAL 初期化（内部で WiFi も起動）
GetHAL().init();
```

### 設計上の注意点

| 注意点 | 詳細 |
|--------|------|
| TCP/IP スタック未初期化でのパニック | `httpd_start()` を早期に呼ぶと `tcpip_send_msg_wait_sem` でクラッシュする |
| WiFi は非同期 | `init()` 直後に `getWifiStatus()` を確認しても常に `None` が返る |
| イベントループの先行作成が必要 | `esp_event_handler_register()` はイベントループが存在しないと無音で失敗する |
| イベントループの作成タイミング | デフォルトイベントループは `startXiaozhi()` 内部で作成されるため、`init()` 後では遅い |

### 動作確認

```sh
curl http://192.168.11.16/hello
# → HTTP 200 / body: OK
curl -X POST http://192.168.11.16/hello
# → HTTP 405 Method Not Allowed
curl http://192.168.11.16/notfound
# → HTTP 404 Not Found
```

---

## 追加機能 2: `/hello` リクエスト時の音声発話

`GET /hello` を受信したとき、デバイスが "hello" と発話する。

### 音声システムの概要

- OGG/Opus 形式の音声ファイルをファームウェアバイナリに埋め込み
- 再生 API: `hal_bridge::app_play_sound(std::string_view)` → `Application::GetInstance().PlaySound()`
- 既存の効果音は `firmware/main/assets/assets.h` に `OGG_CAMERA_SHUTTER` / `OGG_NEW_NOTIFICATION` として定義済み
- `firmware/main/assets/sfx/` 以下の `*.ogg` は CMakeLists.txt の glob で自動的に埋め込まれる

### 実装手順

**① 音声ファイルの生成**

```sh
# macOS の say コマンドで AIFF を生成し ffmpeg で OGG/Opus に変換
say -o /tmp/hello.aiff "hello"
ffmpeg -y -i /tmp/hello.aiff -c:a libopus -b:a 24k -ar 16000 \
    firmware/main/assets/sfx/hello.ogg
```

**② `assets.h` にシンボルを追加** (`firmware/main/assets/assets.h`)

```cpp
extern const char ogg_hello_start[] asm("_binary_hello_ogg_start");
extern const char ogg_hello_end[]   asm("_binary_hello_ogg_end");
static const std::string_view OGG_HELLO{
    static_cast<const char*>(ogg_hello_start),
    static_cast<size_t>(ogg_hello_end - ogg_hello_start)};
```

**③ ハンドラで再生** (`firmware/main/main.cpp`)

```cpp
static esp_err_t hello_get_handler(httpd_req_t *req) {
    mclog::info("Received /hello request!");
    hal_bridge::app_play_sound(OGG_HELLO);   // ← デバイスが "hello" と発話
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}
```

### CMake `file(GLOB)` の注意点

`main/CMakeLists.txt` は `assets/sfx/*.ogg` を glob で取得しているが、**CMake は既存のビルドツリーに新しいファイルが追加されても自動的に再スキャンしない**。`hello.ogg` を追加した後は以下を実行してから `idf.py build`:

```sh
touch firmware/main/CMakeLists.txt
```

---

## 主要ファイル一覧

| 用途 | パス |
|------|------|
| メインアプリケーション | `firmware/main/main.cpp` |
| HAL インターフェース | `firmware/main/hal/hal.h` |
| HAL ブリッジ API | `firmware/main/hal/board/hal_bridge.h` |
| アセットヘッダ | `firmware/main/assets/assets.h` |
| 効果音ファイル置き場 | `firmware/main/assets/sfx/` |
| コンポーネント依存定義 | `firmware/main/idf_component.yml` |
| ビルド設定デフォルト | `firmware/sdkconfig.defaults` |
| ESP-IDF ルート | `/Users/tomomi/.espressif/v5.5.4/esp-idf/` |
