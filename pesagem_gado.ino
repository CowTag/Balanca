
#include <WiFi.h>
#include <PubSubClient.h>
#include <SPI.h>
#include <MFRC522.h>
#include "HX711.h"
#include <ArduinoJson.h>
#include <time.h>

// ---------------------- CONFIGURAÇÕES ----------------------
const char* WIFI_SSID     = "Familia_BR";
const char* WIFI_PASSWORD = "SUA_SENHA";

const char* MQTT_BROKER   = "192.168.0.100";   // Host do broker Mosquitto
const int   MQTT_PORT     = 1883;
const char* MQTT_USER     = "";                
const char* MQTT_PASS     = "";
const char* MQTT_TOPIC_PESAGEM = "fazenda/curral01/pesagem";
const char* MQTT_TOPIC_STATUS  = "fazenda/curral01/status/ESP32-BALANCA-01";
const char* DEVICE_ID = "ESP32-BALANCA-01";

// Pinos HX711
#define HX711_DT   16
#define HX711_SCK  4
// Fator de calibração: ajustar experimentalmente com peso conhecido.
// calibration_factor = leitura_bruta / peso_conhecido_em_kg
float CALIBRATION_FACTOR = 2280.0;

// Pinos RC522
#define RFID_SS_PIN  5
#define RFID_RST_PIN 22

// Estabilização de peso
const int   AMOSTRAS_ESTABILIZACAO = 10;
const float TOLERANCIA_KG          = 0.02; 

// ---------------------- OBJETOS GLOBAIS ----------------------
HX711 balanca;
MFRC522 rfid(RFID_SS_PIN, RFID_RST_PIN);
WiFiClient espClient;
PubSubClient mqttClient(espClient);

// ---------------------- ESTRUTURA DE DADOS DO ANIMAL ----------------------
// Espelha o mapeamento de blocos definido no cartão Mifare Classic 1K
struct RegistroVacina {
  char codigo[4];      
  uint32_t data_unix;
};

struct DadosAnimal {
  char id[16];              
  char data_nascimento[9];  
  char sexo;                
  char raca[20];
  float ultimo_peso_kg;     
  uint32_t timestamp_pesagem;
  RegistroVacina vacinas[6]; 
};

// ==================== SETUP ====================
void setup() {
  Serial.begin(115200);
  delay(500);

  setupWiFi();
  configTime(-3 * 3600, 0, "pool.ntp.org", "time.nist.gov"); 

  mqttClient.setServer(MQTT_BROKER, MQTT_PORT);

  // Inicializa HX711
  balanca.begin(HX711_DT, HX711_SCK);
  balanca.set_scale(CALIBRATION_FACTOR);
  balanca.tare(); 
  Serial.println("HX711 pronto e tarado.");

  // Inicializa RC522
  SPI.begin();
  rfid.PCD_Init();
  Serial.println("RC522 pronto. Aguardando tag...");
}

// ==================== LOOP PRINCIPAL ====================
void loop() {
  if (!mqttClient.connected()) {
    reconnectMQTT();
  }
  mqttClient.loop();

  // Verifica se há uma tag presente
  if (!rfid.PICC_IsNewCardPresent() || !rfid.PICC_ReadCardSerial()) {
    delay(200);
    return;
  }

  Serial.println("Tag detectada. Lendo dados do animal...");
  DadosAnimal animal;
  bool leituraOk = lerDadosDaTag(animal);

  if (!leituraOk) {
    Serial.println("Falha ao ler a tag. Abortando ciclo.");
    finalizarLeituraRFID();
    return;
  }

  // Realiza pesagem estabilizada
  float pesoKg;
  bool pesoOk = obterPesoEstabilizado(pesoKg);

  if (!pesoOk) {
    Serial.println("Peso não estabilizou dentro do tempo limite.");
    finalizarLeituraRFID();
    return;
  }

  Serial.printf("Peso aferido: %.2f kg\n", pesoKg);

  // Atualiza estrutura local e regrava na tag
  animal.ultimo_peso_kg = pesoKg;
  animal.timestamp_pesagem = obterTimestampUnix();
  bool gravacaoOk = gravarPesoNaTag(animal);
  if (!gravacaoOk) {
    Serial.println("Aviso: falha ao regravar peso na tag (dado ainda será enviado via MQTT).");
  }

  // Monta e publica o payload MQTT
  publicarPesagem(animal, pesoKg);

  finalizarLeituraRFID();
  delay(1000); 
}

// ==================== WI-FI ====================
void setupWiFi() {
  Serial.printf("Conectando ao WiFi %s", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    delay(400);
    Serial.print(".");
  }
  Serial.printf("\nWiFi conectado. IP: %s\n", WiFi.localIP().toString().c_str());
}

// ==================== MQTT ====================
void reconnectMQTT() {
  while (!mqttClient.connected()) {
    Serial.print("Conectando ao broker MQTT...");
    bool conectou;
    if (strlen(MQTT_USER) > 0) {
      conectou = mqttClient.connect(DEVICE_ID, MQTT_USER, MQTT_PASS,
                                     MQTT_TOPIC_STATUS, 1, true, "offline");
    } else {
      conectou = mqttClient.connect(DEVICE_ID, MQTT_TOPIC_STATUS, 1, true, "offline");
    }

    if (conectou) {
      Serial.println("conectado.");
      mqttClient.publish(MQTT_TOPIC_STATUS, "online", true); // retained
    } else {
      Serial.printf("falhou, rc=%d. Tentando novamente em 3s.\n", mqttClient.state());
      delay(3000);
    }
  }
}

void publicarPesagem(const DadosAnimal &animal, float pesoKg) {
  StaticJsonDocument<512> doc;

  doc["device_id"] = DEVICE_ID;
  doc["timestamp"] = obterTimestampISO8601();

  JsonObject animalObj = doc.createNestedObject("animal");
  animalObj["id"] = animal.id;
  animalObj["data_nascimento"] = animal.data_nascimento;

  JsonArray vacinasArr = animalObj.createNestedArray("vacinas");
  for (int i = 0; i < 6; i++) {
    if (strlen(animal.vacinas[i].codigo) == 0) continue;
    JsonObject v = vacinasArr.createNestedObject();
    v["codigo"] = animal.vacinas[i].codigo;
    v["timestamp"] = animal.vacinas[i].data_unix;
  }

  JsonObject pesagemObj = doc.createNestedObject("pesagem");
  pesagemObj["peso_kg"] = round(pesoKg * 100) / 100.0;
  pesagemObj["estabilizado"] = true;

  JsonObject dispObj = doc.createNestedObject("dispositivo");
  dispObj["firmware"] = "1.0.0";
  dispObj["rssi_dbm"] = WiFi.RSSI();

  char buffer[512];
  size_t n = serializeJson(doc, buffer);

  bool enviado = mqttClient.publish(MQTT_TOPIC_PESAGEM, buffer, n);
  Serial.println(enviado ? "Payload publicado com sucesso." : "Falha ao publicar payload.");
  Serial.println(buffer);
}

// ==================== PESAGEM (HX711) ====================
bool obterPesoEstabilizado(float &pesoFinal) {
  const int TENTATIVAS_MAX = 50; 
  float ultimaLeitura = -9999;
  int amostrasEstaveis = 0;

  for (int i = 0; i < TENTATIVAS_MAX; i++) {
    if (!balanca.is_ready()) {
      delay(200);
      continue;
    }

    float leitura = balanca.get_units(3); 
    if (leitura < 0) leitura = 0;

    if (fabs(leitura - ultimaLeitura) <= TOLERANCIA_KG) {
      amostrasEstaveis++;
    } else {
      amostrasEstaveis = 0;
    }

    ultimaLeitura = leitura;

    if (amostrasEstaveis >= AMOSTRAS_ESTABILIZACAO) {
      pesoFinal = leitura;
      return true;
    }

    delay(200);
  }

  return false;
}

// ==================== RFID: LEITURA ====================
bool lerDadosDaTag(DadosAnimal &animal) {
  MFRC522::MIFARE_Key key;
  for (byte i = 0; i < 6; i++) key.keyByte[i] = 0xFF; // chave padrão de fábrica

  byte buffer[18];
  byte tamanho = 18;

  // --- Setor 1, bloco 4: ID do animal ---
  if (!autenticarEler(4, key, buffer, tamanho)) return false;
  memcpy(animal.id, buffer, 15);
  animal.id[15] = '\0';

  // --- Setor 2, bloco 8: data de nascimento, sexo, raça ---
  if (!autenticarEler(8, key, buffer, tamanho)) return false;
  memcpy(animal.data_nascimento, buffer, 8);
  animal.data_nascimento[8] = '\0';
  animal.sexo = buffer[8];
  memcpy(animal.raca, buffer + 9, 6); // simplificado para o protótipo (ver nota abaixo)

  // --- Setor 3, bloco 12: último peso + timestamp ---
  if (!autenticarEler(12, key, buffer, tamanho)) return false;
  memcpy(&animal.ultimo_peso_kg, buffer, 4);
  memcpy(&animal.timestamp_pesagem, buffer + 4, 4);

  // --- Setores 4 e 5: histórico de vacinas (6 registros de 8 bytes) ---
  int idx = 0;
  for (int bloco : {16, 17, 20, 21}) {
    if (!autenticarEler(bloco, key, buffer, tamanho)) continue;
    for (int offset = 0; offset < 16 && idx < 6; offset += 8) {
      memcpy(animal.vacinas[idx].codigo, buffer + offset, 3);
      animal.vacinas[idx].codigo[3] = '\0';
      memcpy(&animal.vacinas[idx].data_unix, buffer + offset + 4, 4);
      idx++;
    }
  }

  return true;
}

bool gravarPesoNaTag(const DadosAnimal &animal) {
  MFRC522::MIFARE_Key key;
  for (byte i = 0; i < 6; i++) key.keyByte[i] = 0xFF;

  byte status = rfid.PCD_Authenticate(MFRC522::PICC_CMD_MF_AUTH_KEY_A, 12, &key, &(rfid.uid));
  if (status != MFRC522::STATUS_OK) return false;

  byte dados[16] = {0};
  memcpy(dados, &animal.ultimo_peso_kg, 4);
  memcpy(dados + 4, &animal.timestamp_pesagem, 4);

  status = rfid.MIFARE_Write(12, dados, 16);
  return status == MFRC522::STATUS_OK;
}

bool autenticarEler(byte bloco, MFRC522::MIFARE_Key &key, byte *buffer, byte &tamanho) {
  byte status = rfid.PCD_Authenticate(MFRC522::PICC_CMD_MF_AUTH_KEY_A, bloco, &key, &(rfid.uid));
  if (status != MFRC522::STATUS_OK) {
    Serial.printf("Falha na autenticação do bloco %d\n", bloco);
    return false;
  }
  status = rfid.MIFARE_Read(bloco, buffer, &tamanho);
  return status == MFRC522::STATUS_OK;
}

void finalizarLeituraRFID() {
  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();
}

// ==================== TEMPO ====================
uint32_t obterTimestampUnix() {
  time_t agora;
  time(&agora);
  return (uint32_t)agora;
}

String obterTimestampISO8601() {
  time_t agora;
  time(&agora);
  struct tm timeinfo;
  localtime_r(&agora, &timeinfo);
  char buf[30];
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S-03:00", &timeinfo);
  return String(buf);
}
