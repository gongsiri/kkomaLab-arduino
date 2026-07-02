#include <WiFiS3.h>           
#include <PubSubClient.h>     
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <DHT.h>
#include <SoftwareSerial.h>
#include <MHZ19.h>  
#include "arduino.secrets.h"         

// ============================================
// 설정값
// ============================================
const char* WIFI_SSID = SECRET_WIFI_SSID;
const char* WIFI_PASSWORD = SECRET_WIFI_PASSWORD;
const char* MQTT_SERVER = SECRET_MQTT_SERVER; 
const int MQTT_PORT = SECRET_MQTT_PORT;

const char* TOPIC_TEMP = "temperature";
const char* TOPIC_HUMI = "humidity";
const char* TOPIC_CO2 = "co2";
const char* TOPIC_FAN_STATE = "fan/state";

const char* TOPIC_FAN_COMMAND = "fan/cmd";

#define RX_PIN 2   
#define TX_PIN 3     
#define FAN_PIN 9     
#define DHT_PIN 5     
#define DHT_TYPE DHT22

#define CO2_LIMIT 2000

// 데이터를 측정하고 전송하는 주기
const unsigned long SENSOR_INTERVAL = 10000;

const unsigned long WIFI_RETRY_INTERVAL = 10000;
const unsigned long MQTT_RETRY_INTERVAL = 5000;
const unsigned long WIFI_CONNECTION_TIMEOUT = 10000;
const int WIFI_MAX_RETRY_COUNT = 3;


// ============================================
// 팬 제어 모드
// ============================================
// 현재 팬의 모드
enum FanControlMode {
  FAN_MODE_AUTO,
  FAN_MODE_MANUAL
};


// ============================================
// 객체 생성
// ============================================
DHT dht(DHT_PIN, DHT_TYPE);
LiquidCrystal_I2C lcd(0x27, 16, 2);
SoftwareSerial co2Serial(RX_PIN, TX_PIN); 
MHZ19 myMHZ19;
WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);


// ============================================
// 현재 센서값
// ============================================
int currentCo2 = 400;
float currentTemp = 0.0;
float currentHumi = 0.0;


// ============================================
// 현재 팬 상태
// ============================================
FanControlMode currentFanMode = FAN_MODE_AUTO;

bool currentFanState = LOW;
bool manualFanState = LOW;



// ============================================
// 시간 변수
// ============================================
// 마지막 센서 측정 시각
// 센서 측정 / 팬 제어 / LCD 갱신 / MQTT 발행이 포함됨
unsigned long lastSensorCycleTime = 0;
unsigned long lastWiFiRetryTime = 0;
unsigned long lastMQTTRetryTime = 0;


// ============================================
// LCD 출력 함수
// ============================================
void printLCD(int row, const char* message){
  if (row < 0 || row > 1 || message == nullptr) return;

  char line[17];
  memset(line, ' ', 16); // 이전 출력된 문자열 일부가 남는 것을 막음
  size_t length = strlen(message);

  if (length > 16) length = 16;

  memcpy(line, message, length);
  line[16] = '\0';

  lcd.setCursor(0, row);
  lcd.print(line);
}


// ============================================
// 현재 상태 LCD 출력 함수
// ============================================
void updateLCD() {
  char firstLine[17];
  char secondLine[17];

  snprintf(
    firstLine,
    sizeof(firstLine),
    "T:%.1f H:%.0f",
    currentTemp,
    currentHumi
  );

  snprintf(
    secondLine,
    sizeof(secondLine),
    "C:%d %s %s",
    currentCo2,
    currentFanState == HIGH ? "ON" : "OFF",
    currentFanMode == FAN_MODE_AUTO ? "A" : "M"
  );

  printLCD(0, firstLine);
  printLCD(1, secondLine);
}



// ============================================
// 팬 상태 변경 함수
// ============================================
// 팬 상태를 변경함
// true -> 팬 상태가 변경됨
// false -> 기존 상태와 같아서 변경되지 않음
bool setFanState(bool state){
  if (currentFanState == state) return false;

  currentFanState = state;
  digitalWrite(FAN_PIN, currentFanState);

  Serial.print("[FAN] State Changed : ");
  Serial.println(currentFanState == HIGH ? "ON" : "OFF");

  return true;
}


// ============================================
// MQTT 전송 함수
// ============================================
// 실수형 센서값 MQTT 전송 함수
bool publishSensorValue(const char* topic, float value){
  if (!mqttClient.connected()) return false;

  char payload[100];

  snprintf(
    payload,
    sizeof(payload),
    "{\"value\":%.1f}",
    value
  );

  bool published = mqttClient.publish(topic, payload);

  if (published) {
    Serial.print("[MQTT SEND] ");
    Serial.print(topic);
    Serial.print(" : ");
    Serial.println(payload);
  } else {
    Serial.print("[MQTT] Publish failed: "); 
    Serial.println(topic);
  }

  return published;
}

// 정수형 센서값 MQTT 전송 함수
bool publishSensorValue(const char* topic, int value){
  if (!mqttClient.connected()) return false;

  char payload[100];

  snprintf(
    payload,
    sizeof(payload),
    "{\"value\":%d}",
    value
  );

  bool published = mqttClient.publish(topic, payload);

  if (published) {
    Serial.print("[MQTT SEND] ");
    Serial.print(topic);
    Serial.print(" : ");
    Serial.println(payload);
  } else {
    Serial.print("[MQTT] Publish failed: "); 
    Serial.println(topic);
  }

  return published;
}

// 현재 제어 모드와 팬 상태 전송 함수
bool publishFanState(){
  if (!mqttClient.connected()) return false;

  char payload[100];

  const char* mode = currentFanMode == FAN_MODE_AUTO ? "AUTO" : "MANUAL"; 
  const char* state = currentFanState == HIGH ? "ON" : "OFF";

  snprintf(
    payload,
    sizeof(payload),
    "{\"mode\":\"%s\",\"state\":\"%s\"}",
    mode,
    state
  );

  // retain을 true로 설정해 가장 최근 팬 상태를 보관
  bool published = mqttClient.publish(TOPIC_FAN_STATE, payload, true); 

  if (published) { 
    Serial.print("[MQTT SEND] "); 
    Serial.print(TOPIC_FAN_STATE); 
    Serial.print(" : "); 
    Serial.println(payload); 
  } else { 
    Serial.println( "[MQTT] Fan State publish failed." ); 
  }

  return published;
}


// ============================================
// 센서 측정 함수
// ============================================
void readCO2Sensor() {
    int measuredCo2 = myMHZ19.getCO2();

    if (measuredCo2 > 0) {
      currentCo2 = measuredCo2; 
    } else { 
      Serial.println( "[CO2] Sensor read failed." ); 
    }
}

void readDHTSensor() {
  float measuredTemp =
    dht.readTemperature();

  float measuredHumi =
    dht.readHumidity();

  if (!isnan(measuredTemp)) { 
    currentTemp = measuredTemp; 
  } else {
    Serial.println( "[DHT] Temperature read failed." ); 
  } 
  
  if (!isnan(measuredHumi)) { 
    currentHumi = measuredHumi; 
  } else { 
    Serial.println( "[DHT] Humidity read failed." ); 
  }
}

void readAllSensors() {
  readCO2Sensor();
  readDHTSensor();
}



// ============================================
// 팬 제어 함수
// ============================================
// 현재 팬 모드에 따라 실제 팬 상태에 적용함
// ture -> 실제 팬 상태가 변경됨
// false -> 팬 상태가 그대로임
bool controlFan(){
  bool targetFanState;

  if (currentFanMode == FAN_MODE_MANUAL) {
    targetFanState = manualFanState;
  } else {
    targetFanState = currentCo2 >= CO2_LIMIT ? HIGH : LOW;
  }

  return setFanState(targetFanState);
}


// ============================================
// 센서 데이터 일괄 전송 함수
// ============================================
void publishSensorData(){
  if (!mqttClient.connected()) { 
    Serial.println( "[MQTT] Sensor data not published: disconnected." ); 
    return; 
  }

  bool tempPublished = publishSensorValue(TOPIC_TEMP, currentTemp);
  bool humiPublished = publishSensorValue(TOPIC_HUMI, currentHumi);
  bool co2Published = publishSensorValue(TOPIC_CO2, currentCo2);
  
  if (tempPublished && humiPublished && co2Published) {
    Serial.println("[MQTT] publish success.");
  } else {
    Serial.println("[MQTT] Publish failed.");
  }
}


// ============================================
// 현재 상태 출력 함수
// ============================================
void printCurrentState() {
  Serial.println("[CURRENT]");

  Serial.print("Temp : ");
  Serial.println(currentTemp);

  Serial.print("Humi : ");
  Serial.println(currentHumi);

  Serial.print("CO2 : ");
  Serial.println(currentCo2);

  Serial.print("Fan : ");
  Serial.println(currentFanState == HIGH ? "ON" : "OFF");

  Serial.print("Mode : ");
  Serial.println(currentFanMode == FAN_MODE_AUTO ? "AUTO" : "MANUAL");
}


// ============================================
// 전체 센서 처리 과정
// ============================================
void runSensorCycle() {
  readAllSensors();
  bool fanStateChanged = controlFan();

  if (fanStateChanged) publishFanState();

  updateLCD();
  printCurrentState();
  publishSensorData();
}


// ============================================
// 팬 명령 처리 함수
// ============================================
void handleFanCommand(const char* cmd) {
  if (cmd == nullptr) return;

  if (strcmp(cmd, "0") == 0) {
    currentFanMode = FAN_MODE_MANUAL;
    manualFanState = LOW;

    controlFan();
    updateLCD();

    publishFanState();

    Serial.println("[COMMAND] Manual fan OFF");
  } else if (strcmp(cmd, "1") == 0) {
    currentFanMode = FAN_MODE_MANUAL;
    manualFanState = HIGH;

    controlFan();
    updateLCD();

    publishFanState();

    Serial.println("[COMMAND] Manual fan ON");
  } else if (strcmp(cmd, "AUTO") == 0) {
    currentFanMode = FAN_MODE_AUTO;

    controlFan();
    updateLCD();

    publishFanState();

    Serial.println("[COMMAND] Auto mode enabled");
  } else {
    Serial.println("[COMMAND] Unknown command");
  }
}


// ============================================
// MQTT 메시지 수신 함수
// ============================================
// 메시지를 수신했을 때 호출됨
void mqttCallback(char* topic, byte* payload, unsigned int length){
  if (strcmp(topic, TOPIC_FAN_COMMAND) != 0) return; 

  char command[16];
  unsigned int copyLength = length;

  if (copyLength >= sizeof(command)) copyLength = sizeof(command) - 1;

  memcpy(command, payload, copyLength);
  command[copyLength] = '\0';

  Serial.print("[MQTT] Topic : ");
  Serial.println(topic);

  Serial.print("[MQTT] Command : ");
  Serial.println(command);

  handleFanCommand(command);
}


// ============================================
// WiFi 연결 함수
// ============================================
bool connectWiFi(){
  if(WiFi.status() == WL_CONNECTED) return true;

   if (WiFi.status() == WL_NO_MODULE) { 
    Serial.println( "[WIFI] WiFi module not found." ); 
    while (true) { 
      delay(1000); 
    } 
  }

  Serial.println("[WIFI] Connecting to network...");
  printLCD(0, "WiFi Connecting");
  printLCD(1, "");

  for (int i = 1; i<=WIFI_MAX_RETRY_COUNT; i++){
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    unsigned long startTime = millis();

    while (WiFi.status() != WL_CONNECTED && millis( )- startTime < WIFI_CONNECTION_TIMEOUT) {
      delay(500);
    }

    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("[WIFI] Connected.");
      printLCD(0, "WiFi Connected");
      printLCD(1, "");

      return true;
    }

    WiFi.disconnect(); // 실패한 경우 초기화
    delay(500);
  }

  Serial.println("[WIFI] Connection failed.");
  printLCD(0, "WiFi Failed");
  printLCD(1, "");

  lastWiFiRetryTime = millis();

  return false;
}


// 연결이 끊긴 경우 10초마다 재연결을 시도
void reconnectWiFi(){
  if (WiFi.status() == WL_CONNECTED) return;
  
  unsigned long currentTime = millis();

  if (currentTime - lastWiFiRetryTime < WIFI_RETRY_INTERVAL) return;

  lastWiFiRetryTime = currentTime;

  Serial.println("[WIFI] Reconnecting...");

  WiFi.disconnect();
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
}


// ============================================
// MQTT 연결 함수
// ============================================
void connectMQTT() {
  if (WiFi.status() != WL_CONNECTED) return;
  if (mqttClient.connected()) return;

  unsigned long currentTime = millis();

  if (lastMQTTRetryTime != 0 && currentTime - lastMQTTRetryTime < MQTT_RETRY_INTERVAL) return;

  lastMQTTRetryTime = currentTime;

  const char* MQTT_CLIENT_ID = "IoT-FAN";

  Serial.println("[MQTT] Connecting...");

  if (mqttClient.connect(MQTT_CLIENT_ID)) {
    Serial.println("[MQTT] Connected.");

    bool subscribed = mqttClient.subscribe(TOPIC_FAN_COMMAND);

    if (subscribed) {
      Serial.print("[MQTT] Subscribed : ");
      Serial.println(TOPIC_FAN_COMMAND);
    } else {
      Serial.print("[MQTT] Subscribe failed : ");
      Serial.println(TOPIC_FAN_COMMAND);
    }

    publishFanState();
  } else {
    Serial.print("[MQTT] Connection failed. State: ");
    Serial.println(mqttClient.state());
  }
}


// ============================================
// 초기화 함수
// ============================================
void setup(){
  Serial.begin(115200); 
  co2Serial.begin(9600);

  pinMode(FAN_PIN, OUTPUT);

  currentFanMode = FAN_MODE_AUTO;
  currentFanState = LOW;
  manualFanState = LOW;
  digitalWrite(FAN_PIN, currentFanState);

  dht.begin();
  myMHZ19.begin(co2Serial);

  lcd.init();
  lcd.backlight();

  readAllSensors();
  controlFan();
  updateLCD();
  printCurrentState();

  mqttClient.setServer(MQTT_SERVER, MQTT_PORT);
  mqttClient.setCallback(mqttCallback);

  connectWiFi();

  delay(1000);

  updateLCD();
  
  lastSensorCycleTime = millis();
}


// ============================================
// 메인 반복 함수
// ============================================
void loop(){
  reconnectWiFi();
  connectMQTT();

  if (mqttClient.connected()) mqttClient.loop();

  unsigned long currentTime = millis();

  if (currentTime - lastSensorCycleTime >= SENSOR_INTERVAL) {
    runSensorCycle();
    
    lastSensorCycleTime = currentTime;
  }
}