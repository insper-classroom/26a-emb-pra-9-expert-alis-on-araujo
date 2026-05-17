#include <FreeRTOS.h>
#include <queue.h>
#include <stdio.h>
#include <string.h>
#include <task.h>
#include <stdlib.h>
#include <semphr.h>

#include "hardware/irq.h"
#include "hardware/uart.h"
#include "hardware/i2c.h"
#include "hardware/gpio.h"
#include "hardware/pwm.h"
#include "hardware/adc.h"
#include "pico/stdlib.h"

#include "hc06/hc06.h"
#include "ssd1306/ssd1306.h"
#include "pins/pins.h"

#define HC06_NAME "LAB-EXPERT-BT"
#define HC06_PIN "1234"

#define LED_R_PIN 7
#define LED_G_PIN 8
#define LED_B_PIN 9

typedef struct {
    int eixo;
    int16_t valor;
} joystick_data;

QueueHandle_t xQueueRX;
QueueHandle_t xQueueTX;
QueueHandle_t xQueueADC;

// Atualizado sempre que chega algo do HC-06 (inclui heartbeat 0x00).
static volatile uint32_t g_hc06_last_rx_ms = 0;

ssd1306_t disp;

const uint BTN = 6;

void x_task(void *p){

    int ind = 0;
    joystick_data dados_env;
    int samples[5] = {0, 0, 0, 0, 0};
    int soma = 0;
    dados_env.eixo = 0;

    while(1){
        adc_select_input(0); 
        uint16_t raw_x = adc_read();
        soma = soma - samples[ind];
        samples[ind] = raw_x; //subst amostra mais antiga pela nova leitura
        soma += samples[ind];
        
        int filtrado_x = soma / 5; //nova media
        int centrado_x = filtrado_x - 2047;
        int x_escala_certa = centrado_x / 8 ; 
        
        if((x_escala_certa > -30) && (x_escala_certa < 30)){ //deadzone
            dados_env.valor = 0;            
        }else{
            dados_env.valor = x_escala_certa * 0.1;
        }
        
        if(dados_env.valor != 0){
            if(dados_env.valor != 0xFF){
                //envia o struct
                xQueueSend(xQueueADC,&dados_env,0);
            }
        }

        ind = (ind + 1)% 5; //força voltar pro começo quando chegar no final do array
        
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void y_task(void *p){
    int ind = 0;
    joystick_data dados_env;
    int samples[5] = {0, 0, 0, 0, 0};
    int soma = 0;
    dados_env.eixo = 1;

    while(1){
        adc_select_input(1);
        uint16_t raw_y = adc_read();  
        soma = soma - samples[ind];
        samples[ind] = raw_y;
        soma += samples[ind];
        
        int filtrado_y = soma / 5;
        int centrado_y = filtrado_y - 2047;
        int y_escala_certa = centrado_y/8;
        
        if((y_escala_certa > -30) && (y_escala_certa < 30)){
            dados_env.valor = 0;
        }else{
            dados_env.valor = -y_escala_certa * 0.1;
        }
    
       if(dados_env.valor != 0){
            if(dados_env.valor != 0xFF){
                xQueueSend(xQueueADC,&dados_env,0);
            }
        }

        ind = (ind + 1)% 5;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void click_task(void *p) {
    // Configura o pino como entrada com pull-up
    gpio_init(JOYSTICK_SW);
    gpio_set_dir(JOYSTICK_SW, GPIO_IN);
    gpio_pull_up(JOYSTICK_SW);

    joystick_data dados_env;
    dados_env.eixo = 2; // O ID "2" vai significar CLIQUE para o nosso Python depois
    
    bool estado_anterior = true; // true porque com pull-up, solto é 1

    while(1) {
        bool estado_atual = gpio_get(JOYSTICK_SW);
        
        // Só envia mensagem se o estado do botão MUDOU
        if(estado_atual != estado_anterior) {
            
            // Se estado_atual for false (0), foi pressionado. Vamos enviar valor 1.
            // Se estado_atual for true (1), foi solto. Vamos enviar valor 0.
            if (estado_atual == false) {
                dados_env.valor = 1; 
            } else {
                dados_env.valor = 0;
            }

            // Envia para a fila do ADC (a mesma fila que junta os dados de X e Y)
            xQueueSend(xQueueADC, &dados_env, 0);
            
            estado_anterior = estado_atual;
            vTaskDelay(pdMS_TO_TICKS(50)); // Aquele debounce maroto para não dar duplo-clique
        }
        
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void joystick_bt_task(void *p){
    joystick_data data_receb;
    
    while(1){
        // Fica esperando chegar dados dos eixos X e Y
        if(xQueueReceive(xQueueADC, &data_receb, portMAX_DELAY)){
            
            // Só envia os dados do mouse se o Bluetooth estiver CONECTADO
            // (Para não encher a fila enquanto você ainda está pareando)
            // if (gpio_get(HC06_STATE_PIN) == 1) { 
                
                uint8_t sync = 0xFF;
                uint8_t eixo = data_receb.eixo;
                uint8_t lsb = data_receb.valor & 0xFF;
                uint8_t msb = (data_receb.valor >> 8) & 0xFF;

                // Envia os 4 bytes para a fila de transmissão do Bluetooth
                xQueueSend(xQueueTX, &sync, portMAX_DELAY);
                xQueueSend(xQueueTX, &eixo, portMAX_DELAY);
                xQueueSend(xQueueTX, &lsb, portMAX_DELAY);
                xQueueSend(xQueueTX, &msb, portMAX_DELAY);
            // }
        }
    }
}

void uart_rx_handler() {
        uint8_t ch = uart_getc(HC06_UART_ID);
        xQueueSendFromISR(xQueueRX, &ch, 0);
}

void init_uart_hc06(void) {
    uart_init(HC06_UART_ID, HC06_BAUD_RATE);

    // Set the TX and RX pins by using the function select on the GPIO
    // Set datasheet for more information on function select
    gpio_set_function(HC06_TX_PIN, UART_FUNCSEL_NUM(HC06_UART_ID, HC06_TX_PIN));
    gpio_set_function(HC06_RX_PIN, UART_FUNCSEL_NUM(HC06_UART_ID, HC06_RX_PIN));

    int __unused actual = uart_set_baudrate(HC06_UART_ID, HC06_BAUD_RATE);

    // Set UART flow control CTS/RTS, we don't want these, so turn them off
    uart_set_hw_flow(HC06_UART_ID, false, false);

    // Set our data format
    uart_set_format(HC06_UART_ID, 8, 1, UART_PARITY_NONE);
}

void init_uart_irq() {
    uart_set_fifo_enabled(HC06_UART_ID, false);

    int UART_IRQ = HC06_UART_ID == uart0 ? UART0_IRQ : UART1_IRQ;

    irq_set_exclusive_handler(UART_IRQ, uart_rx_handler);
    irq_set_enabled(UART_IRQ, true);

    uart_set_irq_enables(HC06_UART_ID, true, false);
}

static void led_status_task(void* p) {
    // 1. Configura o pino STATE como entrada
    gpio_init(HC06_STATE_PIN);
    gpio_set_dir(HC06_STATE_PIN, GPIO_IN);
    // Evita leitura flutuando quando o pino STATE não está dirigindo.
    #if HC06_STATE_PULL_UP
        gpio_pull_up(HC06_STATE_PIN);
    #else
        gpio_pull_down(HC06_STATE_PIN);
    #endif

    // Debounce / filtro: só muda o estado quando o nível ficar estável.
    bool conectado = false;
    bool last_raw = gpio_get(HC06_STATE_PIN);
    int stable = 0;

    gpio_init(LED_R_PIN);
    gpio_set_dir(LED_R_PIN, GPIO_OUT);
    gpio_put(LED_R_PIN, 1);

    gpio_init(LED_G_PIN);
    gpio_set_dir(LED_G_PIN, GPIO_OUT);
    gpio_put(LED_G_PIN, 1);

    gpio_set_function(LED_B_PIN, GPIO_FUNC_PWM);
    uint slice_num = pwm_gpio_to_slice_num(LED_B_PIN);
    uint chan = pwm_gpio_to_channel(LED_B_PIN);
    
    // Configura a resolução do PWM (de 0 a 255)
    pwm_set_wrap(slice_num, 255); 
    pwm_set_enabled(slice_num, true);

    int brilho = 0;
    int passo = 5; // O quão rápido a luz aumenta/diminui

    while (true) {
        // Método alternativo: considera "conectado" se houve tráfego RX recentemente.
        // (o terminal Python pode mandar heartbeat 0x00 para manter isso vivo)
        uint32_t now_ms = (uint32_t)to_ms_since_boot(get_absolute_time());
        bool conectado_sw = (g_hc06_last_rx_ms != 0) && ((now_ms - g_hc06_last_rx_ms) < 1200);

        bool raw = gpio_get(HC06_STATE_PIN);
        if (raw == last_raw) {
            if (stable < HC06_STATE_STABLE_SAMPLES) stable++;
        } else {
            last_raw = raw;
            stable = 0;
        }

        bool conectado_state = false;
        if (stable == HC06_STATE_STABLE_SAMPLES) {
        #if HC06_STATE_ACTIVE_HIGH
                    conectado_state = raw;
        #else
                    conectado_state = !raw;
        #endif
        }

        conectado = conectado_sw || conectado_state;

        if (conectado) {
            // Se conectou: Brilho no máximo e trava 
            pwm_set_chan_level(slice_num, chan, 0);
            vTaskDelay(pdMS_TO_TICKS(200)); // Não precisa rodar rápido quando está parado
        } else {
            // Se está aguardando: Efeito Fade [cite: 35]
            pwm_set_chan_level(slice_num, chan, brilho);
            
            brilho += passo;
            
            // Inverte a direção quando chega nos limites
            if (brilho >= 255) {
                brilho = 255;
                passo = -5; // Começa a apagar
            } else if (brilho <= 0) {
                brilho = 0;
                passo = 5;  // Começa a acender
            }
            
            // Um delay curto para a animação ficar fluida
            vTaskDelay(pdMS_TO_TICKS(20)); 
        }
    }
}

static void tx_task(void* p) {
    uint8_t ch;
    while (true) {
        if (xQueueReceive(xQueueTX, &ch, portMAX_DELAY) == pdTRUE) {
            uart_putc_raw(HC06_UART_ID, ch);
        }
    }
}

static void serial_task(void* p) {
    uint8_t ch;
    while (true) {
        int c = getchar_timeout_us(0);
        if (c != PICO_ERROR_TIMEOUT) {
            ch = (uint8_t)c;
            xQueueSend(xQueueTX, &ch, 0);
        }

        while (xQueueReceive(xQueueRX, &ch, 0) == pdTRUE) {
            g_hc06_last_rx_ms = (uint32_t)to_ms_since_boot(get_absolute_time());
            // Heartbeat: não imprime no terminal.
            if (ch == 0x00) continue;
            putchar_raw(ch);
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void oled_init(void) {

    i2c_init(i2c1, 400000);

    gpio_set_function(2, GPIO_FUNC_I2C);
    gpio_set_function(3, GPIO_FUNC_I2C);

    gpio_pull_up(2);
    gpio_pull_up(3);

    disp.external_vcc = false;

    // OLED 128x32
    ssd1306_init(&disp, 128, 32, 0x3C, i2c1);

    ssd1306_clear(&disp);
    ssd1306_show(&disp);
}

static void oled_btn_task(void* p) {
    oled_init();
    
    int pin_bluetooth = 0000; 
    bool estado_anterior_btn = true; 

    // Limpa a tela inicial
    ssd1306_clear(&disp);
    ssd1306_draw_string(&disp, 0, 0, 1, "Aperte B3 para PIN");
    ssd1306_show(&disp);

    while (true) {
        bool estado_atual_btn = gpio_get(BTN); // Lê o Botão B3
       
        // Se o botão foi apertado...
        if (estado_anterior_btn == true && estado_atual_btn == false) {
            
            // 1. Gera o novo PIN aleatório
            static bool semente_plantada = false;
            if (semente_plantada == false) {
                // time_us_32() pega o tempo em microssegundos desde que a Pico ligou
                srand(time_us_32()); 
                semente_plantada = true;
            }
            pin_bluetooth = 1000 + (rand() % 9000); 

            // 2. Avisa na tela que está configurando (opcional, mas fica legal!)
            ssd1306_clear(&disp);
            ssd1306_draw_string(&disp, 0, 12, 1, "Configurando...");
            ssd1306_show(&disp);

            // 3. Monta o comando AT para o HC-06
            char comando_at[30];
            sprintf(comando_at, "AT+PIN%04d", pin_bluetooth); 

            // 4. Envia o comando byte a byte
            for (int i = 0; i < strlen(comando_at); i++) {
                xQueueSend(xQueueTX, &comando_at[i], portMAX_DELAY);
            }

            // 5. AGUARDA A RESPOSTA "OK"
            char resposta[10];
            int idx = 0;
            uint8_t ch;
            // Lê da fila RX até receber a resposta (ou dar timeout)
            // O HC-06 costuma responder "OKsetPIN"
            while (xQueueReceive(xQueueRX, &ch, pdMS_TO_TICKS(1000)) == pdTRUE) {
                resposta[idx++] = ch;
                if (idx >= 2 && resposta[idx-2] == 'O' && resposta[idx-1] == 'K') {
                    break; // Achou o OK! Sai do loop.
                }
            }

            // 6. Atualiza a tela OLED com o PIN (apenas depois de receber a resposta)
            char texto_pin[20];
            sprintf(texto_pin, "Novo PIN: %d", pin_bluetooth);
            ssd1306_clear(&disp);
            ssd1306_draw_string(&disp, 0, 12, 1, texto_pin);
            ssd1306_show(&disp);
        }

        estado_anterior_btn = estado_atual_btn;
        vTaskDelay(pdMS_TO_TICKS(50)); // Debounce do botão
    }
}

int main(void) {
    stdio_init_all();
    adc_init();

    adc_gpio_init(JOYSTICK_X);
    adc_gpio_init(JOYSTICK_Y);
    adc_gpio_init(JOYSTICK_SW);

    gpio_init(BTN);
    gpio_set_dir(BTN, GPIO_IN);
    gpio_pull_up(BTN);

    init_uart_hc06();
    init_uart_irq();

    xQueueRX = xQueueCreate(256, sizeof(uint8_t));
    xQueueTX = xQueueCreate(256, sizeof(uint8_t));
    xQueueADC = xQueueCreate(10, sizeof(joystick_data));

    xTaskCreate(tx_task, "TX", 512, NULL, 2, NULL);
    xTaskCreate(serial_task, "Serial", 1024, NULL, 1, NULL);
    xTaskCreate(oled_btn_task, "OLED_BTN", 1024, NULL, 1, NULL);
    xTaskCreate(led_status_task, "LED_STATUS", 256, NULL, 1, NULL);
    xTaskCreate(x_task, "Eixo_X", 1024, NULL, 1, NULL);
    xTaskCreate(y_task, "Eixo_Y", 1024, NULL, 1, NULL);
    xTaskCreate(click_task, "Clique", 1024, NULL, 1, NULL);
    xTaskCreate(joystick_bt_task, "Joy_BT", 1024, NULL, 1, NULL);
    
    vTaskStartScheduler();

    while (true);
}
