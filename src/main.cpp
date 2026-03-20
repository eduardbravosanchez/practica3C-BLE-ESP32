// =============================================================
//  ESP32-S3-DevKitM-1 – Servidor BLE con NOTIFY ADC + LED RGB + Estado JSON
//  Servicio UUID   : 4fafc201-1fb5-459e-8fcc-c5c9c331914b
//  Char ADC        (READ + NOTIFY) : beb5483e-36e1-4688-b7f5-ea07361b26a8
//  Char LED        (WRITE)         : 1c95d5e3-d8f7-413a-bf3d-7a2e5d7be87e
//  Char Estado JSON(READ)          : a9b4f3c2-7e6d-4b5a-8c1d-2f3e4a5b6c7d
//
//  Char Estado JSON devuelve al leerla:
//    {"adc": 1023, "led": true, "uptime": 42}
//
//  platformio.ini necesario:
//    monitor_speed = 115200
//    lib_deps = adafruit/Adafruit NeoPixel @ ^1.12.3
// =============================================================

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <Adafruit_NeoPixel.h>

// ------------------------------------------------------------------
// UUIDs
// ------------------------------------------------------------------
#define SERVICE_UUID      "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define CHAR_ADC_UUID     "beb5483e-36e1-4688-b7f5-ea07361b26a8"
#define CHAR_LED_UUID     "1c95d5e3-d8f7-413a-bf3d-7a2e5d7be87e"
#define CHAR_STATUS_UUID  "a9b4f3c2-7e6d-4b5a-8c1d-2f3e4a5b6c7d"  // Nueva característica JSON

// ------------------------------------------------------------------
// PINES
// ------------------------------------------------------------------
#define PIN_ADC   1    // GPIO1 = ADC1_CH0, compatible con BLE activo
#define PIN_LED   48   // GPIO48 = LED RGB WS2812 integrado en DevKitM-1

// ------------------------------------------------------------------
// NEOPIXEL
// ------------------------------------------------------------------
#define NUM_LEDS  1
Adafruit_NeoPixel led(NUM_LEDS, PIN_LED, NEO_GRB + NEO_KHZ800);

// ------------------------------------------------------------------
// VARIABLES GLOBALES
// ------------------------------------------------------------------
BLECharacteristic* pCharADC    = nullptr;
BLECharacteristic* pCharLED    = nullptr;
BLECharacteristic* pCharStatus = nullptr;  // Puntero a la nueva característica JSON

bool clienteConectado = false;
bool clienteAnterior  = false;

// Estado del LED: variable global compartida entre callbacks y la char JSON.
// Se actualiza en onWrite del LED y se consulta en onRead del JSON.
bool ledEncendido = false;

// Temporizador para notificaciones ADC
unsigned long ultimaNotificacion = 0;
const unsigned long INTERVALO_MS = 1000;


// ------------------------------------------------------------------
// FUNCIONES AUXILIARES LED
// ------------------------------------------------------------------
void ledEncender() {
  led.setPixelColor(0, led.Color(255, 255, 255));
  led.show();
  ledEncendido = true;   // Actualizar estado global
}

void ledApagar() {
  led.setPixelColor(0, led.Color(0, 0, 0));
  led.show();
  ledEncendido = false;  // Actualizar estado global
}


// ==================================================================
//  CALLBACKS DEL SERVIDOR  –  onConnect / onDisconnect
// ==================================================================
class ServidorCallbacks : public BLEServerCallbacks {

  void onConnect(BLEServer* pServer) override {
    clienteConectado = true;
    Serial.println(">>> Cliente BLE CONECTADO");
  }

  void onDisconnect(BLEServer* pServer) override {
    clienteConectado = false;
    Serial.println("<<< Cliente BLE DESCONECTADO");
    BLEDevice::startAdvertising();
    Serial.println("    Advertising reiniciado – esperando nuevo cliente...\n");
  }
};


// ==================================================================
//  CALLBACKS DE LA CARACTERÍSTICA ADC  –  onRead (lectura manual)
//  Las notificaciones automáticas se gestionan en el loop()
// ==================================================================
class ADCCallbacks : public BLECharacteristicCallbacks {

  void onRead(BLECharacteristic* pCharacteristic) override {
    int valorADC = analogRead(PIN_ADC);
    pCharacteristic->setValue(String(valorADC).c_str());

    Serial.print("    [ADC] Lectura manual: ");
    Serial.println(valorADC);
  }
};


// ==================================================================
//  CALLBACKS DE LA CARACTERÍSTICA LED  –  onWrite
//  0x01 / '1' → encender   |   0x00 / '0' → apagar
// ==================================================================
class LEDCallbacks : public BLECharacteristicCallbacks {

  void onWrite(BLECharacteristic* pCharacteristic) override {
    std::string valor = pCharacteristic->getValue();

    if (valor.length() > 0) {
      uint8_t comando = (uint8_t)valor[0];

      if (comando == 0x01 || comando == '1') {
        ledEncender();   // También pone ledEncendido = true
        Serial.println("    [LED] Encendido (blanco)");
      }
      else if (comando == 0x00 || comando == '0') {
        ledApagar();     // También pone ledEncendido = false
        Serial.println("    [LED] Apagado");
      }
      else {
        Serial.print("    [LED] Byte desconocido: 0x");
        Serial.println(comando, HEX);
      }
    }
  }
};


// ==================================================================
//  CALLBACKS DE LA CARACTERÍSTICA ESTADO JSON  –  onRead
//
//  Cada vez que el cliente lee esta característica, se construye
//  un JSON fresco con los tres campos:
//
//    adc     → lectura actual del ADC (0–4095)
//    led     → estado actual del LED (true / false)
//    uptime  → segundos transcurridos desde el arranque del ESP32
//
//  Se construye con String para evitar dependencias externas (no
//  necesita ArduinoJson ni ninguna librería adicional).
// ==================================================================
class StatusCallbacks : public BLECharacteristicCallbacks {

  void onRead(BLECharacteristic* pCharacteristic) override {

    // Leer ADC en el momento exacto de la petición
    int    valorADC = analogRead(PIN_ADC);

    // millis() devuelve ms desde el arranque; dividimos entre 1000 para segundos
    unsigned long uptime = millis() / 1000;

    // Construir el JSON como String
    // ledEncendido es true/false → lo mapeamos a "true"/"false" de texto JSON
    String json = "{\"adc\": ";
    json += valorADC;
    json += ", \"led\": ";
    json += ledEncendido ? "true" : "false";
    json += ", \"uptime\": ";
    json += uptime;
    json += "}";

    // Actualizar el valor de la característica con el JSON generado
    pCharacteristic->setValue(json.c_str());

    Serial.print("    [JSON] Estado enviado: ");
    Serial.println(json);
  }
};


// ==================================================================
//  SETUP
// ==================================================================
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== ESP32-S3 BLE Server – ADC NOTIFY + LED + JSON ===");

  // --- NeoPixel ---
  led.begin();
  ledApagar();
  for (int i = 0; i < 3; i++) {
    ledEncender(); delay(200);
    ledApagar();   delay(200);
  }
  Serial.println("[LED] NeoPixel listo en GPIO48");

  // --- BLE ---
  BLEDevice::init("ESP32-S3_BLE");

  BLEServer* pServer = BLEDevice::createServer();
  pServer->setCallbacks(new ServidorCallbacks());

  // IMPORTANTE: el número de características afecta al tamaño de la tabla
  // interna del servicio. createService(UUID, numHandles) reserva espacio.
  // Por defecto son 15 handles; con 3 características (una con BLE2902)
  // necesitamos al menos 10. El valor 15 es suficiente y seguro.
  BLEService* pService = pServer->createService(BLEUUID(SERVICE_UUID), 15);

  // ----------------------------------------------------------------
  //  CARACTERÍSTICA 1: ADC  (READ + NOTIFY + BLE2902)
  // ----------------------------------------------------------------
  pCharADC = pService->createCharacteristic(
    CHAR_ADC_UUID,
    BLECharacteristic::PROPERTY_READ |
    BLECharacteristic::PROPERTY_NOTIFY
  );
  pCharADC->setCallbacks(new ADCCallbacks());
  pCharADC->setValue("0");
  pCharADC->addDescriptor(new BLE2902());  // CCCD obligatorio para NOTIFY
  Serial.println("[BLE] Char ADC        (READ + NOTIFY) creada");

  // ----------------------------------------------------------------
  //  CARACTERÍSTICA 2: LED  (WRITE)
  // ----------------------------------------------------------------
  pCharLED = pService->createCharacteristic(
    CHAR_LED_UUID,
    BLECharacteristic::PROPERTY_WRITE
  );
  pCharLED->setCallbacks(new LEDCallbacks());
  Serial.println("[BLE] Char LED        (WRITE)         creada");

  // ----------------------------------------------------------------
  //  CARACTERÍSTICA 3: Estado JSON  (READ)
  //  El cliente lee esta característica para obtener un snapshot
  //  completo del estado del dispositivo en formato JSON.
  //  No tiene NOTIFY: el cliente solicita la lectura cuando la necesita.
  // ----------------------------------------------------------------
  pCharStatus = pService->createCharacteristic(
    CHAR_STATUS_UUID,
    BLECharacteristic::PROPERTY_READ
  );
  pCharStatus->setCallbacks(new StatusCallbacks());
  pCharStatus->setValue("{\"adc\":0,\"led\":false,\"uptime\":0}");  // Valor inicial
  Serial.println("[BLE] Char Estado JSON(READ)          creada");

  pService->start();

  BLEAdvertising* pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->setScanResponse(true);
  pAdvertising->setMinPreferred(0x06);
  pAdvertising->setMinPreferred(0x12);
  BLEDevice::startAdvertising();

  Serial.println("[BLE] Advertising iniciado – esperando cliente...\n");
}


// ==================================================================
//  LOOP  –  Notificaciones ADC periódicas con millis()
// ==================================================================
void loop() {

  // Gestión de desconexión
  if (clienteAnterior && !clienteConectado) {
    ledApagar();
    clienteAnterior = false;
  }
  if (!clienteAnterior && clienteConectado) {
    clienteAnterior = true;
  }

  // Notificación automática ADC cada INTERVALO_MS
  if (clienteConectado) {
    unsigned long ahora = millis();

    if (ahora - ultimaNotificacion >= INTERVALO_MS) {
      ultimaNotificacion = ahora;

      int valorADC = analogRead(PIN_ADC);
      pCharADC->setValue(String(valorADC).c_str());
      pCharADC->notify();

      Serial.print("    [NOTIFY] ADC: ");
      Serial.println(valorADC);
    }
  }

  delay(20);
}
