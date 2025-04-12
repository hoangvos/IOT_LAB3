#include <Adafruit_Sensor.h>
#include <WiFi.h>
#include <Arduino_MQTT_Client.h>
#include <ThingsBoard.h>
#include <DHT20.h> // Use the DHT20 library
#include <Wire.h>  // I2C library for DHT20
#include <ArduinoOTA.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <Server_Side_RPC.h>
#include <Shared_Attribute_Update.h>
#include <OTA_Firmware_Update.h>
#include <Espressif_Updater.h>
#include <Preferences.h>
#include <DHT.h>

// Configuration
#define LED_PIN 5
#define DHTPIN 4
#define DHTTYPE DHT22

constexpr char WIFI_SSID[] = "Hoang";
constexpr char WIFI_PASSWORD[] = "15220903";
constexpr char TOKEN[] = "69o42qcjtwnwdwnsgomy";
constexpr char THINGSBOARD_SERVER[] = "app.coreiot.io";
constexpr uint16_t THINGSBOARD_PORT = 1883U;
constexpr uint32_t MAX_MESSAGE_SIZE = 1024U;
constexpr uint32_t SERIAL_DEBUG_BAUD = 115200U;
constexpr uint16_t MAX_MESSAGE_SEND_SIZE = 256U;
constexpr uint16_t MAX_MESSAGE_RECEIVE_SIZE = 256U;
constexpr int16_t telemetrySendInterval = 5000U; // 10 seconds

Preferences preferences;
String currentFirmwareVersion;

constexpr char CURRENT_FIRMWARE_TITLE[] = "ESP32_update_firmware";
constexpr char DEFAULT_FIRMWARE_VERSION[] = "1.0";
constexpr uint8_t FIRMWARE_FAILURE_RETRIES = 12U;
constexpr uint16_t FIRMWARE_PACKET_SIZE = 4096U;


uint32_t previousDataSend;
// Wi-Fi and ThingsBoard clients
WiFiClient wifiClient;
Arduino_MQTT_Client mqttClient(wifiClient);

constexpr uint8_t MAX_RPC_SUBSCRIPTIONS = 3U;
constexpr uint8_t MAX_RPC_RESPONSE = 5U;
constexpr uint64_t REQUEST_TIMEOUT_MICROSECONDS = 50000000U;


Server_Side_RPC<MAX_RPC_SUBSCRIPTIONS, MAX_RPC_RESPONSE> rpc;

constexpr size_t MAX_ATTRIBUTES = 6U;

Shared_Attribute_Update<1U, MAX_ATTRIBUTES> shared_update;
Attribute_Request<2U, MAX_ATTRIBUTES> attr_request;

OTA_Firmware_Update<> ota;


const std::array<IAPI_Implementation*, 4U> apis = {
    &rpc,
    &shared_update,
    &ota,
    &attr_request
};
ThingsBoard tb(mqttClient, MAX_MESSAGE_RECEIVE_SIZE, MAX_MESSAGE_SEND_SIZE, Default_Max_Stack_Size, apis);

Espressif_Updater<> updater;
bool currentFWSent = false;
bool updateRequestSent = false;
// Statuses for requesting of attributes
bool requestedClient = false;
bool requestedShared = false;



// DHT20 sensor
DHT20 dht20; // Create an instance of the DHT20 sensor
DHT dht(DHTPIN, DHTTYPE);  
// Shared attributes
constexpr char BLINKING_INTERVAL_ATTR[] = "blinkingInterval";
constexpr char LED_STATE_ATTR[] = "ledState";
volatile bool attributesChanged = false;
volatile bool ledState = false;
constexpr std::array<const char *, 2U> SHARED_ATTRIBUTES_LIST = {
  LED_STATE_ATTR,
  BLINKING_INTERVAL_ATTR};


// RPC Callback for LED control
// RPC_Response getLedSwitchState(const RPC_Data &data)
// {
//   Serial.println("Received Switch state");
//   bool newState = data;
//   Serial.println(newState);
//   return RPC_Response("getLedSwitchState", newState);
// }
// RPC_Response setLedSwitchState(const RPC_Data &data)
// {
//   Serial.println("Received Switch state");
//   bool newState = data;
//   Serial.print("Switch state change: ");
//   Serial.println(newState);
//   digitalWrite(LED_PIN, newState);
//   attributesChanged = true;
//   return RPC_Response("setLedSwitchValue", newState);
// }
void processValidationSuccess(const JsonVariantConst &data, JsonDocument &response) {
  // Serial.println("Received validation success request");
  bool isValid = data["isValid"].as<bool>();
  // Serial.print("Validation Success: ");
  // Serial.println(isValid);
  response.set(true);
}
// Register both RPC callbacks
void processValidationFailure(const JsonVariantConst &data, JsonDocument &response) {
  // Serial.println("Received validation failure request");
  bool isValid = data["isValid"].as<bool>();
  // Serial.print("Validation Failure: ");
  // Serial.println(isValid);
  response.set(false);
}
const std::array<RPC_Callback, 2U> callbacks = {
    // RPC_Callback{"getLedSwitchValue", setLedSwitchState},
    // RPC_Callback{"setLedSwitchValue", setLedSwitchState},
    RPC_Callback{"validationSuccess", processValidationSuccess},
    RPC_Callback{"validationFailure", processValidationFailure}
};



// New Shared attribute callback using updated library format
void processSharedAttributeUpdate(const JsonObjectConst &data) {
  for (auto it = data.begin(); it != data.end(); ++it) {
    const char* attributeName = it->key().c_str();
    
    if (strcmp(attributeName, BLINKING_INTERVAL_ATTR) == 0) {
      const uint16_t new_interval = it->value().as<uint16_t>();
      if (new_interval >= 10U && new_interval <= 60000U) {
        Serial.print("Blinking interval is set to: ");
        Serial.println(new_interval);
      }
    }
    else if (strcmp(attributeName, LED_STATE_ATTR) == 0) {
      ledState = it->value().as<bool>();
      digitalWrite(LED_PIN, ledState);
      Serial.print("LED state is set to: ");
      Serial.println(ledState);
    }
  }
  attributesChanged = true;
}


const Shared_Attribute_Callback<MAX_ATTRIBUTES> attributes_callback(&processSharedAttributeUpdate, SHARED_ATTRIBUTES_LIST);

void requestTimedOut() {
  Serial.printf("Attribute request timed out did not receive a response in (%llu) microseconds. Ensure client is connected to the MQTT broker and that the keys actually exist on the target device\n", REQUEST_TIMEOUT_MICROSECONDS);
}
void processSharedAttributeRequest(const JsonObjectConst &data) {
  for (auto it = data.begin(); it != data.end(); ++it) {
    Serial.println(it->key().c_str());
    // Shared attributes have to be parsed by their type.
    Serial.println(it->value().as<const char*>());
    if (strcmp(it->key().c_str(), FW_VER_KEY) == 0) {
      const char* newVersion = it->value().as<const char*>();
      
      // Save the version to preferences
      preferences.putString("version", newVersion);
      
      Serial.print("Saved firmware version to preferences: ");
      Serial.println(newVersion);
    }
  }

  const size_t jsonSize = Helper::Measure_Json(data);
  char buffer[jsonSize];
  serializeJson(data, buffer, jsonSize);
  Serial.println(buffer);
}

constexpr std::array<const char*, MAX_ATTRIBUTES> REQUESTED_SHARED_ATTRIBUTES = {FW_VER_KEY};
const Attribute_Request_Callback<MAX_ATTRIBUTES> sharedCallback(&processSharedAttributeRequest, REQUEST_TIMEOUT_MICROSECONDS, &requestTimedOut, REQUESTED_SHARED_ATTRIBUTES);


// Wi-Fi initialization
void InitWifi()
{
  Serial.print("Connecting to AP ... ");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED)
  {
    delay(500);
    Serial.print(".");
  }
  Serial.println("Connected to AP");
}

bool reconnect()
{
  if (WiFi.status() != WL_CONNECTED)
  {
    InitWifi();
  }
  return WiFi.status() == WL_CONNECTED;
}

// FreeRTOS Tasks
void wifiTask(void *pvParameters)
{
  while (1)
  {
    if (!reconnect())
    {
      vTaskDelay(pdMS_TO_TICKS(1000)); // Wait for 1 second before retrying
    }
    vTaskDelay(pdMS_TO_TICKS(1000)); // Check every second
  }
}

void thingsboardTask(void *pvParameters)
{
  while (1)
  {
    if (!tb.connected())
    {
      Serial.print("Connecting to: ");
      Serial.print(THINGSBOARD_SERVER);
      Serial.print(" with token ");
      Serial.println(TOKEN);
      if (!tb.connect(THINGSBOARD_SERVER, TOKEN, THINGSBOARD_PORT))
      {
        Serial.println("Failed to connect.");
        vTaskDelay(pdMS_TO_TICKS(5000)); // Wait for 5 seconds before retrying
        continue;
      }
      tb.sendAttributeData("macAddress", WiFi.macAddress().c_str());
      // Serial.println("Subscribing for RPC...");
      // if (!rpc.RPC_Subscribe(callbacks.cbegin(), callbacks.end()))
      // {
      //   Serial.println("Failed to subscribe for RPC");
      //   vTaskDelay(pdMS_TO_TICKS(5000)); // Wait for 5 seconds before retrying
      //   continue;
      // }
      if (!shared_update.Shared_Attributes_Subscribe(attributes_callback))
      {
        Serial.println("Failed to subscribe for shared attribute updates");
        vTaskDelay(pdMS_TO_TICKS(5000)); // Wait for 5 seconds before retrying
        continue;
      }
    }
    tb.loop();
    vTaskDelay(pdMS_TO_TICKS(100)); // Run every 100ms
  }
}
void requestedSharedAttrTask(void *pvParameters){
  while (1)
  {
    if (!requestedShared) {
      Serial.println("Requesting shared attributes...");
      // Shared attributes we want to request from the server
      requestedShared = attr_request.Shared_Attributes_Request(sharedCallback);
      if (!requestedShared) {
        Serial.println("Failed to request shared attributes");
      }
    }

    vTaskDelay(pdMS_TO_TICKS(20000));
  }

}

void telemetryTask(void *pvParameters)
{
  while (1)
  {
    if (millis() - previousDataSend > telemetrySendInterval)
    {
      // previousDataSend = millis();
      // int status = dht20.read(); // Read data from DHT20
      // if (status == DHT20_OK)
      // {
      //   float temperature = dht20.getTemperature(); // Get temperature
      //   float humidity = dht20.getHumidity();       // Get humidity
      //   Serial.print("Temperature: ");
      //   Serial.print(temperature);
      //   Serial.print("°C  |  Humidity: ");
      //   Serial.print(humidity);
      //   Serial.println("%");
      //   tb.sendTelemetryData("temperature", temperature);
      //   tb.sendTelemetryData("humidity", humidity);
      // }
      // else
      // {
      //   Serial.println("Failed to read from DHT20 sensor!");
      // }
      previousDataSend = millis();
      
      float temperature = dht.readTemperature(); // Đọc nhiệt độ (°C)
      float humidity = dht.readHumidity();       // Đọc độ ẩm (%)

      if (!isnan(temperature) && !isnan(humidity)) // Kiểm tra dữ liệu hợp lệ
      {
        // Serial.print("Temperature: ");
        // Serial.print(temperature);
        // Serial.print("°C  |  Humidity: ");
        // Serial.print(humidity);
        // Serial.println("%");

        StaticJsonDocument<256> jsonBuffer;
        jsonBuffer["temperature"] = temperature;
        jsonBuffer["humidity"] = humidity;

        // Send telemetry data
        tb.sendTelemetryJson(jsonBuffer, measureJson(jsonBuffer));
      }
      else
      {
        Serial.println("⚠️ Error: Failed to read from DHT22 sensor!");
      }
    }
    vTaskDelay(pdMS_TO_TICKS(1000)); // Run every second
  }
}

void attributesTask(void *pvParameters)
{
  while (1)
  {
    if (attributesChanged)
    {
      attributesChanged = false;
      tb.sendAttributeData(LED_STATE_ATTR, digitalRead(LED_PIN));
    }
    tb.sendAttributeData("rssi", WiFi.RSSI());
    tb.sendAttributeData("channel", WiFi.channel());
    tb.sendAttributeData("bssid", WiFi.BSSIDstr().c_str());
    tb.sendAttributeData("localIp", WiFi.localIP().toString().c_str());
    tb.sendAttributeData("ssid", WiFi.SSID().c_str());
    vTaskDelay(pdMS_TO_TICKS(1000)); // Run every second
  }
}
void finished_callback(const bool & success) {
  if (success) {
    Serial.println("Update successful, saving new version and rebooting");
    // String newVersion = "1.1";
    // preferences.putString("version", newVersion);
    preferences.end(); 

    delay(1000);

    esp_restart();
    return;
  }
  Serial.println("Downloading firmware failed");
}
void progress_callback(const size_t & current, const size_t & total) {
  Serial.printf("Progress %.2f%%\n", static_cast<float>(current * 100U) / total);
}
void update_starting_callback() {
  // Nothing to do
}
void firmwareUpdateTask(void *pvParameters) {
  while (1) {
    if (tb.connected()) {
      // Send current firmware info to ThingsBoard
      if (!currentFWSent) {
        currentFWSent = ota.Firmware_Send_Info(CURRENT_FIRMWARE_TITLE, DEFAULT_FIRMWARE_VERSION);
        if (currentFWSent) {
          Serial.println("Current firmware info sent to ThingsBoard");
        }
      }
      
      // Request firmware update if not already requested
      if (!updateRequestSent) {   
        const OTA_Update_Callback ota_callback(
          CURRENT_FIRMWARE_TITLE, 
          currentFirmwareVersion.c_str(), 
          &updater,
          &finished_callback, 
          &progress_callback, 
          &update_starting_callback, 
          FIRMWARE_FAILURE_RETRIES, 
          FIRMWARE_PACKET_SIZE
        );     
        updateRequestSent = ota.Start_Firmware_Update(ota_callback);
        if (updateRequestSent) {
          Serial.println("Firmware update request sent");
        } else {
          Serial.println("Failed to send firmware update request");
        }
      }
    } 
    vTaskDelay(pdMS_TO_TICKS(15000)); // Check every 30 seconds
  }
}

void setup()
{
  Serial.begin(SERIAL_DEBUG_BAUD);
  pinMode(LED_PIN, OUTPUT);
  delay(1000);

  // Initialize I2C and DHT20 sensor
  Wire.begin();
  dht20.begin();
  dht.begin();
  preferences.begin("firmware", false); 
  currentFirmwareVersion = preferences.getString("version", DEFAULT_FIRMWARE_VERSION);
  Serial.print("Current firmware version: ");
  Serial.println(currentFirmwareVersion);

  // Create FreeRTOS tasks
  xTaskCreate(wifiTask, "WifiTask", 4096, NULL, 1, NULL);
  xTaskCreate(thingsboardTask, "ThingsBoardTask", 8192, NULL, 2, NULL);
  xTaskCreate(telemetryTask, "TelemetryTask", 4096, NULL, 3, NULL);
  xTaskCreate(attributesTask, "AttributesTask", 4096, NULL, 4, NULL);
  xTaskCreate(firmwareUpdateTask, "FirmwareUpdateTask", 8192, NULL, 5, NULL);
  xTaskCreate(requestedSharedAttrTask, "RequestedSharedAttrTask", 4096, NULL, 5, NULL);
}

void loop()
{
  // Empty loop as everything is handled by FreeRTOS tasks
}