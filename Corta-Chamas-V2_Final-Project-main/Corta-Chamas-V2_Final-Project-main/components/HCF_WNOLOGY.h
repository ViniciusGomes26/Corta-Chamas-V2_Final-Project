#ifndef __HCF_WNOLOGY_H
    #define __HCF_WNOLOGY_H

//#include "mqtt_client.h"

// Retorno de status da publicação
typedef enum {
    MQTT_WEGNOLOGY_OK = 0,
    MQTT_WEGNOLOGY_ERROR_NOT_CONNECTED,
    MQTT_WEGNOLOGY_ERROR_NULL,
    MQTT_WEGNOLOGY_ERROR_JSON,
} mqtt_wegnology_status_t;

typedef void (*wegnology_value_handler_t)(const char *value);

typedef struct {
    const char *key;
    wegnology_value_handler_t handler;
} wegnology_handler_entry_t;

// Inicialização do módulo MQTT + Wi-Fi -> Broker "mqtt://broker.app.wnology.io:1883"
//ssid e pass são dados do wifi, user_name e access_token são extraídos do arquivo acces key do wnology e device_id é o endereço do dispositivo no wegnology
void iniciar_wnology_wifi(const char *ssid, const char *pass, const char *device_id, const char *user_name, const char *access_token);

// Definição dos tópicos de publicação e subscrição
void mqtt_wegnology_set_topics(const char *publish_topic, const char *subscribe_topic);

//Envio de atributo em formato float
void mqtt_wegnology_send_float(const char *key, float value);

// Versão estendida para múltiplas chaves/valores
mqtt_wegnology_status_t mqtt_wegnology_publish_json(const char **keys, const char **values, int count, int include_timestamp);

// Versão simplificada para chave/valor único
mqtt_wegnology_status_t mqtt_wegnology_publish(const char *key, const char *value);

// Callback customizado para processar dados recebidos via subscrição
void mqtt_wegnology_register_callback(void (*on_message)(const char *key, const char *value));
void wegnology_register_key_handler(const char *key, wegnology_value_handler_t handler);


void buffer_add(float temp, float umidade);
void enviar_buffer();
void check_wifi_reconnection();

#endif