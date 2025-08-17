#include <esp_err.h>

/* STA Configuration */
#define EXAMPLE_ESP_WIFI_STA_SSID           "xikas"
#define EXAMPLE_ESP_WIFI_STA_PASSWD         "300075zl"
#define EXAMPLE_ESP_MAXIMUM_RETRY           10

/* AP Configuration */
#define EXAMPLE_ESP_WIFI_AP_SSID            "paipaizhu"
#define EXAMPLE_ESP_WIFI_AP_PASSWD          "12345678"
#define EXAMPLE_ESP_WIFI_CHANNEL            1
#define EXAMPLE_MAX_STA_CONN                4

esp_err_t init_wifi(void);
esp_err_t wifi_connected(void);