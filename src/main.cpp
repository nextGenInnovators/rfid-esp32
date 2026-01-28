#include <MFRC522v2.h>
#include <MFRC522DriverSPI.h>
//#include <MFRC522DriverI2C.h>
#include <MFRC522DriverPinSimple.h>
#include <MFRC522Debug.h>
#include "WiFi.h"
#include <HTTPClient.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "RTClib.h"
#include <Preferences.h>
#include <time.h>


#define LED_BUILTIN 2
#define SCREEN_WIDTH 128 // OLED display width, in pixels
#define SCREEN_HEIGHT 64 // OLED display height, in pixels
#define BUZZER_PIN 25

// Declaration for an SSD1306 display connected to I2C (SDA, SCL pins)
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// RTC module
RTC_DS3231 rtc;

Preferences preferences;

static bool retried = false;
static bool wifiWasConnected = false;
static bool serverWasReachable = false;
static bool rtcInitialized = false;

// WiFi credentials
// const char* ssid = "DESKTOP-6DO970B 1565";
// const char* password = "mbeuwifi";
const char* ssid = "VC-1012-9086";
const char* password = "Jbj52fjnH";
// const char* ssid = "OPPO A78 s9jt";
// const char* password = "efin9598";

// Server URLs
const char* serverPostUrl = "http://192.168.18.106:8080/api/v1/attendance/addAttendance"; 
const char* serverGetUrl = "http://192.168.18.106:8080/api/v1/health/"; 

const char* roomName = "Lecture 4"; 

// Time zone offset in hours (South Africa is UTC+2)
const int timeZoneOffsetHours = 2;

// Learn more about using SPI/I2C or check the pin assigment for your board: https://github.com/OSSLibraries/Arduino_MFRC522v2#pin-layout
MFRC522DriverPinSimple ss_pin(5);

MFRC522DriverSPI driver{ss_pin}; // Create SPI driver
//MFRC522DriverI2C driver{};     // Create I2C driver
MFRC522 mfrc522{driver};         // Create MFRC522 instance

MFRC522::MIFARE_Key key;

byte studentNumberBlockAddress = 1;
byte studentNameBlockAddress = 2;
// byte newBlockData[17] = {"402308195       "};
// byte newBlockData[16] = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};   // CLEAR DATA
byte bufferblocksize = 18;
byte studentNumberBlockDataRead[18];
byte studentNameBlockDataRead[18];

// #############Function Declarations##################
bool connectToWiFi();
void makeHttpGetRequest();
String urlencode(String str);
void makeHttpPostRequest(const String& studentNumber, const String& roomName);
void listenForTags();
void readFromBlock(byte blockAddress, byte* blockDataRead, byte bufferBlockSize);
void writeToBlock(byte blockAddress, byte* newBlockData);
void blinkBuiltInLED();
void useBuzzer();
String trimString(String str);
void showTextOnDisplayReplace(String text, int textSize, bool clearDisplay);
String turnByteToString(byte* byte);
String shortenStringToFitScreen(String text);
String getOffsetDateTimeString();
void saveFailedPost(const String& studentNumber, const String& roomName, const String& timestamp);
void retryFailedPosts();
bool isServerReachable();
void clearAllFailedPosts();
void initializeRTC();
void attemptWiFiReconnection();
void setRTCTime(int year, int month, int day, int hour, int minute, int second);
void printRTCStatus();
void syncTimeFromNTP();
// ############################################

void setup() {
  Serial.begin(115200);  // Initialize serial communication
  while (!Serial);       // Do nothing if no serial port is opened
  
  pinMode(LED_BUILTIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, HIGH);
  
  mfrc522.PCD_Init();    // Init MFRC522 board.
  Serial.println("RFID Attendance System Starting...");
  Serial.println("Scan a card/tag...");
 
  // Prepare key - all keys are set to FFFFFFFFFFFF at chip delivery from the factory.
  for (byte i = 0; i < 6; i++) {
    key.keyByte[i] = 0xFF;
  }

  // Initialize display first
  if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) { // Address 0x3D for 128x64
    Serial.println(F("SSD1306 allocation failed"));
    for(;;);
  }
  delay(1000);
  showTextOnDisplayReplace("Starting...", 2, true);

  // Initialize RTC
  initializeRTC();
  printRTCStatus();

  // Initialize preferences
  preferences.begin("failed_posts", false);
  int savedCount = preferences.getInt("count", 0);
  preferences.end();
  
  Serial.print("Found ");
  Serial.print(savedCount);
  Serial.println(" saved posts in flash memory");

  // Try to connect to WiFi with timeout
  Serial.println("Attempting to connect to WiFi...");
  showTextOnDisplayReplace("Connecting\nWiFi...", 2, true);
  
  if (connectToWiFi()) {
    wifiWasConnected = true;
    Serial.println("WiFi connected successfully!");
    showTextOnDisplayReplace("WiFi OK!", 2, true);
    delay(1000);
    
    // Sync time from NTP server
    syncTimeFromNTP();
    
  } else {
    wifiWasConnected = false;
    Serial.println("WiFi connection failed. Proceeding in OFFLINE mode.");
    Serial.println("System will save all scans to flash memory.");
    showTextOnDisplayReplace("OFFLINE\nMode", 2, true);
    delay(2000);
  }

  // --- Add WiFi event handlers ---
  WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info){
    switch(event) {
      case WIFI_EVENT_STA_CONNECTED:
        Serial.println("Wi-Fi connected to AP");
        break;

      case WIFI_EVENT_STA_DISCONNECTED:
        Serial.println("Wi-Fi disconnected from AP");
        retried = false;
        wifiWasConnected = false;
        serverWasReachable = false;
        
        // Attempt to reconnect
        Serial.println("Will attempt to reconnect to WiFi...");
        break;

      case IP_EVENT_STA_GOT_IP:
        Serial.print("Wi-Fi got IP: ");
        Serial.println(WiFi.localIP());
        wifiWasConnected = true;
        retried = false;
        break;

      default:
        break;
    }
  });

  showTextOnDisplayReplace("Scan card/\ntag...", 2, true);
  Serial.println("\n=== System Ready ===");
  Serial.println("Waiting for RFID cards...\n");
}

void loop() {
  listenForTags();

  // Periodic connectivity and retry check every 10 seconds
  static unsigned long lastCheck = 0;
  if (millis() - lastCheck > 10000) {
    lastCheck = millis();

    // Check WiFi status
    if (WiFi.status() == WL_CONNECTED) {
        if (!wifiWasConnected) {
            Serial.println("WiFi reconnected!");
            wifiWasConnected = true;
            retried = false;
            
            // Sync time when WiFi reconnects
            syncTimeFromNTP();
        }

        // Check server reachability
        bool serverReachable = isServerReachable();
        
        if (serverReachable) {
            if (!serverWasReachable) {
                Serial.println("Server became reachable!");
                serverWasReachable = true;
            }
            
            // Retry failed posts if we haven't already
            if (!retried) {
                Serial.println("Server reachable, retrying failed posts...");
                retryFailedPosts();
                retried = true;
            }
        } else {
            if (serverWasReachable) {
                Serial.println("Server became unreachable (internet lost)!");
            }
            serverWasReachable = false;
            retried = false;
        }
    } else {
        if (wifiWasConnected) {
            Serial.println("Wi-Fi connection lost! Switching to OFFLINE mode.");
            wifiWasConnected = false;
        }
        serverWasReachable = false;
        retried = false;
        
        // Attempt to reconnect WiFi
        attemptWiFiReconnection();
    }
  }
}

void listenForTags() {
  if (!mfrc522.PICC_IsNewCardPresent() || !mfrc522.PICC_ReadCardSerial()) {
    delay(500);
    return;
  }

  Serial.print("----------------\nCard UID: ");
  // MFRC522Debug::PrintUID(Serial, (mfrc522.uid)); // use in arduino IDE
  MFRC522Debug::PICC_DumpDetailsToSerial(mfrc522, Serial, &(mfrc522.uid)); // use in PlatformIO
  Serial.println();

  readFromBlock(studentNumberBlockAddress, studentNumberBlockDataRead, bufferblocksize);
  readFromBlock(studentNameBlockAddress, studentNameBlockDataRead, bufferblocksize);

  String studentNumber = trimString(shortenStringToFitScreen(turnByteToString(studentNumberBlockDataRead)));
  String studentName = trimString(shortenStringToFitScreen(turnByteToString(studentNameBlockDataRead)));

  String displayText = studentNumber + "\n" + studentName;
  showTextOnDisplayReplace(displayText, 2, true);
  useBuzzer();

  // Example of writing new data
  // writeToBlock(studentNumberBlockAddress, newBlockData);

  makeHttpPostRequest(studentNumber, roomName);

  mfrc522.PICC_HaltA();
  mfrc522.PCD_StopCrypto1();

  delay(2000);
  showTextOnDisplayReplace("Scan card/\ntag...", 2, true);
}

bool connectToWiFi() {
  WiFi.mode(WIFI_STA); 
  WiFi.begin(ssid, password);
  
  Serial.println("Connecting to WiFi...");
  unsigned long startAttemptTime = millis();

  // Wait up to 15 seconds for connection
  while (WiFi.status() != WL_CONNECTED && millis() - startAttemptTime < 15000) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Failed to connect to WiFi!");
    return false;
  }

  Serial.println("WiFi Connected!");
  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP());
  return true;
}

void attemptWiFiReconnection() {
  static unsigned long lastReconnectAttempt = 0;
  
  // Only attempt reconnection every 30 seconds to avoid spam
  if (millis() - lastReconnectAttempt > 30000) {
    lastReconnectAttempt = millis();
    
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("Attempting WiFi reconnection...");
      WiFi.reconnect();
    }
  }
}

void initializeRTC() {
  Serial.println("Initializing RTC module...");
  showTextOnDisplayReplace("Init RTC...", 2, true);
  
  if (!rtc.begin()) {
    Serial.println("Couldn't find RTC module!");
    showTextOnDisplayReplace("RTC ERROR!", 2, true);
    Serial.flush();
    delay(2000);
    return;
  }

  if (rtc.lostPower()) {
    Serial.println("WARNING: RTC lost power!");
    Serial.println("Time will be synced from NTP when WiFi connects");
    showTextOnDisplayReplace("RTC power\nlost!\nWill sync", 2, true);
    delay(2000);
    
    // Set a default time for now (will be corrected by NTP)
    rtc.adjust(DateTime(2024, 1, 1, 0, 0, 0));
  }

  rtcInitialized = true;
  Serial.println("RTC initialized successfully!");
  showTextOnDisplayReplace("RTC OK!", 2, true);
  Serial.println("Current RTC time: " + getOffsetDateTimeString());
  delay(1000);
}

void syncTimeFromNTP() {
  if (WiFi.status() == WL_CONNECTED && rtcInitialized) {
    Serial.println("Syncing time from NTP server...");
    showTextOnDisplayReplace("Syncing\ntime...", 2, true);
    
    // Configure NTP with your timezone (UTC+2 = 7200 seconds, no DST = 0)
    configTime(7200, 0, "pool.ntp.org", "time.nist.gov");
    
    struct tm timeinfo;
    if (getLocalTime(&timeinfo, 10000)) { // 10 second timeout
      // Set RTC to the time from NTP
      rtc.adjust(DateTime(
        timeinfo.tm_year + 1900,
        timeinfo.tm_mon + 1,
        timeinfo.tm_mday,
        timeinfo.tm_hour,
        timeinfo.tm_min,
        timeinfo.tm_sec
      ));
      
      Serial.println("✓ RTC time synced from NTP!");
      Serial.println("New time: " + getOffsetDateTimeString());
      showTextOnDisplayReplace("Time\nSynced!", 2, true);
      delay(1500);
    } else {
      Serial.println("✗ Failed to get time from NTP");
      showTextOnDisplayReplace("NTP sync\nfailed", 2, true);
      delay(1500);
    }
  } else {
    Serial.println("Cannot sync time - WiFi not connected or RTC not initialized");
  }
}

void makeHttpGetRequest() {
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    http.begin(serverGetUrl);
    int httpCode = http.GET();
    if (httpCode > 0) {
      String payload = http.getString();
      Serial.print("HTTP Response Code: ");
      Serial.println(httpCode);
      Serial.print("Response: ");
      Serial.println(payload);
    } else {
      Serial.print("GET request failed. Error: ");
      Serial.println(http.errorToString(httpCode).c_str());
    }
    http.end();
  }
}

void makeHttpPostRequest(const String& studentNumber, const String& roomName) {
   Serial.println("Hit makeHttpPostRequest");
    if (WiFi.status() == WL_CONNECTED) {
       Serial.println("WiFi is connected for API request");
        HTTPClient http;

        String timestamp = getOffsetDateTimeString();
        String urlWithParams = String(serverPostUrl) + 
                               "?studentNumber=" + urlencode(studentNumber) + 
                               "&roomName=" + urlencode(roomName) + 
                               "&timestamp=" + urlencode(timestamp);
        Serial.println(urlWithParams);
        http.begin(urlWithParams);
        http.setTimeout(5000); // 5 second timeout

        int httpCode = http.POST("");  // Send POST request with empty body
        String payload = http.getString();

        if(payload.length() > 0) {
          showTextOnDisplayReplace(payload, 1, true);
          delay(2000);
        }

        if (httpCode > 0 && httpCode == 200) {
            Serial.print("HTTP Response code: ");
            Serial.println(httpCode);
            Serial.print("Response: ");
            Serial.println(payload);
        } else {
            Serial.print("POST request failed. Error: ");
            Serial.println(http.errorToString(httpCode).c_str());

            // Only save failed post if payload does NOT contain known exceptions
            if(payload != "Attendance already recorded for this student within the last 30 minutes." &&
               payload != "Invalid student number." &&
               payload.indexOf("User not found for student number") < 0 &&
               payload != "Unable to return student projected data." &&
               payload != "Room not found in database.") 
            { 
              saveFailedPost(studentNumber, roomName, timestamp); 
            }
        }

        http.end();
    } else {
        Serial.println("WiFi not connected - saving to flash memory");
        showTextOnDisplayReplace("Saved to\nflash", 2, true);
        delay(1500);
        saveFailedPost(studentNumber, roomName, getOffsetDateTimeString());
    }
}

bool isServerReachable() {
    if (WiFi.status() != WL_CONNECTED) return false;

    HTTPClient http;
    http.begin(serverGetUrl);
    http.setTimeout(5000); // 5 second timeout
    int httpCode = http.GET();
    http.end();

    return (httpCode == 200);
}

String urlencode(String str) {
  String encoded = "";
  char c, code1, code2;
  for (int i = 0; i < str.length(); i++) {
    c = str.charAt(i);
    if (isalnum(c)) {
      encoded += c;
    } else {
      encoded += '%';
      code1 = (c >> 4) & 0xF;
      code1 += (code1 > 9) ? 'A' - 10 : '0';
      code2 = c & 0xF;
      code2 += (code2 > 9) ? 'A' - 10 : '0';
      encoded += code1;
      encoded += code2;
    }
  }
  return encoded;
}

String trimString(String str) {
  int start = 0;
  while (start < str.length() && isspace(str.charAt(start))) start++;
  int end = str.length() - 1;
  while (end >= 0 && isspace(str.charAt(end))) end--;
  if (end < start) return "";
  return str.substring(start, end + 1);
}

void readFromBlock(byte blockAddress, byte* blockDataRead, byte bufferBlockSize = 18) {
  //##############################
  //In RFID communication with MIFARE Classic cards, authentication must be performed before reading or writing data blocks. There are two keys per sector on a MIFARE Classic card:
  // 1. Key A
  // 2. Key B
  // These keys control access to the data in that sector.
  // 0x60 -> authenticate with Key A
  // 0x61 -> authenticate with Key B
  //#############################
  // Authenticate the specified block using KEY_A = 0x60
  if (mfrc522.PCD_Authenticate(0x60, blockAddress, &key, &(mfrc522.uid)) != 0) { 
    Serial.println("Authentication failed.");
    return;
  }
  if (mfrc522.MIFARE_Read(blockAddress, blockDataRead, &bufferBlockSize) != 0) {
    Serial.println("Read failed.");
  } else {
    Serial.println("Read successfully!");
    Serial.print("Data in block ");
    Serial.print(blockAddress);
    Serial.print(": ");
    for (byte i = 0; i < 16; i++) {
      Serial.print((char)blockDataRead[i]);
    }
    Serial.println();
    blinkBuiltInLED();
  }
}

void writeToBlock(byte blockAddress, byte* newBlockData) {
  // Authenticate the specified block using KEY_A = 0x60
  if (mfrc522.PCD_Authenticate(0x60, blockAddress, &key, &(mfrc522.uid)) != 0) {
    Serial.println("Authentication failed.");
    return;
  }

  // Write data to the specified block
  if (mfrc522.MIFARE_Write(blockAddress, newBlockData, 16) != 0) {
    Serial.println("Write failed.");
  } else {
    Serial.print("Data written successfully in block: ");
    Serial.println(blockAddress);
    showTextOnDisplayReplace("Data written successfully in block: " + String(blockAddress), 1, true);
  }
}

void blinkBuiltInLED() {
  digitalWrite(LED_BUILTIN, HIGH); // LED ON
  delay(300);
  digitalWrite(LED_BUILTIN, LOW);  // LED OFF
}

void useBuzzer() {
  digitalWrite(BUZZER_PIN, LOW); // Buzzer on
  delay(100);
  digitalWrite(BUZZER_PIN, HIGH);  // Buzzer off
  delay(100);
}

void showTextOnDisplayReplace(String text, int textSize, bool clearDisplay) {
  if(clearDisplay) display.clearDisplay();
  display.setTextSize(textSize);
  display.setTextColor(WHITE);
  display.setCursor(0, 10);
  display.println(text);
  display.display(); 
}

String turnByteToString(byte* byte) {
  String text = "";
  for (int i = 0; i < 16; i++) {
    if (byte[i] == 0) break;
    text += (char)byte[i];
  }
  return text;
}

String shortenStringToFitScreen(String text) {
  if (text.length() > 10) return text.substring(0, 10);
  return text;
}

String getOffsetDateTimeString() {
  if (!rtcInitialized) {
    return "RTC_NOT_INITIALIZED";
  }

  DateTime now = rtc.now();
  
  // The RTC now stores local time (UTC+2) after NTP sync
  // No need to add offset since NTP already gave us local time
  
  char isoTime[30];
  sprintf(isoTime, "%04d-%02d-%02dT%02d:%02d:%02d+02:00",
      now.year(),
      now.month(),
      now.day(),
      now.hour(),
      now.minute(),
      now.second()
  );

  return String(isoTime);
}

void saveFailedPost(const String& studentNumber, const String& roomName, const String& timestamp) {
    Serial.println("Saving failed post to flash memory");

    preferences.begin("failed_posts", false);

    // Get the current counter
    int counter = preferences.getInt("counter", 0);
    
    // Increment counter for next post
    counter++;
    preferences.putInt("counter", counter);

    // Use simple numeric key (e.g., "p1", "p2", "p3"...)
    String key = "p" + String(counter);
    String value = studentNumber + "," + roomName + "," + timestamp;

    // Save the failed post
    preferences.putString(key.c_str(), value);

    // Update the count of failed posts
    int failedCount = preferences.getInt("count", 0);
    failedCount++;
    preferences.putInt("count", failedCount);

    Serial.println("Saved failed post with key: " + key);
    Serial.println("Value: " + value);
    Serial.println("Total failed posts in flash: " + String(failedCount));

    preferences.end();
}

void retryFailedPosts() {
    preferences.begin("failed_posts", false);  
    Serial.println("=== Starting retry of failed posts ===");

    int failedCount = preferences.getInt("count", 0);
    int counter = preferences.getInt("counter", 0);
    
    Serial.println("Failed posts count: " + String(failedCount));
    Serial.println("Counter: " + String(counter));

    if (failedCount == 0) {
        Serial.println("No failed posts to retry.");
        preferences.end();
        return;
    }

    Serial.println("Retrying failed posts...");
    int successCount = 0;

    // Iterate through all possible keys from 1 to counter
    for (int i = 1; i <= counter; i++) {
        String key = "p" + String(i);
        String value = preferences.getString(key.c_str(), "");

        if (value.length() > 0) {
            int firstComma = value.indexOf(',');
            int secondComma = value.indexOf(',', firstComma + 1);
            String studentNumber = value.substring(0, firstComma);
            String roomName = value.substring(firstComma + 1, secondComma);
            String timestamp = value.substring(secondComma + 1);

            Serial.println("Retrying key " + key + ": " + value);

            // Try to send again
            HTTPClient http;
            String urlWithParams = String(serverPostUrl) + 
                                   "?studentNumber=" + urlencode(studentNumber) + 
                                   "&roomName=" + urlencode(roomName) + 
                                   "&timestamp=" + urlencode(timestamp);
            http.begin(urlWithParams);
            http.setTimeout(5000); // 5 second timeout
            int httpCode = http.POST("");
            String payload = http.getString();
            http.end();

            if (httpCode == 200) {
                Serial.println("✓ Retry successful for key: " + key);
                preferences.remove(key.c_str());
                successCount++;
            } else {
                Serial.println("✗ Retry failed for key: " + key + ", HTTP code: " + String(httpCode));
            }
            
            delay(100); // Small delay between retries
        }
    }

    // Update the count
    int newCount = failedCount - successCount;
    preferences.putInt("count", newCount);

    Serial.println("=== Retry complete ===");
    Serial.println("Successful: " + String(successCount) + ", Remaining: " + String(newCount));

    preferences.end();
}

void clearAllFailedPosts() {
    preferences.begin("failed_posts", false);
    preferences.clear();  // Clears entire namespace
    preferences.end();
    Serial.println("All failed posts cleared from flash memory.");
}

void setRTCTime(int year, int month, int day, int hour, int minute, int second) {
    if (rtcInitialized) {
        rtc.adjust(DateTime(year, month, day, hour, minute, second));
        Serial.println("RTC time manually set to: " + getOffsetDateTimeString());
        showTextOnDisplayReplace("Time\nSet!", 2, true);
        delay(1500);
    } else {
        Serial.println("RTC not initialized - cannot set time");
    }
}

void printRTCStatus() {
    if (!rtcInitialized) {
        Serial.println("RTC Status: NOT INITIALIZED");
        return;
    }
    
    DateTime now = rtc.now();
    Serial.println("=== RTC Status ===");
    Serial.println("RTC Time: " + getOffsetDateTimeString());
    Serial.print("Temperature: ");
    Serial.print(rtc.getTemperature());
    Serial.println("°C");
    Serial.println("RTC Lost Power: " + String(rtc.lostPower() ? "YES" : "NO"));
    Serial.println("================");
}