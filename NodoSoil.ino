/**
 * @file soil_moisture_pump.ino
 * @brief Lectura de humedad de suelo y control de bomba con ESP-NOW y deep sleep en ESP32.
 *
 * Este código crea un nodo de sensor que mide la humedad del suelo,
 * transmite el valor a un "Nodo receptor" vía ESP-NOW y activa una bomba de agua
 * si la humedad desciende por debajo de un umbral configurado. Después entra en modo deep sleep
 * para ahorrar energía.
 *
 */

#include <WiFi.h>
#include <esp_now.h>
#include "esp_wifi.h"
#include "esp_sleep.h"

// —— Configuración de pines y constantes ——
/**
 * @brief Pin analógico conectado al sensor de humedad de suelo.
 * @details Se utiliza ADC1_CH6, mapeado al GPIO34 en ESP32.
 */
#define SOIL_PIN        34

/**
 * @brief Pin digital conectado al relé que controla la bomba.
 */
#define RELAY_PIN       23

/**
 * @brief Porcentaje de humedad mínimo antes de activar la bomba.
 * @details Si la humedad medida (%), calculada entre seco y mojado, es menor que este umbral,
 *          la bomba se enciende durante 2 segundos.
 */
#define UMBRAL_HUMEDAD  30

/**
 * @brief Lectura ADC correspondiente a suelo completamente seco.
 * @details Valor aproximado obtenido en calibración: 2590 (0% humedad).
 */
#define VALOR_SECO      2590

/**
 * @brief Lectura ADC correspondiente a suelo completamente mojado.
 * @details Valor aproximado obtenido en calibración: 1200 (100% humedad).
 */
#define VALOR_MOJADO    1200

/**
 * @brief Indicador de lógica activa del relé.
 * @details false = relé se activa escribiendo HIGH en el pin; true = se activa con LOW.
 */
#define RELAY_ACTIVO_LOW false

/**
 * @brief Dirección MAC del receptor ESP32 (Nodo principal) para ESP-NOW.
 */
uint8_t receiverMAC[] = {0x5C, 0x01, 0x3B, 0x72, 0xF2, 0xCC};

// —— Estructura de datos a enviar ——
/**
 * @brief Estructura para empaquetar y enviar valores de sensores.
 *
 * Por simplicidad sólo incluye humedad de suelo en este nodo.
 */
typedef struct {
  float soil; /**< Porcentaje de humedad de suelo calculado. */
} struct_message_send;

/**
 * @brief Variable global que almacena la estructura a enviar.
 */
struct_message_send sensorSend;

// —— Flag de envío completado ——
/**
 * @brief Variable volatile para indicar cuando el envío ESP-NOW ha terminado.
 * @details Se marca a true en la función callback OnDataSent().
 */
volatile bool envioCompleto = false;

// —— Callback de envío ——
/**
 * @brief Callback invocado tras intentar enviar un paquete ESP-NOW.
 * @param mac_addr Dirección MAC destino.
 * @param status   Resultado del envío (éxito o fallo).
 *
 * Muestra en el monitor serial si el paquete se envió correctamente
 * y actualiza la bandera envioCompleto.
 */
void OnDataSent(const uint8_t *mac_addr, esp_now_send_status_t status) {
  Serial.printf("📡 Envío ESP-NOW: %s\n", status == ESP_NOW_SEND_SUCCESS ? "OK" : "FALLÓ");
  envioCompleto = true;
}

// —— Tarea de lectura de humedad ——
/**
 * @brief Tarea FreeRTOS que lee el sensor de humedad, envía vía ESP-NOW,
 *        activa la bomba si es necesario y entra en deep sleep.
 * @param parameter Parámetros pasados a la tarea (no utilizado, NULL).
 *
 * 1) Lee valor raw del ADC y lo mapea a porcentaje 0–100%.
 * 2) Empaqueta y envía el porcentaje al nodo receptor.
 * 3) Espera confirmación de envío (timeout = 500 ms).
 * 4) Si humedad < UMBRAL_HUMEDAD, enciende la bomba 2 segundos.
 * 5) Apaga bomba y activa deep sleep por 30 s.
 */
void tareaHumedad(void *parameter) {
  // Lectura cruda del sensor
  int raw = analogRead(SOIL_PIN);
  // Mapeo lineal de valor ADC a porcentaje
  int pct = map(raw, VALOR_SECO, VALOR_MOJADO, 0, 100);
  pct = constrain(pct, 0, 100);
  Serial.printf("🌱 Humedad: RAW = %d → %d%%\n", raw, pct);

  // Preparar datos y enviar
  sensorSend.soil = pct;
  envioCompleto = false;
  esp_err_t res = esp_now_send(receiverMAC, (uint8_t *)&sensorSend, sizeof(sensorSend));
  if (res != ESP_OK) {
    Serial.printf("❌ Error al enviar ESP-NOW: %d\n", res);
  }

  // Espera activa con timeout de 500 ms
  unsigned long t0 = millis();
  while (!envioCompleto && millis() - t0 < 500) {
    vTaskDelay(pdMS_TO_TICKS(10));
  }

  // Control de bomba según humedad
  if (pct < UMBRAL_HUMEDAD) {
    Serial.println("💧 Suelo seco: Activando bomba...");
    // Nivel lógico según configuración de relé
    if (RELAY_ACTIVO_LOW)
      digitalWrite(RELAY_PIN, LOW);
    else
      digitalWrite(RELAY_PIN, HIGH);

    // Mantener bomba encendida 2 segundos
    vTaskDelay(pdMS_TO_TICKS(2000));

    // Apagar bomba
    if (RELAY_ACTIVO_LOW)
      digitalWrite(RELAY_PIN, HIGH);
    else
      digitalWrite(RELAY_PIN, LOW);

    Serial.println("🚿 Bomba apagada.");
  } else {
    Serial.println("✅ Suelo húmedo, no se activa bomba.");
  }

  // Prepararse para deep sleep
  Serial.println("😴 Entrando en deep sleep...");
  vTaskDelay(pdMS_TO_TICKS(100));
  esp_deep_sleep_start();
}

/**
 * @brief Función setup principal de Arduino.
 *
 * Inicializa Serial, configura pines, modo WiFi y ESP-NOW,
 * registra callback de envío y crea la tarea de humedad.
 */
void setup() {
  Serial.begin(115200);
  delay(100);

  // Configuración de GPIO para relé
  pinMode(RELAY_PIN, OUTPUT);
  // Asegurar estado de apagado lógico
  if (RELAY_ACTIVO_LOW)
    digitalWrite(RELAY_PIN, HIGH);
  else
    digitalWrite(RELAY_PIN, LOW);

  // Programar timer para deep sleep de 30 segundos
  esp_sleep_enable_timer_wakeup(30 * 1000000ULL);

  // Modo WiFi estación y fijar canal para ESP-NOW
  WiFi.mode(WIFI_STA);
  esp_wifi_set_channel(6, WIFI_SECOND_CHAN_NONE);

  // Inicializar ESP-NOW
  if (esp_now_init() != ESP_OK) {
    Serial.println("❌ Error al iniciar ESP-NOW");
    return;
  }

  // Registrar callback de envío
  esp_now_register_send_cb(OnDataSent);

  // Añadir peer (nodo receptor)
  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, receiverMAC, 6);
  peerInfo.channel = 6;
  peerInfo.encrypt = false;
  if (esp_now_add_peer(&peerInfo) != ESP_OK) {
    Serial.println("❌ Error al agregar receptor");
    return;
  }

  // Crear tarea libreRTOS para gestión de humedad
  xTaskCreatePinnedToCore(
    tareaHumedad,
    "TareaHumedad",
    4096,
    NULL,
    1,
    NULL,
    1
  );
}

/**
 * @brief Función loop de Arduino.
 * @note Vacía: toda la lógica se ejecuta en "tareaHumedad" y deep sleep.
 */
void loop() {
  // No se usa, queda en standby
}