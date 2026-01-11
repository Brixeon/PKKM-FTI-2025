/*
 * Malingkapok – Versi Multi-User (Hardcode per Perangkat)
 *
 * Fitur:
 * - Kontrol relay via Telegram: /start, /on, /off, /status
 * - Deteksi getaran → baca GPS → kirim lokasi & link Google Maps ke pemilik
 * - 1 bot bisa dipakai banyak produk; tiap produk punya MY_CHAT_ID masing-masing
 * - Pengirim lain selain MY_CHAT_ID akan ditolak
 *
 * Catatan:
 * - Ubah WIFI_SSID, WIFI_PASSWORD, BOT_TOKEN
 * - Ubah DEVICE_NAME dan MY_CHAT_ID per produk
 */

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <UniversalTelegramBot.h>
#include <ArduinoJson.h>
#include <TinyGPSPlus.h>

// ==========> 1) KONFIG JARINGAN & BOT <==========
// GANTI INI SESUAI WIFI & BOT KAMU
const char* WIFI_SSID     = "Rentalkodam";
const char* WIFI_PASSWORD = "fawwaz123";
const char* BOT_TOKEN     = "8454707531:AAGQx9a9qJ01qCsOahM4Gk2jVZWiVcDZ_NI";

// ==========> 2) IDENTITAS PERANGKAT (BEDA PER PRODUK) <==========
// GANTI INI PER PRODUK (UNTUK 3 PRODUK, 3 NILAI BERBEDA)
const char* DEVICE_NAME   = "Motor-saya";  // contoh: "Motor-Andi", "Motor-Budi", dll.
String MY_CHAT_ID         = "5844354400";   // chat_id pemilik (String, bukan int)

// ==========> 3) PIN & PARAMETER HARDWARE <==========
const int RELAY_PIN          = 32;      // IN relay
const bool RELAY_ACTIVE_LOW  = false;   // true jika modul relay aktif-LOW
const int VIB_PIN            = 27;     // DO sensor getar

// GPS pada UART1: RX2=GPIO16, TX2=GPIO17
static const int GPS_RX_PIN = 17;      // ESP32 RX dari TX GPS
static const int GPS_TX_PIN = 16;      // ESP32 TX ke RX GPS
static const uint32_t GPS_BAUD = 9600; // baudrate NEO-6M (umum 9600)

// Anti-noise/anti-spam getaran
const unsigned long DEBOUNCE_MS        = 250;
const unsigned long COOLDOWN_MS        = 60000;   // 60 detik antar laporan
const unsigned long GPS_FIX_TIMEOUT_MS = 15000;   // 15 detik tunggu fix

// Polling Telegram
const unsigned long BOT_MTBS = 1000;   // 1 detik

// ==========> 4) OBJEK GLOBAL <==========
WiFiClientSecure secured_client;     // dipakai UniversalTelegramBot
UniversalTelegramBot bot(BOT_TOKEN, secured_client);
TinyGPSPlus gps;
HardwareSerial GPS_Serial(1);

volatile bool vibTriggered = false;
volatile unsigned long vibLastEdgeMs = 0;
unsigned long lastReportMs = 0;
unsigned long bot_lasttime = 0;

// ==========> 5) UTILITAS RELAY & GETAR <==========
void IRAM_ATTR vibISR() {
  unsigned long now = millis();
  if (now - vibLastEdgeMs > DEBOUNCE_MS) {
    vibTriggered = true;
    vibLastEdgeMs = now;
  }
}

void relayOn()  { digitalWrite(RELAY_PIN, RELAY_ACTIVE_LOW ? LOW : HIGH); }
void relayOff() { digitalWrite(RELAY_PIN, RELAY_ACTIVE_LOW ? HIGH : LOW); }
bool relayIsOn(){ return RELAY_ACTIVE_LOW ? (digitalRead(RELAY_PIN)==LOW) : (digitalRead(RELAY_PIN)==HIGH); }

bool isAllowed(const String& chat_id) {
  // Hanya pemilik (MY_CHAT_ID) yang boleh kontrol
  if (MY_CHAT_ID.length() == 0) return true;   // kalau mau uji tanpa filter
  return (chat_id == MY_CHAT_ID);
}

void connectWiFi() {
  Serial.print("Menghubungkan ke WiFi: "); Serial.println(WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  int retry = 0;
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
    if (++retry > 60) { // ~30 detik
      Serial.println("\nGagal konek WiFi. Restart 5s...");
      delay(5000);
      ESP.restart();
    }
  }
  Serial.println();
  Serial.print("WiFi connected. IP: "); Serial.println(WiFi.localIP());
}

// Tunggu koordinat valid hingga timeout
bool getGPSFix(double &lat, double &lng, unsigned long timeout_ms = GPS_FIX_TIMEOUT_MS) {
  unsigned long start = millis();
  bool got = false;
  while (millis() - start < timeout_ms) {
    while (GPS_Serial.available()) {
      char c = GPS_Serial.read();
      gps.encode(c);
      if (gps.location.isValid()) {
        lat = gps.location.lat();
        lng = gps.location.lng();
        got = true;
      }
    }
    if (got) break;
    delay(5);
  }
  return got;
}

// ==========> 6) HELPER POST TELEGRAM VIA HTTPClient <==========
String telegramPost(const String& method, const String& formEncoded) {
  String url = "https://api.telegram.org/bot" + String(BOT_TOKEN) + "/" + method;

  WiFiClientSecure httpsClient;
  httpsClient.setInsecure(); // prototyping; produksi: pakai setCACert()

  HTTPClient https;
  if (!https.begin(httpsClient, url)) {
    Serial.println("HTTP begin() gagal");
    return "begin_failed";
  }

  https.addHeader("Content-Type", "application/x-www-form-urlencoded");
  int httpCode = https.POST(formEncoded);
  String payload = https.getString();
  https.end();

  Serial.printf("HTTP %d %s\n", httpCode, url.c_str());
  Serial.println("Payload: " + payload);
  return payload;
}

// Kirim lokasi ke pemilik
void sendLocationToTelegram(double lat, double lng) {
  if (MY_CHAT_ID.isEmpty()) {
    Serial.println("MY_CHAT_ID kosong — tidak bisa kirim.");
    return;
  }

  // 1) kirim share location (sendLocation)
  String postData = "chat_id=" + MY_CHAT_ID +
                    "&latitude=" + String(lat, 6) +
                    "&longitude=" + String(lng, 6);

  String resp = telegramPost("sendLocation", postData);

  // 2) kirim juga teks + link maps
  String maps = "https://maps.google.com/?q=" + String(lat, 6) + "," + String(lng, 6);
  String msg  = String("[") + DEVICE_NAME + "]\n"
                "Getaran terdeteksi.\n"
                "Lokasi (GPS):\n"
                "Lat: " + String(lat, 6) + "\nLng: " + String(lng, 6) + "\n" + maps;

  bot.sendMessage(MY_CHAT_ID, msg, "");
}

// ==========> 7) HANDLER PERINTAH TELEGRAM <==========
void handleNewMessages(int numNewMessages) {
  for (int i = 0; i < numNewMessages; i++) {
    String chat_id   = bot.messages[i].chat_id;
    String text      = bot.messages[i].text;
    String from_name = bot.messages[i].from_name;

    Serial.printf("Msg from %s (%s): %s\n",
                  from_name.c_str(), chat_id.c_str(), text.c_str());

    if (!isAllowed(chat_id)) {
      bot.sendMessage(chat_id,
        "Maaf, ini bukan perangkat milik Anda. Perangkat ini terdaftar untuk pemilik lain.",
        "");
      continue;
    }

    if (text == "/start") {
      String s = String("Halo ") + from_name + "!\n"
                 "Ini adalah perangkat: " + DEVICE_NAME + "\n\n"
                 "Perintah:\n"
                 "• /on     -> hidupkan relay\n"
                 "• /off    -> matikan relay\n"
                 "• /status -> cek status perangkat";
      bot.sendMessage(chat_id, s, "");
    }
    else if (text == "/on") {
      relayOn();
      String s = String("[") + DEVICE_NAME + "] Relay: ON ✅";
      bot.sendMessage(chat_id, s, "");
    }
    else if (text == "/off") {
      relayOff();
      String s = String("[") + DEVICE_NAME + "] Relay: OFF ⛔";
      bot.sendMessage(chat_id, s, "");
    }
    else if (text == "/status") {
      String s = String("Device : ") + DEVICE_NAME + "\n" +
                 "WiFi RSSI: " + String(WiFi.RSSI()) + " dBm\n" +
                 "Relay   : " + String(relayIsOn() ? "ON" : "OFF");
      bot.sendMessage(chat_id, s, "");
    }
    else {
      bot.sendMessage(chat_id,
        "Perintah tidak dikenali. Coba /start atau /status.",
        "");
    }
  }
}

// ==========> 8) SETUP & LOOP <==========
void setup() {
  Serial.begin(115200);
  delay(300);

  pinMode(RELAY_PIN, OUTPUT);
  relayOff();

  pinMode(VIB_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(VIB_PIN), vibISR, FALLING); // ganti RISING/CHANGE jika perlu

  GPS_Serial.begin(GPS_BAUD, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);

  connectWiFi();

  // TLS untuk UniversalTelegramBot
  secured_client.setInsecure(); // prototyping

  Serial.println("=================================");
  Serial.print("DEVICE_NAME : "); Serial.println(DEVICE_NAME);
  Serial.print("MY_CHAT_ID  : "); Serial.println(MY_CHAT_ID);
  Serial.println("Siap. Kirim /start ke bot untuk mengontrol perangkat ini.");
  Serial.println("=================================");
}

void loop() {
  // Polling Telegram
  if (millis() - bot_lasttime > BOT_MTBS) {
    int numNew = bot.getUpdates(bot.last_message_received + 1);
    while (numNew) {
      handleNewMessages(numNew);
      numNew = bot.getUpdates(bot.last_message_received + 1);
    }
    bot_lasttime = millis();
  }

  // Event getaran
  if (vibTriggered) {
    vibTriggered = false;

    unsigned long now = millis();
    if (now - lastReportMs < COOLDOWN_MS) {
      Serial.println("Getaran diabaikan (cooldown).");
    } else {
      lastReportMs = now;
      Serial.println("Getaran terdeteksi. Mencoba mendapatkan GPS fix...");

      double lat = 0, lng = 0;
      bool ok = getGPSFix(lat, lng, GPS_FIX_TIMEOUT_MS);
      if (ok) {
        Serial.printf("GPS FIX: %.6f, %.6f\n", lat, lng);
        sendLocationToTelegram(lat, lng);
      } else {
        Serial.println("GPS belum mendapatkan fix dalam batas waktu.");
        if (!MY_CHAT_ID.isEmpty()) {
          String msg = String("[") + DEVICE_NAME + "]\n"
                       "Getaran terdeteksi, tetapi GPS belum mendapatkan fix.\n"
                       "Pastikan antena GPS punya pandangan langit yang baik.";
          bot.sendMessage(MY_CHAT_ID, msg, "");
        }
      }
    }
  }
}
