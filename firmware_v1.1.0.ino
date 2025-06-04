#include <WiFi.h>
#include <HTTPClient.h>
#include <FS.h>
#include <SPIFFS.h>
#include <Update.h>
#include "ArduinoJson.h"
#include "esp_sleep.h"
#include "driver/gpio.h"

#define REED_PIN GPIO_NUM_2
#define CURRENT_VERSION "1.0.0"  // Versión actual del firmware
const char* updateJsonURL = "https://tuusuario.github.io/mi_proyecto_ota/firmware.json";
const char* serverUrl = "https://76df-189-153-216-92.ngrok-free.app/api/track";

RTC_DATA_ATTR char lastStatus = 'X';

struct Evento {
  unsigned long timestamp;
  char status;
};

struct WiFiNetwork {
  String ssid;
  String password;
  String type;
};

// --- FUNCIONES OTA ---
void checkAndPerformOTA() {
  WiFiClient client;
  HTTPClient http;

  Serial.println("Verificando si hay nueva versión disponible...");

  http.begin(client, updateJsonURL);
  int httpCode = http.GET();

  if (httpCode != 200) {
    Serial.printf("No se pudo obtener firmware.json. Código HTTP: %d\n", httpCode);
    http.end();
    return;
  }

  String jsonStr = http.getString();
  http.end();

  StaticJsonDocument<512> doc;
  DeserializationError err = deserializeJson(doc, jsonStr);
  if (err) {
    Serial.println("Error al parsear firmware.json");
    return;
  }

  String newVersion = doc["version"];
  String binUrl = doc["bin_url"];

  if (newVersion == CURRENT_VERSION) {
    Serial.println("Ya se tiene la versión más reciente.");
    return;
  }

  Serial.printf("Nueva versión detectada: %s. Actualizando desde: %s\n", newVersion.c_str(), binUrl.c_str());

  http.begin(client, binUrl);
  int httpCodeBin = http.GET();
  if (httpCodeBin != 200) {
    Serial.println("Error al descargar el archivo binario.");
    http.end();
    return;
  }

  int contentLength = http.getSize();
  bool canBegin = Update.begin(contentLength);
  if (!canBegin) {
    Serial.println("No hay suficiente espacio para actualizar.");
    http.end();
    return;
  }

  WiFiClient* stream = http.getStreamPtr();
  size_t written = Update.writeStream(*stream);
  if (written != contentLength) {
    Serial.printf("Solo se escribió %d/%d bytes.\n", written, contentLength);
    http.end();
    return;
  }

  if (!Update.end()) {
    Serial.printf("Error durante la actualización: %s\n", Update.errorString());
    http.end();
    return;
  }

  if (Update.isFinished()) {
    Serial.println("Actualización completada. Reiniciando...");
    delay(1000);
    ESP.restart();
  } else {
    Serial.println("Actualización incompleta.");
  }

  http.end();
}

// --- OTRAS FUNCIONES DEL PROGRAMA ---
bool leerRedesWiFiDesdeSPIFFS(WiFiNetwork *networks, int maxNetworks, int &netCount) {
  netCount = 0;
  if (!SPIFFS.exists("/wifi_networks.json")) return false;

  File file = SPIFFS.open("/wifi_networks.json", FILE_READ);
  if (!file) return false;

  size_t size = file.size();
  std::unique_ptr<char[]> buf(new char[size + 1]);
  file.readBytes(buf.get(), size);
  buf[size] = '\0';

  StaticJsonDocument<2048> doc;
  if (deserializeJson(doc, buf.get())) return false;

  if (!doc.is<JsonArray>()) return false;

  JsonArray arr = doc.as<JsonArray>();
  for (JsonObject net : arr) {
    if (netCount >= maxNetworks) break;
    networks[netCount].ssid = net["ssid"].as<String>();
    networks[netCount].password = net["password"].as<String>();
    networks[netCount].type = net["type"].as<String>();
    netCount++;
  }

  return true;
}

void print_wakeup_reason() {
  switch (esp_sleep_get_wakeup_cause()) {
    case ESP_SLEEP_WAKEUP_GPIO:
      Serial.println("Despertar por GPIO"); break;
    case ESP_SLEEP_WAKEUP_TIMER:
      Serial.println("Despertar por TIMER"); break;
    default:
      Serial.println("Otro tipo de despertar"); break;
  }
}

void guardarEventoSPIFFS(Evento evento) {
  File file = SPIFFS.open("/eventos.txt", FILE_APPEND);
  if (file) {
    file.printf("%lu,%c\n", evento.timestamp, evento.status);
    file.close();
  }
}

bool enviarEvento(Evento evento) {
  if (WiFi.status() != WL_CONNECTED) return false;

  HTTPClient http;
  String bssid = WiFi.BSSIDstr();
  String payload = "{\"id_bin\":\"4\",\"bssid\":\"" + bssid + "\", \"status\":\"" + evento.status + "\"}";

  http.begin(serverUrl);
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(5000);

  int code = http.POST(payload);
  http.end();

  return code > 0 && code < 300;
}

void enviarEventosPendientesSPIFFS() {
  File file = SPIFFS.open("/eventos.txt", FILE_READ);
  if (!file || file.size() == 0) return;

  File tempFile = SPIFFS.open("/temp.txt", FILE_WRITE);
  while (file.available()) {
    String line = file.readStringUntil('\n');
    unsigned long ts;
    char st;
    if (sscanf(line.c_str(), "%lu,%c", &ts, &st) == 2) {
      Evento e = { ts, st };
      if (!enviarEvento(e)) tempFile.printf("%lu,%c\n", ts, st);
    }
  }
  file.close();
  tempFile.close();

  SPIFFS.remove("/eventos.txt");
  SPIFFS.rename("/temp.txt", "/eventos.txt");
}

// --- SETUP PRINCIPAL ---
void setup() {
  Serial.begin(115200);
  delay(1000);
  print_wakeup_reason();

  if (!SPIFFS.begin(true)) {
    Serial.println("Error al montar SPIFFS");
    return;
  }

  pinMode(REED_PIN, INPUT_PULLUP);
  gpio_pullup_en(REED_PIN);
  gpio_pulldown_dis(REED_PIN);

  int reedState = digitalRead(REED_PIN);
  char currentStatus = (reedState == HIGH) ? '1' : '0';
  Serial.printf("Estado reed: %s\n", currentStatus == '0' ? "CERRADO" : "ABIERTO");

  if (currentStatus != lastStatus) {
    lastStatus = currentStatus;

    WiFiNetwork redes[20];
    int totalRedes = 0;
    leerRedesWiFiDesdeSPIFFS(redes, 20, totalRedes);

    bool conectado = false;
    String tipoRed = "";

    int n = WiFi.scanNetworks();
    for (int i = 0; i < totalRedes && !conectado; i++) {
      for (int j = 0; j < n && !conectado; j++) {
        if (WiFi.SSID(j) == redes[i].ssid) {
          WiFi.begin(redes[i].ssid.c_str(), redes[i].password.c_str());
          unsigned long start = millis();
          while (WiFi.status() != WL_CONNECTED && millis() - start < 10000) delay(100);

          if (WiFi.status() == WL_CONNECTED) {
            conectado = true;
            tipoRed = redes[i].type;
            Serial.printf("Conectado a %s (%s)\n", redes[i].ssid.c_str(), tipoRed.c_str());
            checkAndPerformOTA();  // <--- ACTUALIZACIÓN AUTOMÁTICA
          }
        }
      }
    }

    Evento evento = { millis(), currentStatus };

    if (conectado) {
      if (tipoRed == "truck") {
        if (!enviarEvento(evento)) guardarEventoSPIFFS(evento);
        enviarEventosPendientesSPIFFS();
        esp_sleep_enable_timer_wakeup(15 * 60 * 1000000ULL);
      } else if (tipoRed == "store_cedis") {
        for (int i = 0; i < 5; i++) {
          if (!enviarEvento(evento)) guardarEventoSPIFFS(evento);
          enviarEventosPendientesSPIFFS();
          delay(500);
        }
        esp_sleep_enable_timer_wakeup(15 * 60 * 1000000ULL);
        esp_deep_sleep_enable_gpio_wakeup(1ULL << REED_PIN,
          (currentStatus == '0') ? ESP_GPIO_WAKEUP_GPIO_HIGH : ESP_GPIO_WAKEUP_GPIO_LOW);
      }
    } else {
      guardarEventoSPIFFS(evento);
      esp_deep_sleep_enable_gpio_wakeup(1ULL << REED_PIN,
        (currentStatus == '0') ? ESP_GPIO_WAKEUP_GPIO_HIGH : ESP_GPIO_WAKEUP_GPIO_LOW);
    }

  } else {
    esp_deep_sleep_enable_gpio_wakeup(1ULL << REED_PIN,
      (currentStatus == '0') ? ESP_GPIO_WAKEUP_GPIO_HIGH : ESP_GPIO_WAKEUP_GPIO_LOW);
  }

  Serial.println("Entrando en deep sleep...");
  delay(100);
  esp_deep_sleep_start();
}

void loop() {
  // No se usa
}
