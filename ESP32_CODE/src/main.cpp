#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <ESP32Servo.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <MFRC522.h>
#include <SPI.h>
#include <ArduinoJson.h>
#include <SPIFFS.h>
#include <ArduinoOTA.h>

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
#define SCREEN_ADDRESS 0x3C

#define SERVO_PIN 13
#define IR_PIN 25

#define LED_VAGA1_VERDE 4
#define LED_VAGA1_VERMELHO 5
#define LED_VAGA2_VERDE 12
#define LED_VAGA2_VERMELHO 14

#define SS_PIN   15
#define RST_PIN  2

#define SCK_PIN  18
#define MOSI_PIN 23
#define MISO_PIN 19

const char* ssid = "MOB-TAKAZONO";
const char* password = "970327takazono";
const char* api_url = "http://192.168.18.214:8000";
const char* ota_password = "987654321";  
Servo servoMotor;
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

MFRC522 rfid(SS_PIN, RST_PIN);

const int NUM_SENSORS = 2;
const int TRIG_PINS[NUM_SENSORS] = {26, 32};
const int ECHO_PINS[NUM_SENSORS] = {27, 33};

const float DISTANCE_THRESHOLD_VAGA1 = 16.92;
const float DISTANCE_THRESHOLD_VAGA2 = 18.54;
const unsigned long TRANSITION_DELAY = 10000; // Tempo para a transição de estado de ocupado para livre
const unsigned long DEBOUNCE_DELAY = 500; // Debounce para leituras de ocupação

unsigned long lastMeasurementMillis[NUM_SENSORS] = {0};
unsigned long lastOccupiedMillis[NUM_SENSORS] = {0};
unsigned long lastFreeMillis[NUM_SENSORS] = {0};
bool isOccupied[NUM_SENSORS] = {false};
bool isTransitioning[NUM_SENSORS] = {false};
bool isGateOpen = false;
bool isIRDetected = false;
unsigned long gateOpenedMillis = 0;
unsigned long lastIRDetectionMillis = 0;
unsigned long countdownMillis = 0;
bool countdownActive = false;

TaskHandle_t taskHandleGateControl;

void checkParkingSpace(int trigPin, int echoPin, unsigned long &lastMeasurementMillis, unsigned long &lastOccupiedMillis, unsigned long &lastFreeMillis, bool &isOccupied, bool &isTransitioning, float distanceThreshold);
float getDistance(int trigPin, int echoPin);
void displayCenteredText(const char* line1, const char* line2);
void openGate();
void closeGate();
void gateControlTask(void *pvParameters);
String readRFID();
bool checkUID(String uid);
bool registerTimestamp(String uid);
bool vagasDisponiveis();
void writeLog(String logEntry, String uid = "");
void readLog();  

void setup() {
  Serial.begin(115200);

  if (!SPIFFS.begin(true)) {
    Serial.println("Falha ao montar o sistema de arquivos");
    return;
  }
  Serial.println("Sistema de arquivos montado com sucesso");

  // Inicializa o display OLED
  if (!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) {
    Serial.println(F("Falha ao inicializar o display OLED"));
    for (;;);
  }

  display.clearDisplay();
  display.setTextSize(2);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.print("Iniciando...");
  display.display();

  delay(2000);

  // Configura sensores de distância
  for (int i = 0; i < NUM_SENSORS; i++) {
    pinMode(TRIG_PINS[i], OUTPUT);
    pinMode(ECHO_PINS[i], INPUT);
    digitalWrite(TRIG_PINS[i], LOW);
  }

  // Configura os LEDs das vagas
  pinMode(LED_VAGA1_VERDE, OUTPUT);
  pinMode(LED_VAGA1_VERMELHO, OUTPUT);
  pinMode(LED_VAGA2_VERDE, OUTPUT);
  pinMode(LED_VAGA2_VERMELHO, OUTPUT);

  // Configura o servo motor e o sensor IR
  servoMotor.attach(SERVO_PIN, 500, 2400);
  pinMode(IR_PIN, INPUT);

  SPI.begin();
  rfid.PCD_Init();

  closeGate();

  // Conectar ao WiFi
  WiFi.begin(ssid, password);
  Serial.print("Conectando-se à rede WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("Conectado à rede WiFi");
  Serial.print("Endereço IP: ");
  Serial.println(WiFi.localIP());

  ArduinoOTA.setPassword(ota_password);  // Define a senha OTA

  // Feedback de progresso OTA
  ArduinoOTA.onStart([]() {
    String type = (ArduinoOTA.getCommand() == U_FLASH) ? "sketch" : "filesystem";
    Serial.println("Iniciando atualização OTA: " + type);
  });

  ArduinoOTA.onEnd([]() {
    Serial.println("\nAtualização OTA finalizada.");
  });

  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("Progresso: %u%%\r", (progress / (total / 100)));
  });

  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("Erro [%u]: ", error);
    if (error == OTA_AUTH_ERROR) {
      Serial.println("Falha de autenticação");
    } else if (error == OTA_BEGIN_ERROR) {
      Serial.println("Erro no início");
    } else if (error == OTA_CONNECT_ERROR) {
      Serial.println("Erro de conexão");
    } else if (error == OTA_RECEIVE_ERROR) {
      Serial.println("Erro no recebimento");
    } else if (error == OTA_END_ERROR) {
      Serial.println("Erro ao finalizar");
    }
  });

  // Iniciar o serviço OTA
  ArduinoOTA.begin();
  Serial.println("Pronto para atualização OTA");

  // Tarefa para controlar a cancela
  xTaskCreatePinnedToCore(
    gateControlTask,
    "Gate Control Task",
    2048,
    NULL,
    1,
    &taskHandleGateControl,
    1
  );
}

void loop() {
  
  ArduinoOTA.handle();
  int closestFreeIndex = -1;
  float minDistance = 1000;

  // Verificar ocupação das vagas apenas com os sensores de distância
  for (int i = 0; i < NUM_SENSORS; i++) {
    float distanceThreshold = (i == 0) ? DISTANCE_THRESHOLD_VAGA1 : DISTANCE_THRESHOLD_VAGA2;
    checkParkingSpace(TRIG_PINS[i], ECHO_PINS[i], lastMeasurementMillis[i], lastOccupiedMillis[i], lastFreeMillis[i], isOccupied[i], isTransitioning[i], distanceThreshold);
    float distance = getDistance(TRIG_PINS[i], ECHO_PINS[i]);
    if (!isOccupied[i] && distance < minDistance) {
      minDistance = distance;
      closestFreeIndex = i;
    }

    // Atualiza os LEDs das vagas
    if (i == 0) {
      if (isOccupied[i]) {
        digitalWrite(LED_VAGA1_VERDE, LOW);
        digitalWrite(LED_VAGA1_VERMELHO, HIGH);
      } else {
        digitalWrite(LED_VAGA1_VERDE, HIGH);
        digitalWrite(LED_VAGA1_VERMELHO, LOW);
      }
    } else if (i == 1) {
      if (isOccupied[i]) {
        digitalWrite(LED_VAGA2_VERDE, LOW);
        digitalWrite(LED_VAGA2_VERMELHO, HIGH);
      } else {
        digitalWrite(LED_VAGA2_VERDE, HIGH);
        digitalWrite(LED_VAGA2_VERMELHO, LOW);
      }
    }
  }

  // Exibe status no display
  display.clearDisplay();
  if (vagasDisponiveis() && closestFreeIndex != -1) {
    displayCenteredText(
      ("Vaga " + String(closestFreeIndex + 1)).c_str(),
      "LIVRE"
    );
  } else {
    displayCenteredText("TODAS", "OCUPADAS");
  }

  display.display();

  // Leitura do cartão RFID
  if (rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial()) {
    String uid = readRFID();
    Serial.print("UID detectado: ");
    Serial.println(uid);

    if (checkUID(uid)) {
      if (vagasDisponiveis()) { // Somente registra se houver vagas disponíveis
        openGate();
        gateOpenedMillis = millis();
        lastIRDetectionMillis = millis();
        countdownActive = false; // Reset countdown

        if (registerTimestamp(uid)) {
          Serial.println("Timestamp registrado.");
        } else {
          Serial.println("Erro ao registrar timestamp.");
        }
      } else {
        Serial.println("Não há vagas disponíveis. Não foi possível registrar timestamp.");
      }
    } else {
      Serial.println("UID não registrado.");
    }

    rfid.PICC_HaltA();
    rfid.PCD_StopCrypto1();
  }

  // Verificação do sensor IR
  isIRDetected = digitalRead(IR_PIN) == LOW;

  // Ativa a contagem regressiva se a cancela estiver aberta e o sensor IR detectar o carro saindo
  if (isGateOpen && !isIRDetected) {
    if (countdownActive) {
      if (millis() - countdownMillis >= TRANSITION_DELAY) {
        closeGate();
        countdownActive = false;
      }
    } else {
      countdownMillis = millis();
      countdownActive = true;
    }
  } else {
    countdownActive = false;
  }

  // Verifica comandos enviados pelo terminal serial
  if (Serial.available()) {
    String command = Serial.readStringUntil('\n');
    command.trim();  // Remove espaços em branco e quebras de linha extras

    // Se o comando for "logs", lê e exibe os logs no console
    if (command.equalsIgnoreCase("logs")) {
      readLog();
    }
  }

  delay(100);
}



// Função para verificar estado de ocupação das vagas
void checkParkingSpace(int trigPin, int echoPin, unsigned long &lastMeasurementMillis, unsigned long &lastOccupiedMillis, unsigned long &lastFreeMillis, bool &isOccupied, bool &isTransitioning, float distanceThreshold) {
  long duration;
  float distance;

  // Gera um pulso para o sensor ultrassônico
  digitalWrite(trigPin, LOW);
  delayMicroseconds(2);
  digitalWrite(trigPin, HIGH);
  delayMicroseconds(10);
  digitalWrite(trigPin, LOW);

  duration = pulseIn(echoPin, HIGH);
  distance = (duration / 2.0) * 0.0344; // Converte a duração do pulso em distância

  unsigned long currentMillis = millis();


  // Verifica se o objeto está dentro do limite de distância para considerar a vaga ocupada
  if (distance < distanceThreshold) {
    if (!isOccupied && !isTransitioning) {
      lastOccupiedMillis = currentMillis;
      isTransitioning = true;
    } else if (isTransitioning && currentMillis - lastOccupiedMillis >= DEBOUNCE_DELAY) {
      isOccupied = true;
      isTransitioning = false;
    }
  } else {
    if (isOccupied && !isTransitioning) {
      lastFreeMillis = currentMillis;
      isTransitioning = true;
    } else if (isTransitioning && currentMillis - lastFreeMillis >= DEBOUNCE_DELAY) {
      isOccupied = false;
      isTransitioning = false;
    }
  }
}

// Função para obter a distância
float getDistance(int trigPin, int echoPin) {
  digitalWrite(trigPin, LOW);
  delayMicroseconds(2);
  digitalWrite(trigPin, HIGH);
  delayMicroseconds(10);
  digitalWrite(trigPin, LOW);
  long duration = pulseIn(echoPin, HIGH);
  float distance = (duration / 2.0) * 0.0344;
  return distance;
}

// Exibe texto centralizado no display OLED
void displayCenteredText(const char* line1, const char* line2) {
  display.clearDisplay();
  display.setTextSize(2);
  display.setTextColor(SSD1306_WHITE);
  int16_t x1, y1;
  uint16_t w, h;
  display.getTextBounds(line1, 0, 0, &x1, &y1, &w, &h);
  display.setCursor((SCREEN_WIDTH - w) / 2, 0);
  display.print(line1);
  display.getTextBounds(line2, 0, 0, &x1, &y1, &w, &h);
  display.setCursor((SCREEN_WIDTH - w) / 2, 30);
  display.print(line2);
  display.display();
}

// Abre a cancela
void openGate() {
  servoMotor.write(90); // Angulo para abrir a cancela
  isGateOpen = true;
}

// Fecha a cancela
void closeGate() {
  servoMotor.write(0); // Angulo para fechar a cancela
  isGateOpen = false;
}

// Tarefa para controlar a cancela
void gateControlTask(void *pvParameters) {
  while (true) {
    // Verificação do sensor IR
    isIRDetected = digitalRead(IR_PIN) == LOW;

    // Verifica se a cancela deve ser fechada
    if (isGateOpen && !isIRDetected) {
      if (countdownActive) {
        if (millis() - countdownMillis >= TRANSITION_DELAY) {
          closeGate();
          countdownActive = false;
        }
      } else {
        countdownMillis = millis();
        countdownActive = true;
      }
    } else {
      countdownActive = false;
    }

    vTaskDelay(100 / portTICK_PERIOD_MS);
  }
}

// Lê o cartão RFID
// Função para ler o UID do cartão RFID
String readRFID() {
  String uid = "";
  for (byte i = 0; i < rfid.uid.size; i++) {
    uid += String(rfid.uid.uidByte[i], HEX);
  }
  uid.toUpperCase();

  String logEntry = "Tentativa de acesso RFID: UID = " + uid + " em " + String(millis());
  writeLog(logEntry, uid);

  return uid;
}

// Função para verificar se o UID é válido
bool checkUID(String uid) {
  HTTPClient http;
  String endpoint = api_url + String("/check_uid/") + uid;
  http.begin(endpoint);
  int httpCode = http.GET();

  if (httpCode == 200) {
    String payload = http.getString();
    Serial.println("Resposta do servidor:");  // Para depuração
    Serial.println(payload);                  // Para depuração
    
    StaticJsonDocument<200> doc;
    deserializeJson(doc, payload);

    const char* message = doc["message"];  // Verificar o campo "message"
    Serial.print("Mensagem do servidor: ");
    Serial.println(message);  // Para depuração

    bool isValid = (String(message) == "UID registrado");
    Serial.print("Validação do UID: ");
    Serial.println(isValid);  // Para depuração

    http.end();
    return isValid;
  }

  http.end();
  return false;
}



// Função para registrar timestamp (verifique se o endpoint está correto também)
bool registerTimestamp(String uid) {
  HTTPClient http;
  String endpoint = api_url + String("/timestamps?uid=") + uid; // Adiciona UID como parâmetro de query
  http.begin(endpoint);
  int httpCode = http.POST("");

  if (httpCode == 200) {
    String logEntry = "Timestamp registrado para UID = " + uid + " em " + String(millis());
    writeLog(logEntry, uid);
    http.end();
    return true;
  }
  String logEntry = "Falha ao registrar timestamp para UID = " + uid;
  writeLog(logEntry, uid);
  http.end();
  return false;
}

// Função para verificar se há vagas disponíveis
bool vagasDisponiveis() {
  for (int i = 0; i < NUM_SENSORS; i++) {
    if (!isOccupied[i]) {
      return true;
    }
  }
  return false;
}


void writeLog(String logEntry, String uid) {
  File file = SPIFFS.open("/access_log.txt", FILE_APPEND);
  if (!file) {
    Serial.println("Erro ao abrir o arquivo para gravar");
    return;
  }

  // Se um UID for passado, adiciona ao log
  if (uid != "") {
    logEntry = logEntry + " (UID: " + uid + ")";
  }

  file.println(logEntry);
  file.close();
}

void readLog() {
  File file = SPIFFS.open("/access_log.txt", FILE_READ);
  if (!file) {
    Serial.println("Erro ao abrir o arquivo de log");
    return;
  }
  Serial.println("Log de acessos:");
  while (file.available()) {
    Serial.println(file.readStringUntil('\n'));
  }
  file.close();
}

