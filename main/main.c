#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "mqtt_client.h"
#include "simple_wifi_sta.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include <string.h>

static const char* TAG = "main";
static const int RX_BUF_SIZE = 1024;

#define TXD_PIN (GPIO_NUM_17)
#define RXD_PIN (GPIO_NUM_18)
#define MQTT_ADDRESS    "mqtt://47.99.95.191"     //MQTT连接地址
#define MQTT_PORT       1883                        //MQTT连接端口号
#define MQTT_CLIENT     "mqttx_test"              //Client ID（设备唯一，大家最好自行改一下）
#define MQTT_USERNAME   "####"                     //MQTT用户名
#define MQTT_PASSWORD   
"####"                  //MQTT密码

#define MQTT_PUBLIC_TOPIC      "/test/topic1"       //测试用的,推送消息主题
#define MQTT_SUBSCRIBE_TOPIC    "/test/topic2"      //测试用的,需要订阅的主题

//定义一个事件组，用于通知main函数WIFI连接成功
#define WIFI_CONNECT_BIT     BIT0
static EventGroupHandle_t   s_wifi_ev = NULL;

//MQTT客户端操作句柄
static esp_mqtt_client_handle_t     s_mqtt_client = NULL;

//MQTT连接标志
static bool   s_is_mqtt_connected = false;

//存储接收到的 MQTT 消息内容
static char* received_mqtt_data = NULL;  // 使用指针代替数组

static bool is_new_data = false;  // 标志位，指示是否有新数据需要发送

/**
 * mqtt连接事件处理函数
 * @param event 事件参数
 * @return 无
 */
static void aliot_mqtt_event_handler(void* event_handler_arg,
                                        esp_event_base_t event_base,
                                        int32_t event_id,
                                        void* event_data)
{
    esp_mqtt_event_handle_t event = event_data;
    //esp_mqtt_client_handle_t client = event->client;

    // your_context_t *context = event->context;
    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED:  //连接成功
            ESP_LOGI(TAG, "mqtt connected");
            s_is_mqtt_connected = true;
            //连接成功后，订阅测试主题
            esp_mqtt_client_subscribe(s_mqtt_client, MQTT_SUBSCRIBE_TOPIC, 1);
            break;
        case MQTT_EVENT_DISCONNECTED:   //连接断开
            ESP_LOGI(TAG, "mqtt disconnected");
            s_is_mqtt_connected = false;
            break;
        case MQTT_EVENT_SUBSCRIBED:     //收到订阅消息ACK
            ESP_LOGI(TAG, " mqtt subscribed ack, msg_id=%d", event->msg_id);
            break;
        case MQTT_EVENT_UNSUBSCRIBED:   //收到解订阅消息ACK
            break;
        case MQTT_EVENT_PUBLISHED:      //收到发布消息ACK
            ESP_LOGI(TAG, "mqtt publish ack, msg_id=%d", event->msg_id);
            break;
        case MQTT_EVENT_DATA:           //收到订阅消息
            ESP_LOGI(TAG, "Received MQTT message on topic %.*s: %.*s", event->topic_len, event->topic, event->data_len, event->data);
            // 将接收到的 MQTT 消息内容复制到全局变量中
            strncpy(received_mqtt_data, event->data, event->data_len);
            received_mqtt_data[event->data_len] = '\0';  // 添加字符串终止符
            
            is_new_data = true;  // 标志位设为真，表示有新数据需要发送
            break;
        case MQTT_EVENT_ERROR:
            ESP_LOGI(TAG, "MQTT_EVENT_ERROR");
            break;
        default:
            break;
    }
}

/** 启动mqtt连接
 * @param 无
 * @return 无
*/
void mqtt_start(void)
{
    esp_mqtt_client_config_t mqtt_cfg = {0};
    mqtt_cfg.broker.address.uri = MQTT_ADDRESS;
    mqtt_cfg.broker.address.port = MQTT_PORT;
    //Client ID
    mqtt_cfg.credentials.client_id = MQTT_CLIENT;
    //用户名
    mqtt_cfg.credentials.username = MQTT_USERNAME;
    //密码
    mqtt_cfg.credentials.authentication.password = MQTT_PASSWORD;
    ESP_LOGI(TAG,"mqtt connect->clientId:%s,username:%s,password:%s",mqtt_cfg.credentials.client_id,
    mqtt_cfg.credentials.username,mqtt_cfg.credentials.authentication.password);
    //设置mqtt配置，返回mqtt操作句柄
    s_mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    //注册mqtt事件回调函数
    esp_mqtt_client_register_event(s_mqtt_client, ESP_EVENT_ANY_ID, aliot_mqtt_event_handler, s_mqtt_client);
    //启动mqtt连接
    esp_mqtt_client_start(s_mqtt_client);
}

/** wifi事件通知
 * @param 无
 * @return 无
*/
void wifi_event_handler(WIFI_EV_e ev)
{
    if(ev == WIFI_CONNECTED)
    {
        xEventGroupSetBits(s_wifi_ev,WIFI_CONNECT_BIT);
    }
}

//初始化串口
void init(void)
{
    const uart_config_t uart_config = {
        .baud_rate = 9600,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    uart_driver_install(UART_NUM_1, RX_BUF_SIZE * 2, 0, 0, NULL, 0);
    uart_param_config(UART_NUM_1, &uart_config);
    uart_set_pin(UART_NUM_1, TXD_PIN, RXD_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
}

//串口发送函数
int sendData(const char* logName, const uint8_t* data, size_t len)
{
    const int txBytes = uart_write_bytes(UART_NUM_1, (const char*)data, len);
    ESP_LOGI(logName, "Wrote %d bytes", txBytes);
    return txBytes;
}

// 将字符串格式的 MQTT 数据转换为字节数组
void parse_mqtt_message_to_bytes(const char* mqtt_data, uint8_t* byte_array, size_t* byte_array_len) {
    const char* ptr = mqtt_data;
    int index = 0;
    
    while (*ptr != '\0') {
        if (*ptr == ' ' || *ptr == '\n' || *ptr == '\r') {
            ptr++;  // 跳过空格和换行符
            continue;
        }

        sscanf(ptr, "%02hhx", &byte_array[index]);
        index++;
        ptr += 2;  // 跳过两位十六进制数字
    }
    
    *byte_array_len = index;
}

//串口发送任务
static void tx_task(void *arg) {
    static const char *TX_TASK_TAG = "TX_TASK";
    esp_log_level_set(TX_TASK_TAG, ESP_LOG_INFO);

    uint8_t byte_array[RX_BUF_SIZE];
    size_t byte_array_len = 0;

    while (1) {
        // 检查标志位，只有在接收到新数据时才发送
        if (is_new_data) {
            parse_mqtt_message_to_bytes(received_mqtt_data, byte_array, &byte_array_len);
            
            if (byte_array_len > 0) {
                const int txBytes = uart_write_bytes(UART_NUM_1, (const char*)byte_array, byte_array_len);
                ESP_LOGI(TX_TASK_TAG, "Wrote %d bytes", txBytes);
            }

            is_new_data = false;  // 发送完后，重置标志位
        }
        
        vTaskDelay(200 / portTICK_PERIOD_MS);  // 适当延迟，避免任务一直占用 CPU
    }
}


// 串口接收任务
static void rx_task(void *arg)
{
    static const char *RX_TASK_TAG = "RX_TASK";
    esp_log_level_set(RX_TASK_TAG, ESP_LOG_INFO);
    uint8_t* data = (uint8_t*) malloc(RX_BUF_SIZE + 1);
    // 每个字节转换为2个字符，并在每个字节后加一个空格，还需包含字符串终止符空间
    char* hex_str = (char*) malloc((RX_BUF_SIZE * 3) + 1); 
    while (1) {
        const int rxBytes = uart_read_bytes(UART_NUM_1, data, RX_BUF_SIZE, 1000 / portTICK_PERIOD_MS);
        if (rxBytes > 0) {  // 确保接收到数据
            int hex_str_index = 0;
            for (int i = 0; i < rxBytes; i++) {  // 每个字节转换为2个字符
                sprintf(hex_str + hex_str_index, "%02x ", data[i]);
                hex_str_index += 3;  // 每个字节占用2个字符加1个空格
            }
            hex_str[hex_str_index - 1] = 0; // 替换最后一个空格为字符串终止符
            
            ESP_LOGI(RX_TASK_TAG, "Read %d bytes", rxBytes);
            
            // 通过 MQTT 发送字符串形式的数据
            if (s_is_mqtt_connected) {
                esp_mqtt_client_publish(s_mqtt_client, MQTT_PUBLIC_TOPIC, hex_str, hex_str_index - 1, 1, 0);
            }

            // 打印转换后的十六进制字符串
            printf("Hex String: %s\n", hex_str);
        }
    }
    free(data);
    free(hex_str);
}


void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        //NVS出现错误，执行擦除
        ESP_ERROR_CHECK(nvs_flash_erase());
        //重新尝试初始化
        ESP_ERROR_CHECK(nvs_flash_init());
    }
    s_wifi_ev = xEventGroupCreate();
    EventBits_t ev = 0;

    // 为 received_mqtt_data 分配内存
    received_mqtt_data = (char*)malloc(RX_BUF_SIZE);
    if (received_mqtt_data == NULL) {
        ESP_LOGE(TAG, "Failed to allocate memory for received_mqtt_data");
        return;
    }
    memset(received_mqtt_data, 0, RX_BUF_SIZE);  // 初始化内存

    //初始化WIFI，传入回调函数，用于通知连接成功事件
    wifi_sta_init(wifi_event_handler);

    //一直监听WIFI连接事件，直到WiFi连接成功后，才启动MQTT连接
    ev = xEventGroupWaitBits(s_wifi_ev,WIFI_CONNECT_BIT,pdTRUE,pdFALSE,portMAX_DELAY);
    if(ev & WIFI_CONNECT_BIT)
    {
        mqtt_start();
    }

    init();
    xTaskCreate(rx_task, "uart_rx_task", 1024 * 2, NULL, configMAX_PRIORITIES - 1, NULL);
    xTaskCreate(tx_task, "uart_tx_task", 1024 * 2, NULL, configMAX_PRIORITIES - 1, NULL);

    // 程序结束时，释放分配的内存（根据你的程序逻辑可能需要在不同的位置释放）
    // free(received_mqtt_data);
}
