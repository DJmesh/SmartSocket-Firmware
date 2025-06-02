/**************************************************************
 *  Smart Socket – ESP32
 *
 *  • Mede tensão / corrente com EmonLib
 *  • Publica JSON em   POST http://{IP{PORTA}}/api/energy/
 *  • Aceita comando    POST /relay { "state":"on"|"off" }  → liga/desliga relé
 *  • Mantém atalhos    GET /liga  – GET /desliga (debug rápido)
 *************************************************************/

#include <WiFi.h>
#include <EmonLib.h>          // https://github.com/openenergymonitor/EmonLib
#include <ESPAsyncWebServer.h>
#include <HTTPClient.h>

/*─────────────────────────────────────────────
  CONFIGURAÇÕES
─────────────────────────────────────────────*/
const char* SSID     = "iPhone LvLuz";
const char* PASSWORD = "leoverluz";

const char* POST_URL = "http://{IP:{PORTA}}/api/energy/"; // backend Django
const char* DEVICE_ID = "station01";                            // id fixo

const uint32_t SEND_INTERVAL_MS = 15'000; // 15 s

// pinos da placa – altere se necessário
const int PIN_RELAY     = 26;
const int PIN_CORRENTE  = 35;
const int PIN_TENSAO    = 34;

/*─────────────────────────────────────────────
  OBJETOS GLOBAIS
─────────────────────────────────────────────*/
EnergyMonitor emonI;    // corrente
EnergyMonitor emonV;    // tensão
AsyncWebServer server(80);

unsigned long lastSend = 0;

/*─────────────────────────────────────────────
  SETUP
─────────────────────────────────────────────*/
void setup() {
  Serial.begin(115200);
  delay(200);

  // Conexão Wi-Fi
  Serial.printf("Conectando-se a %s ...\n", SSID);
  WiFi.begin(SSID, PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    Serial.print('.');
    delay(500);
  }
  Serial.printf("\nWi-Fi conectado! IP: %s\n", WiFi.localIP().toString().c_str());

  // Inicializa sensores (calibrações de exemplo)
  emonI.current(PIN_CORRENTE, 111.1);  // 111.1 = calibração (A / ADC count)
  emonV.voltage(PIN_TENSAO, 260, 1.7); // 260   = calibração (V / ADC count)

  pinMode(PIN_RELAY, OUTPUT);
  digitalWrite(PIN_RELAY, LOW);        // relé OFF no boot

  /*────────── Endpoints HTTP ─────────*/
  // Página rápida de diagnóstico
  server.on("/", HTTP_GET, handleRoot);

  // GET /liga  |  GET /desliga   (atalhos)
  server.on("/liga",    HTTP_GET, [](auto *req){ switchRelay(true);  req->send(200, "text/plain", "on"); });
  server.on("/desliga", HTTP_GET, [](auto *req){ switchRelay(false); req->send(200, "text/plain", "off"); });

  // POST /relay  { "state":"on" } ou { "state":"off" }
  server.on("/relay", HTTP_POST, handleRelay);

  server.begin();
  Serial.println("HTTP server iniciado.");
}

/*─────────────────────────────────────────────
  LOOP
─────────────────────────────────────────────*/
void loop() {
  unsigned long now = millis();
  if (now - lastSend >= SEND_INTERVAL_MS) {
    lastSend = now;
    publishReading();
  }
}

/*─────────────────────────────────────────────
  FUNÇÕES AUXILIARES
─────────────────────────────────────────────*/
void switchRelay(bool on) {
  digitalWrite(PIN_RELAY, on ? HIGH : LOW);
}

void handleRoot(AsyncWebServerRequest *req) {
  double I = emonI.calcIrms(1480);
  double V = emonV.calcIrms(1480);
  double S = V * I;
  double pf = 0.95;              // suposição
  double P = S * pf;
  double Q = sqrt(S * S - P * P);

  String html;
  html += "<h3>Leituras de Energia</h3>";
  html += "Tensão: "; html += V; html += " V<br>";
  html += "Corrente: "; html += I; html += " A<br>";
  html += "Potência Ativa: "; html += P; html += " W<br>";
  html += "Potência Aparente: "; html += S; html += " VA<br>";
  html += "Potência Reativa: "; html += Q; html += " VAR<br>";
  req->send(200, "text/html", html);
}

void handleRelay(AsyncWebServerRequest *req) {
  if (!req->hasParam("plain", true)) {
    req->send(400, "application/json", "{\"error\":\"JSON ausente\"}");
    return;
  }
  String body = req->getParam("plain", true)->value();
  body.toLowerCase();

  if (body.indexOf("\"state\":\"on\"") >= 0) {
    switchRelay(true);
    req->send(200, "application/json", "{\"rele\":\"on\"}");
  } else if (body.indexOf("\"state\":\"off\"") >= 0) {
    switchRelay(false);
    req->send(200, "application/json", "{\"rele\":\"off\"}");
  } else {
    req->send(400, "application/json", "{\"error\":\"use 'on' ou 'off'\"}");
  }
}

void publishReading() {
  // 1. Mede
  double I = emonI.calcIrms(1480);
  double V = emonV.calcIrms(1480);
  double S = V * I;
  double pf = 0.95;
  double P = S * pf;
  double Q = sqrt(S * S - P * P);

  // 2. Monta JSON (compatível com serializer)
  String json = "{";
  json += "\"device_id\":\"";      json += DEVICE_ID; json += "\",";
  json += "\"timestamp_ms\":";    json += millis();  json += ",";
  json += "\"voltage_v\":";       json += String(V, 2); json += ",";
  json += "\"current_a\":";       json += String(I, 2); json += ",";
  json += "\"active_power_w\":";  json += String(P, 2); json += ",";
  json += "\"apparent_power_va\":"; json += String(S, 2); json += ",";
  json += "\"reactive_power_var\":"; json += String(Q, 2);
  json += "}";

  Serial.println("\nPOST -> " + String(POST_URL));
  Serial.println(json);

  // 3. Envia
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    http.begin(POST_URL);
    http.addHeader("Content-Type", "application/json");

    int code = http.POST(json);
    if (code > 0)
      Serial.printf("Resposta HTTP: %d\n", code);
    else
      Serial.printf("Erro POST: %s\n", http.errorToString(code).c_str());

    http.end();
  } else {
    Serial.println("Wi-Fi OFF – não enviado.");
  }
}
