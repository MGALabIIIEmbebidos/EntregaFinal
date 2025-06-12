#include <esp_now.h>
#include <WiFi.h>
#include <DHT.h>
#include "esp_wifi.h"

#define DHTPIN 5
#define DHTTYPE DHT22
#define MQ5_PIN 34
#define LDR_PIN 32

DHT dht(DHTPIN, DHTTYPE);

// MAC del receptor
uint8_t receiverMAC[] = {0x5c, 0x01, 0x3b, 0x72, 0xf2, 0xcc};

// Estructura de datos
typedef struct {
  float temp;
  float hum;
  int luz;
  int mq5;
} struct_message_send;

struct_message_send sensorSend;

// Callback de envío
void OnDataSent(const uint8_t *mac_addr, esp_now_send_status_t status) {
  Serial.printf("Envío ESP-NOW: %s\n", status == ESP_NOW_SEND_SUCCESS ? "OK" : "FALLÓ");
}

void setup() {
  Serial.begin(115200);
  delay(100);  // Tiempo para que se estabilice el serial

  dht.begin();

  WiFi.mode(WIFI_STA);
  Serial.print("Transmisor MAC STA: ");
  Serial.println(WiFi.macAddress());

  const uint8_t canal = 6;
  esp_wifi_set_channel(canal, WIFI_SECOND_CHAN_NONE);
  Serial.printf("Transmisor fuerza canal: %d\n", canal);

  if (esp_now_init() != ESP_OK) {
    Serial.println("Error al iniciar ESP-NOW");
    // No seguimos si falla
    while (true) {
      delay(1000);
    }
  }
  esp_now_register_send_cb(OnDataSent);

  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, receiverMAC, 6);
  peerInfo.channel = canal;
  peerInfo.encrypt = false;

  if (esp_now_add_peer(&peerInfo) != ESP_OK) {
    Serial.println("Fallo al agregar peer");
    while (true) {
      delay(1000);
    }
  }

  // Leer sensores
  float t = dht.readTemperature();
  float h = dht.readHumidity();
  int luz = analogRead(LDR_PIN);
  int mq5 = analogRead(MQ5_PIN);

  if (isnan(t) || isnan(h)) {
    Serial.println("Error al leer DHT22");
  } else {
    sensorSend.temp = t;
    sensorSend.hum = h;
    sensorSend.luz = luz;
    sensorSend.mq5 = mq5;

    Serial.printf("Temp: %.1f °C | Hum: %.1f %% | Luz: %d | MQ-5: %d\n", t, h, luz, mq5);

    esp_err_t res = esp_now_send(receiverMAC, (uint8_t *)&sensorSend, sizeof(sensorSend));
    if (res != ESP_OK) {
      Serial.println("Error al enviar datos");
    }
  }

  // ESP-NOW tarda un momento en enviar
  delay(100);

  Serial.println("Entrando en deep sleep por 30 segundos...");

  esp_deep_sleep(30* 1000000); // 10 segundos en microsegundos
}

void loop() {
  // No se ejecuta porque estamos usando deep sleep y reset para ciclo
}
