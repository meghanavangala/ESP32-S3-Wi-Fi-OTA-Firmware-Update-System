#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "nvs_flash.h"
#include "nvs.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"

#include "esp_http_client.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_netif.h"

#include "driver/gpio.h"

/* ---------- USER CONFIG ---------- */


#define WIFI_SSID       "YOUR_WIFI_SSID"
#define WIFI_PASS       "YOUR_WIFI_PASSWORD"

#define VERSION_URL     "http://YOUR_SERVER/ota/version.txt"
#define FIRMWARE_URL    "http://YOUR_SERVER/ota/firmware.bin"

#define LED_GPIO        GPIO_NUM_2
#define OTA_RETRY_COUNT 3
#define OTA_RETRY_DELAY_MS 5000

/* --------------------------------- */

static const char *TAG = "MAIN_APP";

char server_version_global[32] = {0};

/* ---------- SINGLE LED BLINK TASK ---------- */
/* GPIO2 LED blinks continuously while Wi-Fi/OTA testing runs. */
static void led_task(void *arg)
{
    ESP_LOGI(TAG, "LED test started on GPIO %d", (int)LED_GPIO);

    while (1)
    {
        gpio_set_level(LED_GPIO, 1);  // LED ON (for active-low LEDs, reverse these levels)
        ESP_LOGD(TAG, "LED ON");
        vTaskDelay(pdMS_TO_TICKS(5000));

        gpio_set_level(LED_GPIO, 0);  // LED OFF
        ESP_LOGD(TAG, "LED OFF");
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

/* ---------- NVS READ VERSION ---------- */
static void read_version_nvs(char *version)
{
    nvs_handle_t handle;

    if (nvs_open("storage", NVS_READONLY, &handle) == ESP_OK)
    {
        size_t size = 32;
        if (nvs_get_str(handle, "fw_ver", version, &size) != ESP_OK)
        {
            strcpy(version, "0.0.0");
        }
        nvs_close(handle);
    }
    else
    {
        strcpy(version, "0.0.0");
    }
}

/* ---------- NVS WRITE VERSION ---------- */
static void write_version_nvs(const char *version)
{
    nvs_handle_t handle;

    if (nvs_open("storage", NVS_READWRITE, &handle) == ESP_OK)
    {
        nvs_set_str(handle, "fw_ver", version);
        nvs_commit(handle);
        nvs_close(handle);
    }
}

/* ---------- VERSION CHECK WITH RETRY ---------- */

/* ---------- VERSION CHECK WITH RETRY ---------- */
static int check_server_version(char *version_buffer)
{
    ESP_LOGI(TAG, "Checking version from: %s", VERSION_URL);

    for (int retry = 0; retry < OTA_RETRY_COUNT; retry++)
    {
        esp_http_client_config_t config = {
            .url = VERSION_URL,
            .timeout_ms = 10000,
            .buffer_size = 1024,
        };

        esp_http_client_handle_t client =
            esp_http_client_init(&config);

        if (client == NULL)
        {
            ESP_LOGE(TAG, "HTTP client initialization failed");
            return -1;
        }

        esp_err_t err = esp_http_client_open(client, 0);

        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "Version connection failed: %s",
                     esp_err_to_name(err));

            esp_http_client_cleanup(client);

            if (retry < OTA_RETRY_COUNT - 1)
                vTaskDelay(pdMS_TO_TICKS(OTA_RETRY_DELAY_MS));

            continue;
        }

        int content_length =
            esp_http_client_fetch_headers(client);

        int status = esp_http_client_get_status_code(client);

        ESP_LOGI(TAG, "Version HTTP status: %d", status);

        if (status != 200)
        {
            ESP_LOGE(TAG, "Version server returned HTTP %d", status);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);

            if (retry < OTA_RETRY_COUNT - 1)
                vTaskDelay(pdMS_TO_TICKS(OTA_RETRY_DELAY_MS));

            continue;
        }

        int total = 0;
        char buffer[32];

        while (total < (int)sizeof(buffer) - 1)
        {
            int n = esp_http_client_read(
                client, buffer + total,
                sizeof(buffer) - 1 - total);

            if (n < 0)
            {
                total = -1;
                break;
            }

            if (n == 0)
                break;

            total += n;
        }

        esp_http_client_close(client);
        esp_http_client_cleanup(client);

        if (total <= 0)
        {
            ESP_LOGE(TAG, "Empty or failed version response");

            if (retry < OTA_RETRY_COUNT - 1)
                vTaskDelay(pdMS_TO_TICKS(OTA_RETRY_DELAY_MS));

            continue;
        }

        buffer[total] = '\0';
        buffer[strcspn(buffer, "\r\n")] = '\0';

        if (buffer[0] == '\0' ||
            strspn(buffer, "0123456789.") != strlen(buffer))
        {
            ESP_LOGE(TAG, "Invalid version text: '%s'", buffer);
            return -1;
        }

        strcpy(version_buffer, buffer);
        strcpy(server_version_global, buffer);

        ESP_LOGI(TAG, "Server version: %s", version_buffer);
        return 0;
    }

    ESP_LOGE(TAG, "Could not read server version after retries");
    return -1;
}

/* ---------- OTA DOWNLOAD ---------- */
static void perform_ota_update()
{
    ESP_LOGI(TAG, "=== STARTING OTA DOWNLOAD ===");
    ESP_LOGI(TAG, "Downloading from: %s", FIRMWARE_URL);

    esp_http_client_config_t config = {
        .url = FIRMWARE_URL,
        .timeout_ms = 30000,
        .buffer_size = 4096,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    
    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to initialize HTTP client");
        return;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Firmware connect failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return;
    }

    esp_http_client_fetch_headers(client);

    int status = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "Firmware HTTP Status = %d", status);

    if (status != 200)
    {
        ESP_LOGE(TAG, "Firmware HTTP error: %d", status);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return;
    }

    int content_length = esp_http_client_get_content_length(client);
    ESP_LOGI(TAG, "Firmware Size = %d bytes", content_length);

    if (content_length <= 0) {
        ESP_LOGE(TAG, "Invalid content length");
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return;
    }

    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
    if (update_partition == NULL) {
        ESP_LOGE(TAG, "Failed to get OTA partition");
        esp_http_client_cleanup(client);
        return;
    }
    
    ESP_LOGI(TAG, "Writing to partition: %s at offset 0x%" PRIx32, 
             update_partition->label, update_partition->address);

    esp_ota_handle_t ota_handle;
    err = esp_ota_begin(update_partition, content_length, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA begin failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return;
    }

    char buffer[4096];
    int read_len;
    int total_written = 0;
    int last_percent = -1;

    while ((read_len = esp_http_client_read(client, buffer, sizeof(buffer))) > 0)
    {
        err = esp_ota_write(ota_handle, buffer, read_len);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "OTA write failed: %s", esp_err_to_name(err));
            esp_ota_abort(ota_handle);
            esp_http_client_cleanup(client);
            return;
        }
        total_written += read_len;
        
        int percent = (total_written * 100) / content_length;
        if (percent != last_percent && (percent % 10 == 0 || percent == 100)) {
            ESP_LOGI(TAG, "OTA Progress: %d%% (%d/%d bytes)", 
                     percent, total_written, content_length);
            last_percent = percent;
            
            // Allow other tasks to run during long OTA download
            taskYIELD();
        }
    }

    if (read_len < 0) {
        ESP_LOGE(TAG, "HTTP read error: %d", read_len);
        esp_ota_abort(ota_handle);
        esp_http_client_cleanup(client);
        return;
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA end failed: %s", esp_err_to_name(err));
        return;
    }

    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA set boot partition failed: %s", esp_err_to_name(err));
        return;
    }

    write_version_nvs(server_version_global);

    ESP_LOGI(TAG, "=========================================");
    ESP_LOGI(TAG, "=== OTA SUCCESS! Version %s installed ===", server_version_global);
    ESP_LOGI(TAG, "=== Rebooting in 5 seconds ===");
    ESP_LOGI(TAG, "=========================================");
    
    vTaskDelay(pdMS_TO_TICKS(5000));
    esp_restart();
}

/* ---------- OTA CONTROL WITH RETRY ---------- */
static void start_ota()
{
    ESP_LOGI(TAG, "Starting OTA check...");
    char device_version[32];
    char server_version[32];

    read_version_nvs(device_version);
    ESP_LOGI(TAG, "Current Device Version: %s", device_version);

    if (check_server_version(server_version) != 0)
    {
        ESP_LOGE(TAG, "Version check failed after %d attempts - continuing without update", 
                 OTA_RETRY_COUNT);
        return;
    }

    ESP_LOGI(TAG, "Server Version: %s", server_version);

    if (strcmp(device_version, server_version) == 0)
    {
        ESP_LOGI(TAG, "Firmware is already up to date");
        return;
    }

    ESP_LOGI(TAG, "New firmware found! Starting OTA update...");
    perform_ota_update();
}

/* ---------- OTA TASK ---------- */
static void ota_task(void *pv)
{
    ESP_LOGI(TAG, "OTA Task Started - Waiting for network stabilization...");
    vTaskDelay(pdMS_TO_TICKS(8000));
    
    ESP_LOGI(TAG, "Testing network connectivity...");
    esp_http_client_config_t test_config = {
        .url = VERSION_URL,
        .timeout_ms = 5000,
    };
    
    esp_http_client_handle_t test_client = esp_http_client_init(&test_config);
    if (test_client != NULL) {
        esp_err_t test_err = esp_http_client_open(test_client, 0);
        if (test_err == ESP_OK) {
            ESP_LOGI(TAG, "Network connectivity verified");
            esp_http_client_close(test_client);
        } else {
            ESP_LOGW(TAG, "Network not ready yet, waiting additional 5 seconds...");
            vTaskDelay(pdMS_TO_TICKS(5000));
        }
        esp_http_client_cleanup(test_client);
    }
    
    start_ota();
    
    ESP_LOGI(TAG, "OTA Task completed");
    vTaskDelete(NULL);
}

/* ---------- WIFI EVENT HANDLER ---------- */
static void wifi_event_handler(void *arg,
                               esp_event_base_t base,
                               int32_t id,
                               void *data)
{
    static bool ota_started = false;
    
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "WiFi STA started, connecting...");
        esp_wifi_connect();
    }
    else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "WiFi disconnected, retrying...");
        ota_started = false;
        esp_wifi_connect();
    }
    else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) data;
        ESP_LOGI(TAG, "=== WiFi Connected! IP: " IPSTR " ===", IP2STR(&event->ip_info.ip));
        
        if (!ota_started) {
            ota_started = true;
            ESP_LOGI(TAG, "Creating OTA task...");
            xTaskCreate(ota_task, "ota_task", 8192, NULL, 5, NULL);
        }
    }
}

/* ---------- WIFI INIT ---------- */
static void wifi_init()
{
    ESP_LOGI(TAG, "Initializing WiFi...");
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t ret = esp_wifi_init(&cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WiFi init failed: %s", esp_err_to_name(ret));
        return;
    }

    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL);

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };

    ret = esp_wifi_set_mode(WIFI_MODE_STA);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WiFi set mode failed: %s", esp_err_to_name(ret));
        return;
    }
    
    ret = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WiFi set config failed: %s", esp_err_to_name(ret));
        return;
    }
    
    ret = esp_wifi_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WiFi start failed: %s", esp_err_to_name(ret));
        return;
    }
    
    ESP_LOGI(TAG, "WiFi initialized, connecting to %s...", WIFI_SSID);
}

/* ---------- MAIN APPLICATION ---------- */
void app_main()
{
    // Configure LED GPIO
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << LED_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t gpio_ret = gpio_config(&io_conf);
    ESP_ERROR_CHECK(gpio_ret);

    // Start with LED OFF before the blink task runs.
    ESP_ERROR_CHECK(gpio_set_level(LED_GPIO, 0));

    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS error, erasing and re-initializing...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    
    // Mark current app as valid
    esp_ota_mark_app_valid_cancel_rollback();
    
    // Create tasks
    xTaskCreate(led_task, "led_task", 2048, NULL, 5, NULL);
    
    // Initialize WiFi
    wifi_init();
    
    ESP_LOGI(TAG, "==========================================");
    ESP_LOGI(TAG, "System Initialized:");
    ESP_LOGI(TAG, "- LED blinking on GPIO %d", LED_GPIO);
    ESP_LOGI(TAG, "- WiFi connecting to %s", WIFI_SSID);
    ESP_LOGI(TAG, "- OTA check will run after WiFi connects");
    ESP_LOGI(TAG, "==========================================");
}