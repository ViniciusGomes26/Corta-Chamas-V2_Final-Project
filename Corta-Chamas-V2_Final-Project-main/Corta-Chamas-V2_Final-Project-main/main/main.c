#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/ledc.h" // Biblioteca de controle de PWM
// #include "HCF_WNOLOGY.h" // Biblioteca de comunicação com wegnology

// ---------------------------MACROS------------------------------------------------------------------------
// Valor da velocidade do motor (0 a 65535 para resolução de 16-bits)
#define SPEED 65535   

// ---------------------------DEFINIÇÃO DOS PINOS-----------------------------------------------------------
#define PIN_VISION 19 // definição do pino de sinal
#define PIN_FIRE_DETECTOR 4 // definição do pino receptor do sensor
#define PIN_MOV_BOMB 18 // definição do pino do servo da bomba

// Pinos do ESP32 conectados ao módulo drive
#define PWM_BOMB 23           // Pino principal (Velocidade por PWM)
#define PWM_BOMB_REFERENCE 22  // Pino de referência  (Mantido em 0)

// definição do IHM

#define BUTTON_ON 34
#define DEBOUNCE_TIME 50
#define BUTTON_OFF 35
  
// -------------------------------------IHM----------------------------------------------------------------

void buttons ()
{
    gpio_config_t io_conf = {           // configuração de pinos para o botão ligar e desligar
        .pin_bit_mask = (1ULL << BUTTON_ON) | (1ULL << BUTTON_OFF),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLDOWN_ENABLE,
    };
      gpio_config(&io_conf);
}


/* ------------------------------------IoT-----------------------------------------------------------------
#define ESP 4

#define WIFI_SSID "nome da internet"
#define WIFI_PASS "senha"


#if (ESP==1)
    #define DEVICE_ID "65774aa82623fd911ab650c1" //ESP de testes
#elif (ESP==2)
    #define DEVICE_ID "6810f5b23c10b7b2e9e4e6d8" //ESP2
#elif (ESP==3)
    #define DEVICE_ID "6811023f30642df2ffeaa490" //ESP3
#else
    #define DEVICE_ID "6811025d30642df2ffeaa4da" //ESP4
#endif

#define W_ACCESS_KEY "76ac5ed2-ed18-4e96-9e02-d2dd572db083" //use a chave de acesso e a senha
#define W_PASSWORD "f52797619b7205bc2ac8d796d80fd0cb23f988e882cd0b82d575b26939f78c1c"

#define IN(x) (entradas>>x)&1

// Área de declaração de variáveis e protótipos de funções
//-----------------------------------------------------------------------------------------------------------------------

char *TAG = "Placa";
uint8_t entradas, saidas = 0; //variáveis de controle de entradas e saídas
char tecla = '-' ;
char escrever[40];
bool direcao = false;
int angulo = 0;
float temperatura = 0.0, umidade = 0.0; */


// --------------------------------SERVOS MOTORES----------------------------------------------------------
uint32_t angle_to_duty(int angle) { // definição do limite do giro

    uint32_t min_spin = 500; // 0 Graus
    uint32_t max_spin = 2500; // 180 Graus
    uint32_t pulse = min_spin + ((max_spin - min_spin) * angle) / 180; // Controle de pulso que varia a depender do sentido
    return (pulse * 65535) / 20000; // é um conversor de mS para duty cicle

}

void servo_config() { // configuração do servo via biblioteca Ledc

    ledc_timer_config_t timer = { // controle do PWM
        .speed_mode = LEDC_LOW_SPEED_MODE, 
        .timer_num = LEDC_TIMER_0,
        .duty_resolution = LEDC_TIMER_16_BIT,
        .freq_hz = 50,
        .clk_cfg = LEDC_AUTO_CLK
    };
    ledc_timer_config(&timer);

    ledc_channel_config_t channel = { // config do servo visão
        .gpio_num = PIN_VISION,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
        .hpoint = 0
    };
    ledc_channel_config(&channel);


     ledc_channel_config_t channel_bomb = {  // config do servo bomba
        .gpio_num = PIN_MOV_BOMB,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_1, 
        .timer_sel = LEDC_TIMER_0, 
        .duty = 0,
        .hpoint = 0
    };
    ledc_channel_config(&channel_bomb); 
}

   void servo_update(ledc_channel_t channel, int angle) {  // controle da posição exata do servo via canais  (CHANNEL 1 E 0)
    uint32_t duty = angle_to_duty(angle);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, channel, duty); 
    ledc_update_duty(LEDC_LOW_SPEED_MODE, channel); 
}

// --------------------------------------CONFIGURAÇÃO DA BOMBA -----------------------------------------------
void BOMB_CONFIG() {
    
    gpio_config_t io_confh = {               
        .pin_bit_mask = (1ULL << PWM_BOMB_REFERENCE),
        .mode = GPIO_MODE_OUTPUT,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&io_confh); 
    gpio_set_level(PWM_BOMB_REFERENCE, 0); // Garante o referencial de rotação direta

    ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE, 
        .timer_num = LEDC_TIMER_1,
        .duty_resolution = LEDC_TIMER_16_BIT,
        .freq_hz = 980,                    // Frequência ideal para motores DC menores
        .clk_cfg = LEDC_AUTO_CLK
    };
    ledc_timer_config(&timer);

    // Configura o Canal PWM associado ao pino principal

    ledc_channel_config_t ledc_channel_water = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_3,
        .timer_sel = LEDC_TIMER_1,
        .intr_type = LEDC_INTR_DISABLE,
        .gpio_num = PWM_BOMB,
        .duty = 0 // Inicializa desligado por segurança
    };
    ledc_channel_config(&ledc_channel_water);
}

// Função para definir a força/velocidade da bomba
void BOMB_POWER(uint32_t duty_cycle) {
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_3, duty_cycle);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_3);
}

// ------------------------------CONFIGURAÇÃO DO SENSOR---------------------------------------------------
void sensor_infra_config () {
    
    gpio_config_t io_config = {               
        .pin_bit_mask = (1ULL << PIN_FIRE_DETECTOR),
        .mode = GPIO_MODE_INPUT,
    };
    gpio_config(&io_config); 

}

// --------------------------------PARAMETROS DE SINCRONIZAÇÃO--------------------------------------------
void inspection_obstacle (int target_angle) {
    
    while (gpio_get_level(PIN_FIRE_DETECTOR) == 0) {
        
       
        servo_update(LEDC_CHANNEL_1, target_angle);
        vTaskDelay(pdMS_TO_TICKS(200)); // Aguarda o servo da bomba se posicionar
        
        BOMB_POWER(SPEED); // ativação da bomba
        vTaskDelay(pdMS_TO_TICKS(1000)); // Mantém ligada por 1 segundo
    }
    BOMB_POWER(0);  // desliga a bomba
}

void servo_and_bomb_task (void *pvParameters) { 
    int compensacao = 6; // Ajuste de valor (em graus) 

    while (1) {
        // Sentido horário (0 a 180)
        for (int p = 60; p <= 120; p++) { // angulo de monitoramento do servo
            servo_update(LEDC_CHANNEL_0, p); // Atualiza o servo de visão
            vTaskDelay(pdMS_TO_TICKS(30));   

            int angulo_alvo = (p - compensacao < 0) ? 0 : (p - compensacao);
            inspection_obstacle(angulo_alvo);    
        }

        // Sentido anti-horário (180 a 0)
        for (int p = 120; p >= 60; p--) {  
            servo_update(LEDC_CHANNEL_0, p); 
            vTaskDelay(pdMS_TO_TICKS(30));   

            int angulo_alvo = (p + compensacao > 180) ? 180 : (p + compensacao);
            inspection_obstacle(angulo_alvo);    
        }
    }
}
// ----------------------------------------------------------------------------------------------------

void app_main (){

    buttons ();
    
  while (1)
    {
        if (gpio_get_level(BUTTON_ON) == 0)
        {
            BOMB_CONFIG(); // Inicializa o hardware da bomba
            sensor_infra_config (); // Inicializa do sensor 
            servo_config(); // Inicializa o hardware dos servos
            xTaskCreate(servo_and_bomb_task, "servo_and_bomb_task", 3072, NULL, 5, NULL); // tarefa dos servos e bomba
            vTaskDelay(pdMS_TO_TICKS(15));

            gpio_set_level (PIN_FIRE_DETECTOR, 1);
            gpio_set_level (PIN_VISION, 1);
            vTaskDelay(pdMS_TO_TICKS(200));  // debounce fixo
        }
          if (gpio_get_level(BUTTON_OFF) == 0) 
          {
              BOMB_CONFIG(0); // Inicializa o hardware da bomba
              sensor_infra_config (0); // Inicializa do sensor 
              servo_config(0); // Inicializa o hardware dos servos
              gpio_set_level (PIN_FIRE_DETECTOR, 0);
              gpio_set_level (PIN_VISION, 0);
              vTaskDelay(pdMS_TO_TICKS(200)); //// debounce fixo
          }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
 
} 