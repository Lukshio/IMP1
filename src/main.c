// Autor: Lukáš Ježek xjezek19

#include <string.h>
#include <sys/time.h>
#include "math.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"
#include "esp_spiffs.h"
#include "esp_http_server.h"
#include "esp_adc_cal.h"

#include "nvs_flash.h"

#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include "lwip/netdb.h"
#include "lwip/dns.h"

#include "driver/gpio.h"
#include "driver/adc.h"
#include "driver/rtc_io.h"


#define LED_PIN GPIO_NUM_2 // GPIO pin where your LED is connected
#define SENSOR_ADC_PIN ADC1_CHANNEL_6

// bitove posuny pro definice stavu wifi
#define WIFI_SUCCESS 1
#define WIFI_FAILURE 0010

// Wifi udaje
#define WIFI_SSID ""
#define WIFI_PASS ""

// Proměnná pro uložení hodnoty z formuláře
static char input_value[32];
double number = 22;
double histerze = 1.5;
int records = 0;
double globTemp = 0;

static const char *TAG = "IMP PROJEKT Lukáš Ježek xjezek19";

static esp_err_t root_handler(httpd_req_t *req)
{
    // Odeslání hlavní stránky
    char response[1050];

    snprintf(response, sizeof(response),
             "<!DOCTYPE html><html><head> "
             "<meta charset=\"UTF-8\"><title>IMP Projekt</title>"
             "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\" />"
             "</head><body>"
             "<h1>IMP Projekt</h1>"
             "<h2>Poslední teplota: %.2f </h2>"
             "<form id=\"myForm\" action=\"/submit\" method=\"get\">"
             "   <label for=\"value\">Zadejte teplotu:</label>"
             "   <input type=\"number\" step=\"0.1\" id=\"value\" name=\"value\"><br><br>"
             "   <label for=\"hist\">Zadejte histerzi:</label>"
             "   <input type=\"number\" step=\"0.1\" id=\"hist\" name=\"histerze\" min=\"0\"><br><br>"
             "   <input type=\"button\" value=\"Uložit\" onclick=\"updateValue()\">"
             "</form> "
             "<iframe src=\"/historie.txt\" width=\"350\" height=\"500\"></iframe> "
             "<script>"
             "   function updateValue() {"
             "       var inputValue = document.getElementById('value').value;"
             "       var histVal = document.getElementById('hist').value;"
             "       fetch('/submit?value=' + inputValue + '&hist=' + histVal)"
             "           .then(response => window.location.reload());"
             "   }"
             "   document.getElementById('value').value = %.2f;"
             "   document.getElementById('hist').value = %.2f;"
             "</script>"
             "</body></html>",
             globTemp, number, histerze);

    httpd_resp_send(req, response, strlen(response));
    return ESP_OK;
}


// Stránka s historií
esp_err_t get_file_handler(httpd_req_t *req)
{
    FILE *file = fopen("/spiffs/historie.txt", "r");
    if (file == NULL)
    {
        ESP_LOGE(TAG, "Failed to open file");
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "text/plain");

    char buffer[1024];
    size_t bytesRead;
    do
    {
        bytesRead = fread(buffer, 1, sizeof(buffer), file);
        if (bytesRead > 0)
        {
            if (httpd_resp_send_chunk(req, buffer, bytesRead) != ESP_OK)
            {
                fclose(file);
                ESP_LOGE(TAG, "File sending failed");
                return ESP_FAIL;
            }
        }
    } while (bytesRead > 0);

    fclose(file);

    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

// Získání dat z formuláře a uložení do globálních proměnných
static esp_err_t submit_handler(httpd_req_t *req)
{
    // Získání dat z formuláře
    char buf[64];
    httpd_req_get_url_query_str(req, buf, sizeof(buf));
    char value[32];

    if (httpd_query_key_value(buf, "value", value, sizeof(value)) == ESP_OK)
    {
        // Uložení prahové hodnoty
        strncpy(input_value, value, sizeof(input_value) - 1);
        ESP_LOGI(TAG, "Hodnota uložena: %s", input_value);
        number = atof(input_value);
    }
    if (httpd_query_key_value(buf, "hist", value, sizeof(value)) == ESP_OK)
    {
        // Uložení hodnoty histerze
        strncpy(input_value, value, sizeof(input_value) - 1);
        ESP_LOGI(TAG, "Hodnota uložena: %s", input_value);
        float histCheck = atof(input_value);

        // kontrola záporné histerze
        if (histCheck < 0) histerze = 0;
        else histerze = histCheck;
    }

    // Přesměrování na hlavní stránku
    httpd_resp_set_status(req, "303");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

// definice cest a metod
static const httpd_uri_t uri_get_file = {
    .uri = "/historie.txt",
    .method = HTTP_GET,
    .handler = get_file_handler,
    .user_ctx = NULL};

static const httpd_uri_t root_uri = {
    .uri = "/",
    .method = HTTP_GET,
    .handler = root_handler,
    .user_ctx = NULL};

static const httpd_uri_t submit_uri = {
    .uri = "/submit",
    .method = HTTP_GET,
    .handler = submit_handler,
    .user_ctx = NULL};

static const httpd_uri_t not_found_uri = {
    .uri = "/*",
    .method = HTTP_GET,
    .handler = httpd_resp_send_404,
    .user_ctx = NULL};


// Vytvoření HTTP serveru
void http_server_task(void *pvParameter)
{
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();

    // registrace url
    if (httpd_start(&server, &config) == ESP_OK)
    {
        httpd_register_uri_handler(server, &root_uri);
        httpd_register_uri_handler(server, &submit_uri);
        httpd_register_uri_handler(server, &not_found_uri);
        httpd_register_uri_handler(server, &uri_get_file);
    }

// aktualizace co 0.1s
    ESP_LOGI(TAG, "Start webserveru");
    while (1)
    {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}


static EventGroupHandle_t wifi_event_group;
static int pocetPokusu = 0;

// handler pro wifi
static void wifi_handle(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        ESP_LOGI(TAG, "Připojování k WIFI... ");
        esp_wifi_connect();
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        //max 15 pokusu
        if (pocetPokusu < 15)
        {
            ESP_LOGI(TAG, "Pokus o opětovné připojení... ");
            esp_wifi_connect();
            pocetPokusu++;
        }
        else
        {
            xEventGroupSetBits(wifi_event_group, WIFI_FAILURE);
        }
    }
}

// handler pro IP adresy
static void ip_handle(void *arg, esp_event_base_t event_base,
                             int32_t event_id, void *event_data)
{
    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "IP adresa: " IPSTR, IP2STR(&event->ip_info.ip));

        // reset poctu pokudu
        pocetPokusu = 0;
        xEventGroupSetBits(wifi_event_group, WIFI_SUCCESS);
    }
}

// pripojeni k wifi a predani vysledku
esp_err_t connect_wifi()
{
    // initializace
    int status = WIFI_FAILURE;
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();

    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    wifi_event_group = xEventGroupCreate();

    // wifi handler
    esp_event_handler_instance_t wifi_handler_event_instance;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_handle,
                                                        NULL,
                                                        &wifi_handler_event_instance));

    // ip hander
    esp_event_handler_instance_t got_ip_event_instance;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &ip_handle,
                                                        NULL,
                                                        &got_ip_event_instance));

    // setup udaju k wifi pripojeni
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = {
                .capable = true,
                .required = false},
        },
    };

    // nastaveni wifi jako klienta
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));

    // start wifi
    ESP_ERROR_CHECK(esp_wifi_start());

    // cekani na pripojeni
    EventBits_t bits = xEventGroupWaitBits(wifi_event_group,
                                           WIFI_SUCCESS | WIFI_FAILURE,
                                           pdFALSE,
                                           pdFALSE,
                                           portMAX_DELAY);
    // kontrola vysledku 
    if (bits & WIFI_SUCCESS)
    {
        ESP_LOGI(TAG, "Připojeno k WIFI ");
        status = WIFI_SUCCESS;
    }
    else if (bits & WIFI_FAILURE)
    {
        ESP_LOGI(TAG, "Nepodařilo se připojit");
        status = WIFI_FAILURE;
    }
    else
    {
        ESP_LOGE(TAG, "====== Nastala chyba při připojování");
        status = WIFI_FAILURE;
    }

    // ukončení handlerů po připojení
    ESP_ERROR_CHECK(esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, got_ip_event_instance));
    ESP_ERROR_CHECK(esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_handler_event_instance));
    vEventGroupDelete(wifi_event_group);

    return status;
}


// Funkce pro přidání stringu na konec souboru
void append_string_to_file(const char *filename, const char *data)
{
    FILE *file = fopen(filename, "a");
    if (file == NULL)
    {
        ESP_LOGE(TAG, "Nepodařilo se otevřít soubor s historí");
        return;
    }

    fprintf(file, "%s", data);
    fclose(file);
}

// Funkce pro přepsání souboru stringem
void save_string_to_file(const char *filename, const char *data) {
    FILE *file = fopen(filename, "w");
    if (file == NULL) {
        ESP_LOGE(TAG, "Nepodařilo se otevřít soubor pro zápis");
        return;
    }

    fprintf(file, "%s", data);
    fclose(file);
}

void app_main(void)
{
    // Inicializace GPIO pro LED
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << LED_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .intr_type = GPIO_INTR_DISABLE,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE};
    gpio_config(&io_conf);

    // Inicializace uloziste SPIFFS

    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = NULL,
        .max_files = 5,
        .format_if_mount_failed = true};

    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK)
    {
        if (ret == ESP_FAIL)
        {
            ESP_LOGE(TAG, "Chyba připojení nebo formátování úložiště");
        }
        else if (ret == ESP_ERR_NOT_FOUND)
        {
            ESP_LOGE(TAG, "Nepodařilo se najít SPIFFS oddíl");
        }
        else
        {
            ESP_LOGE(TAG, "Nepodařilo se inicializovat SPIFFS (%s)", esp_err_to_name(ret));
        }
        return;
    }

    // nazev souboru a vlozeni nadpisu
    const char *filename = "/spiffs/historie.txt";
    const char *data_to_save = "Historie teplot\n";
    save_string_to_file(filename, data_to_save);

    // inicializace uloziste
    ESP_ERROR_CHECK(nvs_flash_init());
    esp_err_t status = WIFI_FAILURE;

    // Pripojeni k wifi
    status = connect_wifi();
    if (WIFI_SUCCESS != status)
    {
        ESP_LOGI(TAG, "Nepodarilo se pripojit, ukoncuji...");
        return;
    }

    // vytvoreni tasku pro http server
    xTaskCreate(&http_server_task, "http_server_task", 4096, NULL, 5, NULL);

    // inicializace ADC prevodniku pro teplotni cidlo
    static esp_adc_cal_characteristics_t adc1_chars;
    esp_adc_cal_characterize(ADC_UNIT_1, ADC_ATTEN_DB_11, ADC_WIDTH_BIT_DEFAULT, 0, &adc1_chars);

    ESP_ERROR_CHECK(adc1_config_width(ADC_WIDTH_BIT_DEFAULT));
    ESP_ERROR_CHECK(adc1_config_channel_atten(SENSOR_ADC_PIN, ADC_ATTEN_DB_11));


    // Čas
    time_t now;
    char strftime_buf[64];
    struct tm timeinfo;

    time(&now);
    // Nastavení časového pásma
    setenv("TZ", "GMT+1", 1);
    tzset();

    localtime_r(&now, &timeinfo);
    strftime(strftime_buf, sizeof(strftime_buf), "%c", &timeinfo);
    ESP_LOGI(TAG, "Systémový čas: %s", strftime_buf);

    // konfigurace a synchronizace s NTP serverem
    ESP_LOGI(TAG, "Pokus o aktualizaci času z NTP ");

    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("tik.cesnet.cz");
    esp_netif_sntp_init(&config);

    if (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(15000)) != ESP_OK)
    {
        printf("Nepodařilo se aktualizovat čas ze serveru po 15 vteřinách");
    }
    else
    {
        printf("------------- CAS AKTUALIZOVAN Z NTP ------------- \n");
    }

    // Získání a výpis aktuálního času
    time(&now);
    localtime_r(&now, &timeinfo);
    strftime(strftime_buf, sizeof(strftime_buf), "%c", &timeinfo);
    ESP_LOGI(TAG, "Aktualni čas: %s", strftime_buf);

    // Hlavni smycka programu
    ESP_LOGI(TAG, "Začátek měření");

    while (1)
    {
        // ziskani hodnoty z cidla v mV
        int mV = esp_adc_cal_raw_to_voltage(adc1_get_raw(SENSOR_ADC_PIN), &adc1_chars);

        // vypocet teploty dle dokumentace cidla
        float temp = (8.194 - (sqrt((-8.194 * -8.194) + (4 * 0.00262 * (1324 - (mV)))))) / (2 * -0.00262) + 30;

        // ulozeni do glob promenne a vypis
        globTemp = temp;
        printf("Napětí: %d mV, ", mV);
        printf("Temp: %.2f C, ", temp);
        printf("Nastavena teplota: %.2f C ", number);

        // rozsviceni a zhasinani ledky
        if (temp <= (number - histerze)) 
        {
            gpio_set_level(LED_PIN, 0);
            printf("LED ON \n");
        }
        else if (temp > (number + histerze)) 
        {
            gpio_set_level(LED_PIN, 1);
            printf("LED OFF \n");
        } else printf("LED nezměněna \n");

        // ziskani aktualniho casu
        struct tm timeinfo;
        time(&now);
        localtime_r(&now, &timeinfo);
        strftime(strftime_buf, sizeof(strftime_buf), "%c", &timeinfo);

        // zapis do souboru, pridani na konec
        append_string_to_file(filename, strftime_buf);
        append_string_to_file(filename, " Teplota: ");
        char tempString[7];
        sprintf(tempString, "%.2f", temp);
        append_string_to_file(filename, tempString);

        append_string_to_file(filename, " C\n");

        // cekani 5 vterin na dalsi cyklus
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}