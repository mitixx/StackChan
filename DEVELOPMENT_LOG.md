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

## 追加機能 3: インターネット経由の GitHub Webhook 受信

GitHub のプッシュや PR などのイベントをスタックちゃんが音声で通知する機能。

### アーキテクチャ

```
GitHub (クラウド)
    │ POST https://xxxx.trycloudflare.com/github
    ▼
Cloudflare Edge
    │ (トンネル経由)
    ▼
Mac (cloudflared プロセス)
    │ HTTP転送 → http://192.168.11.16:80
    ▼
ESP32 スタックちゃん
    │ /github ハンドラ
    ▼
「GitHubから通知です」と音声再生
```

### 使用ツール

| ツール | 用途 | 備考 |
|--------|------|------|
| `cloudflared` | Mac → インターネット間のトンネル | 認証不要・無料（Cloudflare公式） |
| `edge-tts` | 音声ファイル生成 | Microsoft Edge TTS、`ja-JP-NanamiNeural` ボイス |
| `ffmpeg` | MP3 → OGG/Opus 変換 | 16kHz モノラル、24kbps |

### 音声ファイル生成

```sh
# Microsoft Edge TTS で「GitHubから通知です」を生成
python3 -m edge_tts --voice ja-JP-NanamiNeural \
    --text "GitHubから通知です" --write-media /tmp/github_notify.mp3

# ESP32 用 OGG/Opus 形式に変換
ffmpeg -y -i /tmp/github_notify.mp3 -c:a libopus -b:a 24k -ar 16000 \
    firmware/main/assets/sfx/github.ogg
```

### 実装コード (`firmware/main/main.cpp`)

```cpp
#include "cJSON.h"

// POST /github — GitHub webhook ハンドラ
static esp_err_t github_webhook_handler(httpd_req_t *req) {
    char event[64] = "unknown";
    httpd_req_get_hdr_value_str(req, "X-GitHub-Event", event, sizeof(event));

    // ボディ読み込み（最大 4KB）
    const size_t BUF_SIZE = 4096;
    char *buf = (char *)malloc(BUF_SIZE);
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
    const char *repo = "unknown", *sender = "unknown";
    cJSON *root = cJSON_Parse(buf);
    if (root) {
        cJSON *r = cJSON_GetObjectItem(root, "repository");
        cJSON *s = cJSON_GetObjectItem(root, "sender");
        if (r) repo   = cJSON_GetStringValue(cJSON_GetObjectItem(r, "full_name")) ?: repo;
        if (s) sender = cJSON_GetStringValue(cJSON_GetObjectItem(s, "login"))     ?: sender;
    }
    mclog::info("GitHub event: {} | repo: {} | sender: {}", event, repo, sender);

    hal_bridge::app_play_sound(OGG_GITHUB);  // 「GitHubから通知です」

    if (root) cJSON_Delete(root);
    free(buf);
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static const httpd_uri_t github_uri = {
    .uri = "/github", .method = HTTP_POST,
    .handler = github_webhook_handler, .user_ctx = NULL
};
```

### `assets.h` への追加

```cpp
extern const char ogg_github_start[] asm("_binary_github_ogg_start");
extern const char ogg_github_end[]   asm("_binary_github_ogg_end");
static const std::string_view OGG_GITHUB{
    static_cast<const char*>(ogg_github_start),
    static_cast<size_t>(ogg_github_end - ogg_github_start)};
```

### トンネルの起動方法

```sh
# Cloudflare Tunnel でスタックちゃんをインターネットに公開
cloudflared tunnel --url http://192.168.11.16:80 --no-autoupdate &

# 発行された URL を確認（起動後 5〜10 秒待つ）
# 例: https://infants-commission-clause-laid.trycloudflare.com
```

### GitHub Webhook 設定手順

1. 通知したいリポジトリ → **Settings → Webhooks → Add webhook**
2. **Payload URL**: `https://<自動発行URL>.trycloudflare.com/github`
3. **Content type**: `application/json`
4. **Which events**: 受信したいイベント（push / pull_request / issues など）
5. **Active**: チェックを入れて **Add webhook** をクリック

### 動作確認コマンド

```sh
# ローカル確認
curl -X POST http://192.168.11.16/github \
  -H "X-GitHub-Event: push" \
  -H "Content-Type: application/json" \
  -d '{"repository":{"full_name":"mitixx/stack-chan"},"sender":{"login":"mitixx"}}'

# インターネット経由
curl -X POST https://<URL>/github \
  -H "X-GitHub-Event: push" \
  -H "Content-Type: application/json" \
  -d '{"repository":{"full_name":"mitixx/stack-chan"},"sender":{"login":"mitixx"}}'
# → HTTP 200 / スタックちゃんが「GitHubから通知です」と発話
```

### 注意事項

| 項目 | 内容 |
|------|------|
| URL の固定 | `trycloudflare.com` の URL は起動ごとに変わる。ngrok 有料プランか自前 Caddy リバースプロキシで固定化可能 |
| 認証 | 現在は無認証。本番運用では `X-Hub-Signature-256` ヘッダの HMAC 検証を追加推奨 |
| ペイロードサイズ | 現在 4KB 上限。大きなコミットセットは途中で切り捨てられる（JSON 抽出は成功する） |
| 音声ファイル追加後 | `touch firmware/main/CMakeLists.txt` で CMake に再スキャンを強制してからビルド |

---

## 主要ファイル一覧

| 用途 | パス |
|------|------|
| メインアプリケーション | `firmware/main/main.cpp` |
| HAL インターフェース | `firmware/main/hal/hal.h` |
| HAL ブリッジ API | `firmware/main/hal/board/hal_bridge.h` |
| アセットヘッダ | `firmware/main/assets/assets.h` |
| 効果音ファイル置き場 | `firmware/main/assets/sfx/` |
| GitHub 通知音声 | `firmware/main/assets/sfx/github.ogg` |
| コンポーネント依存定義 | `firmware/main/idf_component.yml` |
| ビルド設定デフォルト | `firmware/sdkconfig.defaults` |
| ESP-IDF ルート | `/Users/tomomi/.espressif/v5.5.4/esp-idf/` |
