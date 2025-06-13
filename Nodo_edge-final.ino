#include <WiFi.h>
#include <esp_now.h>
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>
#include <time.h>
#include "esp_wifi.h"
#include "SD_MMC.h"

// Configuración WiFi
const char* ssid     = "nombrered";
const char* password = "claverred";

// Configuración Telegram
#define BOTtoken  "XXXXXXXXX:AAXXXXXX-4618XXX15XX"
#define CHAT_ID   "XXXXXXXXXXXX"
WiFiClientSecure client;
UniversalTelegramBot bot(BOTtoken, client);

// Configuración NTP
const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = -5 * 3600;  // UTC-5 para Colombia
const int daylightOffset_sec = 0;

// Umbrales
#define TEMP_THRESHOLD      30.0
#define HUM_THRESHOLD       80.0
#define SOIL_THRESHOLD_LOW  30.0
#define SOIL_THRESHOLD_HIGH 70.0
#define LDR_THRESHOLD       700.0
#define MQ5_THRESHOLD       1500.0

// MACs de los nodos
uint8_t unifiedSenderMAC[] = {0x08, 0xD1, 0xF9, 0xEE, 0x4F, 0x2C};
uint8_t soilSenderMAC[] = {0xA0, 0xB7, 0x65, 0x1B, 0x18, 0x50};

// Estructuras de datos
typedef struct {
    float temp;
    float hum;
    int luz;
    int mq5;
} dht_message_t;

typedef struct {
    float soil;
} soil_message_t;

// Colas
QueueHandle_t dhtQueue;
QueueHandle_t soilQueue;
QueueHandle_t msgQueue;

// ——— Función para guardar datos en la SD ———
void saveDataToSD(float temp, float hum, float soil, int luz, int mq5) {
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo)) {
        Serial.println("❌ Error al obtener la hora");
        return;
    }

    char datePath[15];   // "/YYYY-MM-DD"
    char hourPath[18];   // "/YYYY-MM-DD/HH"
    char filePath[25];   // "/YYYY-MM-DD/HH/data.txt"

    strftime(datePath, sizeof(datePath), "/%Y-%m-%d", &timeinfo);
    strftime(hourPath, sizeof(hourPath), "/%Y-%m-%d/%H", &timeinfo);
    strftime(filePath, sizeof(filePath), "/%Y-%m-%d/%H/data.txt", &timeinfo);

    if (!SD_MMC.exists(datePath)) SD_MMC.mkdir(datePath);
    if (!SD_MMC.exists(hourPath)) SD_MMC.mkdir(hourPath);

    File file = SD_MMC.open(filePath, FILE_APPEND);
    if (file) {
        file.printf("Temp: %.1f°C, Hum: %.1f%%, Suelo: %.1f%%, Luz: %d, MQ5: %d\n",
                    temp, hum, soil, luz, mq5);
        file.close();
        Serial.println("✅ Datos guardados en " + String(filePath));
    } else {
        Serial.println("❌ Error al abrir el archivo");
    }
}

// ——— Recepción ESP-NOW ———
void OnDataRecv(const esp_now_recv_info_t *info, const uint8_t *incoming, int len) {
    if (memcmp(info->src_addr, soilSenderMAC, 6) == 0 && len == sizeof(soil_message_t)) {
        soil_message_t s;
        memcpy(&s, incoming, len);
        xQueueSend(soilQueue, &s, portMAX_DELAY);
        saveDataToSD(0, 0, s.soil, 0, 0);
    } else if (memcmp(info->src_addr, unifiedSenderMAC, 6) == 0 && len == sizeof(dht_message_t)) {
        dht_message_t d;
        memcpy(&d, incoming, len);
        xQueueSend(dhtQueue, &d, portMAX_DELAY);
        saveDataToSD(d.temp, d.hum, 0, d.luz, d.mq5);
    }
}

// ——— Tareas ———
void DHTTask(void *parameter) {
    dht_message_t d;
    for (;;) {
        if (xQueueReceive(dhtQueue, &d, portMAX_DELAY)) {
            String msg = "";
            Serial.printf("🌡 Temp=%.1f°C 💧 Hum=%.1f%% 🔆 Luz=%d 🔥 MQ5=%d\n", d.temp, d.hum, d.luz, d.mq5);

            if (d.temp > TEMP_THRESHOLD) msg += "⚠ Alta temperatura: " + String(d.temp, 1) + "°C\n";
            if (d.hum > HUM_THRESHOLD) msg += "💧 Alta humedad: " + String(d.hum, 1) + "%\n";
            if (d.luz > LDR_THRESHOLD) msg += "🌑 Luz baja: " + String(d.luz) + "\n";
            if (d.mq5 > MQ5_THRESHOLD) msg += "🔥 Posible gas detectado: " + String(d.mq5) + "\n";

            if (msg.length()) {
                char* buf = (char*)malloc(msg.length() + 1);
                msg.toCharArray(buf, msg.length() + 1);
                xQueueSend(msgQueue, &buf, portMAX_DELAY);
            }
        }
    }
}

void SoilTask(void *parameter) {
    soil_message_t s;
    for (;;) {
        if (xQueueReceive(soilQueue, &s, portMAX_DELAY)) {
            String msg = "";
            Serial.printf("🌱 Humedad del suelo: %.1f%%\n", s.soil);

            if (s.soil < SOIL_THRESHOLD_LOW)
                msg += "🚨 Suelo muy seco, Bomba encendida: " + String(s.soil, 1) + "%\n";
            else if (s.soil > SOIL_THRESHOLD_HIGH)
                msg += "🌊 Suelo demasiado húmedo: " + String(s.soil, 1) + "%\n";

            if (msg.length()) {
                char* buf = (char*)malloc(msg.length() + 1);
                msg.toCharArray(buf, msg.length() + 1);
                xQueueSend(msgQueue, &buf, portMAX_DELAY);
            }
        }
    }
}

void TelegramTask(void *parameter) {
    char* recvBuffer;
    for (;;) {
        if (xQueueReceive(msgQueue, &recvBuffer, portMAX_DELAY)) {
            bool ok = bot.sendMessage(CHAT_ID, String(recvBuffer), "");
            Serial.println(ok ? "✅ Telegram enviado" : "❌ Error en Telegram");
            free(recvBuffer);
        }
    }
}

// ——— Setup ———
void setup() {
    Serial.begin(115200);
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, password);

    while (WiFi.status() != WL_CONNECTED) delay(500);
    client.setInsecure();
    bot.sendMessage(CHAT_ID, "🤖 Bot receptor iniciado", "");

    // NTP
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo)) {
        Serial.println("❌ Error al obtener la hora del servidor NTP");
        bot.sendMessage(CHAT_ID, "❌ Error al obtener la hora desde el servidor NTP", "");
    } else {
        Serial.printf("⏳ Hora obtenida: %02d:%02d:%02d\n", timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
        bot.sendMessage(CHAT_ID, "⏳ Hora NTP obtenida correctamente", "");
    }

    // SD
    if (!SD_MMC.begin()) {
        Serial.println("❌ Error al montar SD_MMC");
        bot.sendMessage(CHAT_ID, "❌ Error al montar la SD_MMC", "");
    } else {
        Serial.println("✅ SD_MMC montada correctamente");
        bot.sendMessage(CHAT_ID, "✅ SD_MMC montada correctamente", "");
    }

    // Colas y tareas
    dhtQueue = xQueueCreate(5, sizeof(dht_message_t));
    soilQueue = xQueueCreate(5, sizeof(soil_message_t));
    msgQueue = xQueueCreate(5, sizeof(char*));

    xTaskCreate(DHTTask, "DHTTask", 4096, NULL, 1, NULL);
    xTaskCreate(SoilTask, "SoilTask", 4096, NULL, 1, NULL);
    xTaskCreate(TelegramTask, "TelegramTask", 8192, NULL, 1, NULL);

    // ESP-NOW
    if (esp_now_init() != ESP_OK) {
        Serial.println("❌ Error al iniciar ESP-NOW");
        return;
    }
    esp_now_register_recv_cb(OnDataRecv);

    esp_now_peer_info_t peer = {};
    peer.channel = WiFi.channel();
    peer.encrypt = false;

    memcpy(peer.peer_addr, unifiedSenderMAC, 6);
    esp_now_add_peer(&peer);

    memcpy(peer.peer_addr, soilSenderMAC, 6);
    esp_now_add_peer(&peer);
}

// ——— Loop ———
void loop() {
    vTaskDelay(portMAX_DELAY);
}
