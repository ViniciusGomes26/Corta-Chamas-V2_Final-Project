// Ainda em desenvolvimento


#include "HCF_WNOLOGY.h"
#include "HCF_WIFI.h"
#include "esp_log.h"
#include "cJSON.h"
#include "nvs_flash.h"
#include <time.h>
#include "esp_sntp.h"
#include <string.h>
#include "mqtt_client.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_sntp.h"
#include "time.h"
#include "sys/time.h"
#include "esp_timer.h"
#include "esp_wifi.h"

/*
Exemplo de configuração
#define W_DEVICE_ID "65774aa82623fd911ab650c1" //Use o DeviceID no Wegnology  
#define W_ACCESS_KEY "76ac5ed2-ed18-4e96-9e02-d2dd572db083" //use a chave de acesso e a senha
#define W_PASSWORD "f52797619b7205bc2ac8d796d80fd0cb23f988e882cd0b82d575b26939f78c1c"
#define W_TOPICO_PUBLICAR "wnology/65774aa82623fd911ab650c1/state" //esse número no meio do tópico deve ser mudado pelo ID do seu device Wegnology
#define W_TOPICO_SUBSCREVER "wnology/65774aa82623fd911ab650c1/command" // aqui também
#define W_BROKER "mqtt://broker.app.wnology.io:1883"
#define SSID "coqueiro"
#define PASSWORD "amigos12"
*/


// HCF_WNOLOGY.c - biblioteca para integração com Wegnology via MQTT



#define MAX_BUFFER_ENTRIES 100
#define RECONNECT_INTERVAL_SECONDS 3600 // 1 hora

static const char *TAG = "HCF_WNOLOGY";

// Buffer circular de dados
typedef struct {
    char timestamp[30];
    float temperatura;
    float umidade;
} sensor_data_t;

static sensor_data_t data_buffer[MAX_BUFFER_ENTRIES];
static int buffer_index = 0;

// Controle de reconexão Wi-Fi
static time_t last_activity = 0;

static const char* SSID;
static const char* PASS;

#define MQTT_URI "mqtt://broker.app.wnology.io:1883"

static esp_mqtt_client_handle_t mqtt_client = NULL;
static char dev_id[64];
static char publish_topic[128];
static char subscribe_topic[128];
static char mqtt_username[64];
static char mqtt_password[128];

static bool mqtt_connected = false;

static void (*user_callback)(const char *, const char *) = NULL;

static QueueHandle_t mqtt_msg_queue;

typedef struct {
    char key[64];
    float value;
} mqtt_message_t;

#define MAX_HANDLERS 10
static wegnology_handler_entry_t handler_table[MAX_HANDLERS];
static int handler_count = 0;

//Esta função serve para registrar um atributo rotulado para subscrição
void wegnology_register_key_handler(const char *key, wegnology_value_handler_t handler) {
    if (handler_count < MAX_HANDLERS) {
        handler_table[handler_count].key = key;
        handler_table[handler_count].handler = handler;
        handler_count++;
    }
}

//Esta função serve para um callback interno
static void internal_callback(const char *key, const char *value) {
    for (int i = 0; i < handler_count; i++) {
        if (strcmp(handler_table[i].key, key) == 0) {
            handler_table[i].handler(value);
            return;
        }
    }
    ESP_LOGW("WNOLOGY", "Chave não reconhecida: %s", key);
}


// Callback do evento MQTT
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    esp_mqtt_event_handle_t event = event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
            esp_mqtt_client_subscribe(mqtt_client, subscribe_topic, 0);
            mqtt_connected = true;
 
            break;

        case MQTT_EVENT_DATA:
            cJSON *json = cJSON_ParseWithLength(event->data, event->data_len);

            if (json) {
                cJSON *payload = cJSON_GetObjectItem(json, "payload");

                if (payload && cJSON_IsObject(payload)) {
                    cJSON *item = payload->child;
                    while (item) {
                        if (user_callback) user_callback(item->string, item->valuestring);
                        //if()
                        item = item->next;
                    }
                }
                cJSON_Delete(json);
            }

            ESP_LOGI(TAG, "Mensagem recebida:");
            ESP_LOGI(TAG, "TOPIC=%.*s", event->topic_len, event->topic);
            ESP_LOGI(TAG, "DATA=%.*s", event->data_len, event->data);
    
            break;
        case MQTT_EVENT_DISCONNECTED:
            mqtt_connected = false;
            break;
        default:
            break;
    }
}

//Tarefa de publicação periódica do que estiver na fila
static void mqtt_publish_task(void *param) {
    mqtt_message_t msg;
    while (1) {
        if (xQueueReceive(mqtt_msg_queue, &msg, portMAX_DELAY)) {
            cJSON *root = cJSON_CreateObject();
            cJSON *data = cJSON_CreateObject();
            cJSON_AddNumberToObject(data, msg.key, msg.value);
            cJSON_AddItemToObject(root, "data", data);

            // Adiciona timestamp automático (poderia vir de um RTC ou NTP futuramente) ou receber da própria wegnology
            time_t now;
            struct tm timeinfo;
            time(&now);
            localtime_r(&now, &timeinfo);

            char iso_timestamp[30];
            strftime(iso_timestamp, sizeof(iso_timestamp), "%Y-%m-%dT%H:%M:%SZ", &timeinfo);

            cJSON_AddStringToObject(root, "time", iso_timestamp);

            char *json_str = cJSON_PrintUnformatted(root);
            esp_mqtt_client_publish(mqtt_client, publish_topic, json_str, 0, 1, 0);
            ESP_LOGI(TAG, "Publicado: %s", json_str);
            cJSON_Delete(root);
            free(json_str);
        }
    }
}

//Inicialização do NTP
void ntp_init(void)
{
    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    sntp_setservername(0, "pool.ntp.org");
    sntp_init();

    // Aguarda sincronização
    time_t now = 0;
    struct tm timeinfo = { 0 };
    int retry = 0;
    const int retry_count = 10;

    while (timeinfo.tm_year < (2020 - 1900) && ++retry < retry_count) {
        ESP_LOGI("NTP", "Aguardando sincronização NTP...");
        vTaskDelay(pdMS_TO_TICKS(2000));
        time(&now);
        localtime_r(&now, &timeinfo);
    }
    // Ajuste para horário de Brasília
    setenv("TZ", "UTC", 1);  // ou use "BRST-3BRDT-2,M10.3.0/0,M2.3.0/0" para horário de verão UTC+3
    tzset();

    time(&now);
    localtime_r(&now, &timeinfo);
    ESP_LOGI("NTP", "Tempo sincronizado (Brasília): %s", asctime(&timeinfo));
}
 
// Tarefa de reconexão automática do broker
void mqtt_connection_watchdog(void *param) {
    while (true) {
        if (!mqtt_connected) {
            ESP_LOGW(TAG, "MQTT desconectado! Tentando reconectar...");
            esp_mqtt_client_reconnect(mqtt_client); // já tenta reconectar se possível
        }
        vTaskDelay(pdMS_TO_TICKS(10000)); // checa a cada 10 segundos
    }
}

// Inicializa Wi-Fi e MQTT
void iniciar_wnology_wifi(const char *ssid, const char *pass, const char *device_id, const char *user_name, const char *access_token) {

    SSID = ssid;
    PASS = pass;
    nvs_flash_init();
    //tcpip_adapter_init();
    wifi_init();
    wifi_connect_sta(ssid, pass, 10000);

    ntp_init();

    snprintf(dev_id, sizeof(dev_id), "%s", device_id);
    snprintf(publish_topic, sizeof(publish_topic), "wnology/%s/state", device_id);
    snprintf(subscribe_topic, sizeof(subscribe_topic), "wnology/%s/command", device_id);
    snprintf(mqtt_username, sizeof(mqtt_username), "%s", user_name);
    snprintf(mqtt_password, sizeof(mqtt_password), "%s", access_token);

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = MQTT_URI,
        .credentials.set_null_client_id = false,  
        .credentials.client_id = dev_id,
        .credentials.username = mqtt_username,
        .credentials.authentication.password = mqtt_password,
    };
    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(mqtt_client);

    mqtt_msg_queue = xQueueCreate(10, sizeof(mqtt_message_t));
    xTaskCreate(mqtt_publish_task, "mqtt_pub_task", 4096, NULL, 5, NULL);

    mqtt_wegnology_register_callback(internal_callback);
    xTaskCreate(mqtt_connection_watchdog, "mqtt_connection_watchdog", 4096, NULL, 5, NULL);

  //  xTaskCreate(periodic_reconnect_task, "periodic_reconnect_task", 4096, NULL, 5, NULL);
}

//Envio de atributo numérico
void mqtt_wegnology_send_float(const char *key, float value){
    mqtt_message_t msg;
    snprintf(msg.key, sizeof(msg.key), "%s", key);
    msg.value = value;
    xQueueSend(mqtt_msg_queue, &msg, portMAX_DELAY);
}

//Para o MQTT
void mqtt_wegnology_stop() {
    if (mqtt_client) {
        esp_mqtt_client_stop(mqtt_client);
        esp_mqtt_client_destroy(mqtt_client);
        mqtt_client = NULL;
    }
}

//Configura dos tópicos de subscrição e publicação
void mqtt_wegnology_set_topics(const char *pub, const char *sub) {
    strncpy(publish_topic, pub, sizeof(publish_topic));
    strncpy(subscribe_topic, sub, sizeof(subscribe_topic));
}


// Publica um único par chave/valor
mqtt_wegnology_status_t mqtt_wegnology_publish(const char *key, const char *value) {
    if (!mqtt_client || !key || !value) return MQTT_WEGNOLOGY_ERROR_NULL;
    if (!mqtt_connected) return MQTT_WEGNOLOGY_ERROR_NOT_CONNECTED;

    cJSON *root = cJSON_CreateObject();
    if (!root) return MQTT_WEGNOLOGY_ERROR_JSON;

    cJSON_AddStringToObject(root, key, value);
    char *json_str = cJSON_PrintUnformatted(root);
    mqtt_wegnology_status_t status = MQTT_WEGNOLOGY_OK;

    if (esp_mqtt_client_publish(mqtt_client, publish_topic, json_str, 0, 1, 0) < 0) {
        status = MQTT_WEGNOLOGY_ERROR_JSON;
    }

    free(json_str);
    cJSON_Delete(root);
    return status;
}



// Publica múltiplos pares chave/valor + timestamp
mqtt_wegnology_status_t mqtt_wegnology_publish_json(const char **keys, const char **values, int count, int include_timestamp) {
    if (!mqtt_client || !keys || !values || count <= 0) return MQTT_WEGNOLOGY_ERROR_NULL;
    if (!mqtt_connected) return MQTT_WEGNOLOGY_ERROR_NOT_CONNECTED;

    cJSON *root = cJSON_CreateObject();
    cJSON *data = cJSON_CreateObject();
    if (!root || !data) return MQTT_WEGNOLOGY_ERROR_JSON;

    for (int i = 0; i < count; i++) {
        cJSON_AddStringToObject(data, keys[i], values[i]);
    }

    cJSON_AddItemToObject(root, "data", data);

    if (include_timestamp) {
        time_t now;
        time(&now);
        struct tm timeinfo;
        localtime_r(&now, &timeinfo);

        char timestamp[30];
        strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%SZ", &timeinfo);

        cJSON_AddStringToObject(root, "time", timestamp);
    }

    char *json_str = cJSON_PrintUnformatted(root);
    mqtt_wegnology_status_t status = MQTT_WEGNOLOGY_OK;

    if (esp_mqtt_client_publish(mqtt_client, publish_topic, json_str, 0, 1, 0) < 0) {
        status = MQTT_WEGNOLOGY_ERROR_JSON;
    }

    free(json_str);
    cJSON_Delete(root);
    return status;
}

//Registro do callback
void mqtt_wegnology_register_callback(void (*callback)(const char *key, const char *value)) {
    user_callback = callback;
}


//Atualiza a conexão
void mqtt_wegnology_set_connected(bool connected) {
    mqtt_connected = connected;
    time(&last_activity); // Atualiza último evento de atividade
}

//Adciona ao buffer de publicação em batelada
void buffer_add(float temp, float umidade) {
    if (buffer_index >= MAX_BUFFER_ENTRIES) return;
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);
    strftime(data_buffer[buffer_index].timestamp, sizeof(data_buffer[buffer_index].timestamp), "%Y-%m-%dT%H:%M:%SZ", &timeinfo);
    data_buffer[buffer_index].temperatura = temp;
    data_buffer[buffer_index].umidade = umidade;
    buffer_index++;
}

//Publica o buffer de batelada
void enviar_buffer() {
if (!mqtt_connected || buffer_index == 0) return;

    for (int i = 0; i < buffer_index; i++) {
        cJSON *root = cJSON_CreateObject();
        cJSON *data = cJSON_CreateObject();

        cJSON_AddNumberToObject(data, "Temperatura", data_buffer[i].temperatura);
        cJSON_AddNumberToObject(data, "Umidade", data_buffer[i].umidade);
        

        cJSON_AddItemToObject(root, "data", data);
        cJSON_AddStringToObject(root, "time", data_buffer[i].timestamp);


        char *json_str = cJSON_PrintUnformatted(root);
        esp_mqtt_client_publish(mqtt_client, publish_topic, json_str, 0, 1, 0);

        free(json_str);
        cJSON_Delete(root);

        // Aguarda 2 segundos entre cada envio
        vTaskDelay(pdMS_TO_TICKS(2000));
    }

    // Após envio, esvazia o buffer
    buffer_index = 0;
    return;
}

//Verifica a conexão wifi
void check_wifi_reconnection() {
    time_t now;
    time(&now);
    if ((now - last_activity) > RECONNECT_INTERVAL_SECONDS) {
        ESP_LOGW(TAG, "Reconectando Wi-Fi após inatividade");
        esp_wifi_disconnect();
        esp_wifi_connect();
        time(&last_activity);
    }
}

//Reinicia o temporizador de reconexão
void on_wifi_event(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED) {
        time(&last_activity);
        ESP_LOGI(TAG, "Wi-Fi reconectado, temporizador reiniciado");
    }
}

//Tarefa de reconexão
void periodic_reconnect_task(void *param) {
    while (1) {
        time_t now;
        time(&now);

        if (difftime(now, last_activity) > RECONNECT_INTERVAL_SECONDS) {
            ESP_LOGI(TAG, "Reconectando Wi-Fi para envio em lote...");
            wifi_connect_sta(SSID, PASS, 10000); // reconectar
            ntp_init(); // re-sincronizar o horário se necessário
            enviar_buffer(); // envia os dados em lote
            wifi_disconnect(); // desconecta para economia de energia
            last_activity = now;
        }
        vTaskDelay(pdMS_TO_TICKS(10000)); // Verifica a cada 10 segundos
    }
}


//Publica em formato JSON
mqtt_wegnology_status_t mqtt_wegnology_publish_json_with_data_root(const char **keys, const char **values, int count, int include_timestamp) {
    if (!mqtt_client || !keys || !values || count <= 0) return MQTT_WEGNOLOGY_ERROR_NULL;
    if (!mqtt_connected) return MQTT_WEGNOLOGY_ERROR_NOT_CONNECTED;

    cJSON *root = cJSON_CreateObject();
    cJSON *data = cJSON_CreateObject();
    if (!root || !data) return MQTT_WEGNOLOGY_ERROR_JSON;

    for (int i = 0; i < count; i++) {
        cJSON_AddStringToObject(data, keys[i], values[i]);
    }
    cJSON_AddItemToObject(root, "data", data);

    if (include_timestamp) {
        time_t now;
        time(&now);
        struct tm timeinfo;
        localtime_r(&now, &timeinfo);

        char timestamp[30];
        strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%SZ", &timeinfo);

        cJSON_AddStringToObject(root, "time", timestamp);
    }

    char *json_str = cJSON_PrintUnformatted(root);
    mqtt_wegnology_status_t status = MQTT_WEGNOLOGY_OK;

    if (esp_mqtt_client_publish(mqtt_client, publish_topic, json_str, 0, 1, 0) < 0) {
        status = MQTT_WEGNOLOGY_ERROR_JSON;
    }

    free(json_str);
    cJSON_Delete(root);
    return status;
}
