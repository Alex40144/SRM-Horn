#include <stdio.h>
#include <inttypes.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_mac.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"

#include "config.h"

#include "esp_wifi.h"
#include <esp_http_server.h>
#include "nvs_flash.h"

const char *ssid = "espAP";

#define CONNECT_WIFI_SSID "WSC-Mgmt"
#define CONNECT_WIFI_PASS "R6CYhWr9&v6B!fR$"
#define CONNECT_WIFI_MAXIMUM_RETRY 4
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA2_PSK
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1

/* FreeRTOS event group to signal when we are connected*/
static EventGroupHandle_t s_wifi_event_group;

static const char *TAG = "Main";

int counting = 0;
int up = 0;
int timer = 300;

extern const uint8_t short_html_start[] asm("_binary_short_html_start");
extern const uint8_t short_html_end[] asm("_binary_short_html_end");
extern const uint8_t medium_html_start[] asm("_binary_medium_html_start");
extern const uint8_t medium_html_end[] asm("_binary_medium_html_end");
extern const uint8_t long_html_start[] asm("_binary_long_html_start");
extern const uint8_t long_html_end[] asm("_binary_long_html_end");

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_id == WIFI_EVENT_AP_STACONNECTED)
    {
        wifi_event_ap_staconnected_t *event = (wifi_event_ap_staconnected_t *)event_data;
        ESP_LOGI(TAG, "station " MACSTR " join, AID=%d",
                 MAC2STR(event->mac), event->aid);
    }
    else if (event_id == WIFI_EVENT_AP_STADISCONNECTED)
    {
        wifi_event_ap_stadisconnected_t *event = (wifi_event_ap_stadisconnected_t *)event_data;
        ESP_LOGI(TAG, "station " MACSTR " leave, AID=%d",
                 MAC2STR(event->mac), event->aid);
    }
}

static void init_nvs()
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
}

static void init_AP()
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *wifiAP = esp_netif_create_default_wifi_ap();

    esp_netif_ip_info_t ipInfo;
    esp_netif_set_ip4_addr(&ipInfo.ip, 192, 168, 1, 1);
    esp_netif_set_ip4_addr(&ipInfo.gw, 192, 168, 1, 1);
    esp_netif_set_ip4_addr(&ipInfo.netmask, 255, 255, 255, 0);
    esp_netif_dhcps_stop(wifiAP);
    esp_netif_set_ip_info(wifiAP, &ipInfo);
    esp_netif_dhcps_start(wifiAP);

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        NULL));
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t wifi_config = {
        .ap = {
            .ssid = CONFIG_SSID,
            .password = "",
            .max_connection = 3},
    };

    wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));

    ESP_ERROR_CHECK(esp_wifi_start());
}

static int s_retry_num = 0;
static void event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        esp_wifi_connect();
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        if (s_retry_num < CONNECT_WIFI_MAXIMUM_RETRY)
        {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "retry to connect to the AP");
        }
        else
        {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        ESP_LOGI(TAG, "connect to the AP fail");
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static void connect_AP()
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());

    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = CONNECT_WIFI_SSID,
            .password = CONNECT_WIFI_PASS,
            /* Authmode threshold resets to WPA2 as default if password matches WPA2 standards (pasword len => 8).
             * If you want to connect the device to deprecated WEP/WPA networks, Please set the threshold value
             * to WIFI_AUTH_WEP/WIFI_AUTH_WPA_PSK and set the password with length and format matching to
             * WIFI_AUTH_WEP/WIFI_AUTH_WPA_PSK standards.
             */
            .threshold.authmode = ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD,
            .sae_pwe_h2e = WPA3_SAE_PWE_BOTH,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "wifi_init_sta finished.");

    /* Waiting until either the connection is established (WIFI_CONNECTED_BIT) or connection failed for the maximum
     * number of re-tries (WIFI_FAIL_BIT). The bits are set by event_handler() (see above) */
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE,
                                           pdFALSE,
                                           portMAX_DELAY);

    /* xEventGroupWaitBits() returns the bits before the call returned, hence we can test which event actually
     * happened. */
    if (bits & WIFI_CONNECTED_BIT)
    {
        ESP_LOGI(TAG, "connected to ap SSID:%s password:%s",
                 CONNECT_WIFI_SSID, CONNECT_WIFI_PASS);
    }
    else if (bits & WIFI_FAIL_BIT)
    {
        ESP_LOGI(TAG, "Failed to connect to SSID:%s, password:%s",
                 CONNECT_WIFI_SSID, CONNECT_WIFI_PASS);
    }
    else
    {
        ESP_LOGE(TAG, "UNEXPECTED EVENT");
    }
}

esp_err_t short_handler(httpd_req_t *req)
{
    httpd_resp_send(req, (const char *)short_html_start, short_html_end - short_html_start);

    gpio_set_level(CONFIG_RELAY, 1);
    vTaskDelay(250 / portTICK_PERIOD_MS);
    gpio_set_level(CONFIG_RELAY, 0);

    return ESP_OK;
}

esp_err_t medium_handler(httpd_req_t *req)
{
    httpd_resp_send(req, (const char *)medium_html_start, medium_html_end - medium_html_start);

    gpio_set_level(CONFIG_RELAY, 1);
    vTaskDelay(500 / portTICK_PERIOD_MS);
    gpio_set_level(CONFIG_RELAY, 0);

    return ESP_OK;
}

esp_err_t long_handler(httpd_req_t *req)
{
    httpd_resp_send(req, (const char *)long_html_start, long_html_end - long_html_start);

    gpio_set_level(CONFIG_RELAY, 1);
    vTaskDelay(750 / portTICK_PERIOD_MS);
    gpio_set_level(CONFIG_RELAY, 0);

    return ESP_OK;
}

void app_main(void)
{
    vTaskDelay(500 / portTICK_PERIOD_MS);

    init_nvs();

    connect_AP();
    // init_AP();

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    httpd_handle_t server = NULL;
    httpd_start(&server, &config);

    httpd_uri_t uri_short = {
        .uri = "/short",
        .method = HTTP_GET,
        .handler = short_handler,
        .user_ctx = NULL};

    httpd_uri_t uri_medium = {
        .uri = "/medium",
        .method = HTTP_GET,
        .handler = medium_handler,
        .user_ctx = NULL};

    httpd_uri_t uri_long = {
        .uri = "/long",
        .method = HTTP_GET,
        .handler = long_handler,
        .user_ctx = NULL};

    httpd_register_uri_handler(server, &uri_short);
    httpd_register_uri_handler(server, &uri_medium);
    httpd_register_uri_handler(server, &uri_long);

    gpio_reset_pin(CONFIG_RELAY);
    gpio_set_direction(CONFIG_RELAY, GPIO_MODE_OUTPUT);

    while (1)
    {
        vTaskDelay(500 / portTICK_PERIOD_MS);
    }
}
