#include <WiFiS3.h>
#include <PubSubClient.h>
#include "arduino.secrets.h"

// ============================================
// 설정값
// ============================================
const char* WIFI_SSID     = SECRET_WIFI_SSID;
const char* WIFI_PASSWORD = SECRET_WIFI_PASSWORD;
const char* MQTT_SERVER   = SECRET_MQTT_SERVER;
const int   MQTT_PORT     = SECRET_MQTT_PORT;

const char* TOPIC_MOTION = "motion-detect";
const char* TOPIC_LIGHT_STATE = "light/state";

const char* TOPIC_LIGHT_COMMAND = "light/cmd";

#define RADAR_PIN 2
#define LED_PIN   8

// 사람이 감지된 후 LED를 켜기까지의 최소 시간(ms)
const unsigned long MOTION_HOLD_TIME = 2000;

// 사람이 사라진 후 LED를 끄기까지 대기 시간(ms)
const unsigned long NO_MOTION_TIME   = 3000;

const unsigned long WIFI_RETRY_INTERVAL = 10000;
const unsigned long MQTT_RETRY_INTERVAL = 5000;
const unsigned long WIFI_CONNECTION_TIMEOUT = 10000;
const int WIFI_MAX_RETRY_COUNT = 3;


// ============================================
// 조명 제어 모드
// ============================================
enum LightControlMode {
  LIGHT_MODE_AUTO,
  LIGHT_MODE_MANUAL
};


// ============================================
// 객체 생성
// ============================================
WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);


// ============================================
// 현재 재실 감지 상태
// ============================================
bool currentMotionSignal = LOW;
bool currentMotionState = LOW;


// ============================================
// 현재 조명 상태
// ============================================
LightControlMode currentLightMode = LIGHT_MODE_AUTO;

bool currentLightState = LOW;
bool manualLightState = LOW;


// ============================================
// 시간 변수
// ============================================
// 연속 감지 시간을 측정 중인지
bool isMotionTiming = false;

unsigned long motionStartTime = 0;
unsigned long lastMotionTime = 0;

unsigned long lastWiFiRetryTime = 0;
unsigned long lastMQTTRetryTime = 0;


// ============================================
// 조명 상태 변경 함수 
// ============================================
// 실제 LED 상태를 변경함
// ture -> LED 상태가 실제로 변경됨
// false -> 기존 상태와 같아서 변경되지 않음
bool setLightState(bool state) {
  if (currentLightState == state) return false;

  currentLightState = state;

  digitalWrite(
    LED_PIN,
    currentLightState
  );

  Serial.print("[LIGHT] State Changed : "); 
  Serial.println(currentLightState == HIGH ? "ON" : "OFF");

  return true;
}


// ============================================
// MQTT 전송 함수
// ============================================
// 확정된 재실 상태를 전송함
// 1 -> 사람 있음
// 0 -> 사람 없음
bool publishMotionState() {
  if (!mqttClient.connected()) return false;

  int motionValue = currentMotionState == HIGH ? 1 : 0;

  char payload[100];

  snprintf(
    payload,
    sizeof(payload),
    "{\"value\":%d}",
    motionValue
  );

  bool published = mqttClient.publish(TOPIC_MOTION, payload);

  if (published) {
    Serial.print("[MQTT SEND] ");
    Serial.print(TOPIC_MOTION);
    Serial.print(" : ");
    Serial.println(payload);
  } else {
    Serial.print("[MQTT] Publish failed: ");
    Serial.println(TOPIC_MOTION);
  }

  return published;
}

// 현재 조명 제어 모드와 실제 LED 상태를 전송
bool publishLightState() {
  if (!mqttClient.connected()) return false;

  const char* mode = currentLightMode == LIGHT_MODE_AUTO ? "AUTO" : "MANUAL";
  const char* state = currentLightState == HIGH ? "ON" : "OFF";

  char payload[100];

  snprintf(
    payload,
    sizeof(payload),
    "{\"mode\":\"%s\",\"state\":\"%s\"}",
    mode,
    state
  );

  bool published = mqttClient.publish(TOPIC_LIGHT_STATE, payload, true);

  if (published) { 
    Serial.print("[MQTT SEND] "); 
    Serial.print(TOPIC_LIGHT_STATE); 
    Serial.print(" : "); 
    Serial.println(payload); 
  } else { 
    Serial.println( "[MQTT] Light state publish failed." ); 
  }

  return published;
}


// ============================================
// 센서 측정 함수
// ============================================
void readMotionSensor() {
  currentMotionSignal = digitalRead(RADAR_PIN);
}


// ============================================
// LED 제어 함수
// ============================================
// 현재 조명 모드에 따라 LED 적용
// true -> 실제 LED 상태가 변경됨
// false -> LED 상태가 그대로임
bool controlLight() {
  bool targetLightState;

  if (currentLightMode == LIGHT_MODE_MANUAL) {
    targetLightState = manualLightState;
  } else {
    targetLightState = currentMotionState;
  }

  return setLightState(targetLightState);
}


// ============================================
// 재실 상태 처리 함수
// ============================================
void updateMotionState() {
  unsigned long currentTime = millis();

  // 움직임이 감지되고 있는 경우
  if (currentMotionSignal == HIGH) {
    lastMotionTime = currentTime;

    if (!isMotionTiming) {
      isMotionTiming = true;
      motionStartTime = currentTime;
    }

    if (currentMotionState == LOW && currentTime - motionStartTime >= MOTION_HOLD_TIME) {
      currentMotionState = HIGH;

      Serial.println("[MOTION] Occupied");

      publishMotionState();

      if (currentLightMode == LIGHT_MODE_AUTO) {
        bool lightStateChanged = controlLight();

        if (lightStateChanged) publishLightState();
      }
    }
  } else {
    isMotionTiming = false;

    if (currentMotionState == HIGH && currentTime - lastMotionTime >= NO_MOTION_TIME) {
      currentMotionState = LOW;

      Serial.println("[MOTION] Unoccupied");

      publishMotionState();

      if (currentLightMode == LIGHT_MODE_AUTO) {
        bool lightStateChanged = controlLight();

        if (lightStateChanged) publishLightState();
      }
    }
  }
}


// ============================================
// 조명 명령 처리 함수
// ============================================
// 0 -> 수동 모드 LED OFF
// 1 -> 수동 모드 LED ON
// AUTO -> 자동 모드 복귀
void handleLightCommand(const char* cmd) {
  if (cmd == nullptr) return;

  if (strcmp(cmd, "0") == 0) { 
    currentLightMode = LIGHT_MODE_MANUAL; 
    manualLightState = LOW; 
    
    controlLight();
    publishLightState(); 

    Serial.println("[COMMAND] Manual light OFF"); 
  } else if (strcmp(cmd, "1") == 0) {
    currentLightMode = LIGHT_MODE_MANUAL; 
    manualLightState = HIGH; 
    
    controlLight();
    publishLightState(); 

    Serial.println("[COMMAND] Manual light ON"); 
  } else if (strcmp(cmd, "AUTO") == 0) {
    currentLightMode = LIGHT_MODE_AUTO; 

    controlLight();
    publishLightState(); 

    Serial.println("[COMMAND] Auto mode enabled"); 
  } else {
    Serial.println("[COMMAND] Unknown command");
  }
}


// ============================================
// MQTT 메시지 수신 함수
// ============================================
void mqttCallback( char* topic, byte* payload, unsigned int length ) { 
  if ( strcmp(topic, TOPIC_LIGHT_COMMAND) != 0 ) return; 
  
  char command[16]; 
  unsigned int copyLength = length; 

  if (copyLength >= sizeof(command)) copyLength = sizeof(command) - 1;
  memcpy(command, payload, copyLength); 

  command[copyLength] = '\0'; 
  Serial.print("[MQTT] Topic: "); 
  Serial.println(topic); 
  Serial.print("[MQTT] Command: "); 
  Serial.println(command); 

  handleLightCommand(command); 
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

  for (int i = 1; i<=WIFI_MAX_RETRY_COUNT; i++){
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    unsigned long startTime = millis();

    while (WiFi.status() != WL_CONNECTED && millis( )- startTime < WIFI_CONNECTION_TIMEOUT) {
      delay(500);
    }

    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("[WIFI] Connected.");

      return true;
    }

    WiFi.disconnect(); // 실패한 경우 초기화
    delay(500);
  }

  Serial.println("[WIFI] Connection failed.");

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

  const char* MQTT_CLIENT_ID = "IoT-LED";

  Serial.println("[MQTT] Connecting...");

  if (mqttClient.connect(MQTT_CLIENT_ID)) {
    Serial.println("[MQTT] Connected.");

    bool subscribed = mqttClient.subscribe(TOPIC_LIGHT_COMMAND);

    if (subscribed) {
      Serial.print("[MQTT] Subscribed : ");
      Serial.println(TOPIC_LIGHT_COMMAND);
    } else {
      Serial.print("[MQTT] Subscribe failed : ");
      Serial.println(TOPIC_LIGHT_COMMAND);
    }

    publishLightState();
  } else {
    Serial.print("[MQTT] Connection failed. State: ");
    Serial.println(mqttClient.state());
  }
}


// ============================================
// 초기화 함수
// ============================================
void setup() {
  Serial.begin(115200);

  pinMode(RADAR_PIN, INPUT);
  pinMode(LED_PIN, OUTPUT);

  currentMotionSignal = LOW;
  currentMotionState = LOW;

  currentLightMode = LIGHT_MODE_AUTO;
  currentLightState = LOW;
  manualLightState = LOW;

  digitalWrite(LED_PIN, currentLightState);

  mqttClient.setServer(MQTT_SERVER, MQTT_PORT);
  mqttClient.setCallback( mqttCallback );

  connectWiFi();
}


// ============================================
// 메인 반복 함수
// ============================================
void loop() {
  reconnectWiFi();
  connectMQTT();

  if (mqttClient.connected()) mqttClient.loop();

  readMotionSensor();
  updateMotionState();
}
