#include <thread>
#include <chrono>
#include <iostream>
#include <iomanip>
#include <fstream>
#include <sstream>
#include <mutex>
#include <Arduino.h>
#include <ArduinoJson.h>
#include <SD.h>
#include <M5Unified.h>
#include <BLEServer.h>
#include <BLEDevice.h>
#include <WiFi.h>
#include <Time.h>
#include "FS.h"

const char *deviceShortName = "M5Time"; // デバイス名
const char *ssid_filename = "/ssid.txt";

String JsonData; // JSON形式データの格納用
int sdstat = 0;
String i_ssid, i_pass;

std::shared_ptr<std::thread> th = nullptr;
BLEServer *server = nullptr;
BLEAdvertising *advertising = nullptr;

std::shared_ptr<std::thread> timethread = nullptr;

std::mutex mtx_;

const char* ntpServer = "ntp.nict.jp";
const long gmtOffset_sec = 9 * 3600;
const int daylightOffset_sec = 0;

WiFiClient client;

// 時間関連
struct tm timeinfo;
struct tm disp_timeinfo;

// NTPによる時刻取得関数
int ntp(){
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer); //NTPによる時刻取得
  tm time;
  if (!getLocalTime(&time)) {
    M5.Display.printf("/nFailed to obtain time"); //時刻取得失敗表示
    return (false); //時刻取得失敗でリターン
  } else {
    std::lock_guard<std::mutex> lock(mtx_);
    timeinfo = time;
  }
  return (true); //時刻取得成功でリターン
}

int ntpWithWIFI(){
  char buf_ssid[33], buf_pass[65]; // SSID,パスワードをChar型へ変更
  i_ssid.toCharArray(buf_ssid, 33);
  i_pass.toCharArray(buf_pass, 65);

  uint8_t wifi_retry_cnt;
  M5.Lcd.fillScreen(TFT_BLACK); //画面初期化
  M5.Lcd.setTextSize(1);
  M5.Lcd.setCursor(0, 0);
  M5.Lcd.printf("Connecting to %s\n", buf_ssid);
  WiFi.begin(buf_ssid, buf_pass); //WiFi接続開始
  wifi_retry_cnt = 20; //0.5秒×20=最大10秒で接続タイムアウト
  while (WiFi.status() != WL_CONNECTED){
    delay(500);
    M5.Lcd.printf("*"); //0.5秒毎に”＊”を表示
    if(--wifi_retry_cnt == 0) {
      WiFi.disconnect(true); //タイムアウトでWiFiオフ
      WiFi.mode(WIFI_OFF);
      M5.Lcd.printf("\nCONNECTION FAIL"); //WiFi接続失敗表示
      return(false); //接続失敗でリターン
    }
  }
  M5.Lcd.printf("\nCONNECTED"); //WiFi接続成功表示
  bool result = ntp();
  WiFi.disconnect(true); //WiFi切断
  WiFi.mode(WIFI_OFF); //WiFiオフ
  M5.Lcd.fillScreen(TFT_BLACK); //画面消去
  return (result); //時刻取得成功でリターン
}

std::string getLocalTimeAsString() {
  tm time;
  {
    std::lock_guard<std::mutex> lock(mtx_);
    time = timeinfo;
  }

  std::stringstream tmstr;
  tmstr << std::setw(4) << std::setfill('0') << (time.tm_year + 1900) << "-";
  tmstr << std::setw(2) << std::setfill('0') << (time.tm_mon + 1) << "-";
  tmstr << std::setw(2) << std::setfill('0') << time.tm_mday << "T";
  tmstr << std::setw(2) << std::setfill('0') << time.tm_hour << ":";
  tmstr << std::setw(2) << std::setfill('0') << time.tm_min << ":";
  tmstr << std::setw(2) << std::setfill('0') << time.tm_sec << "+09:00";
  return tmstr.str();
}

std::vector<char> getLocalTimeAsCharArray() {
  tm time;
  {
    std::lock_guard<std::mutex> lock(mtx_);
    time = timeinfo;
  }

  std::vector<char> tmarray;
  tmarray.push_back((char)((time.tm_year + 1900) % 256));
  tmarray.push_back((char)((time.tm_year + 1900) / 256));
  tmarray.push_back((char)(time.tm_mon + 1));
  tmarray.push_back((char)time.tm_mday);
  tmarray.push_back((char)time.tm_hour);
  tmarray.push_back((char)time.tm_min);
  tmarray.push_back((char)time.tm_sec);
  tmarray.push_back((char)9);
  tmarray.push_back((char)0);
  tmarray.push_back((char)time.tm_wday);
  return tmarray;
}

void setAdvertisementData(BLEAdvertising *pAdvertising)
{
  std::vector<char> tmarray = getLocalTimeAsCharArray();

  // string領域に送信情報を連結する
  std::string strData = "";
  strData += (char)13;                        // length: 13 octets
  strData += (char)0xff;                      // Manufacturer specific data
  strData += (char)0xff;                      // manufacturer ID low byte
  strData += (char)0xff;                      // manufacturer ID high byte
  Serial.printf("\n");
  for (const char& chr : tmarray) {
    strData += chr;                           // 日時
    Serial.printf("%02x ", chr);
  }
  Serial.printf("\n");

  // デバイス名とフラグをセットし、送信情報を組み込んでアドバタイズオブジェクトに設定する
  BLEAdvertisementData oAdvertisementData = BLEAdvertisementData();
  oAdvertisementData.setName(deviceShortName);
  oAdvertisementData.setFlags(0x06); // LE General Discoverable Mode | BR_EDR_NOT_SUPPORTED
  oAdvertisementData.addData(strData);
  pAdvertising->setAdvertisementData(oAdvertisementData);
  pAdvertising->setAdvertisementType(esp_ble_adv_type_t::ADV_TYPE_NONCONN_IND);
}

void setupBLE()
{
  Serial.println("Starting BLE");
  BLEDevice::init(deviceShortName);
  server = BLEDevice::createServer();
  advertising = server->getAdvertising();
  th = std::make_shared<std::thread>([&]()
                                     {
    while (true) {
      setAdvertisementData(advertising);
      Serial.print("Starting Advertisement: ");
      advertising->start();
      std::this_thread::sleep_for(std::chrono::seconds(4));
      advertising->stop();
      Serial.println("Stop Advertisement. ");
    } });
}

void setup()
{
  auto cfg = M5.config();
  cfg.serial_baudrate = 9600;
  M5.begin(cfg);

  // Get MAC address for WiFi station
  uint8_t baseMac[6];
  esp_read_mac(baseMac, ESP_MAC_WIFI_STA);
  char baseMacChr[18] = {0};
  sprintf(baseMacChr, "%02X:%02X:%02X:%02X:%02X:%02X", baseMac[0], baseMac[1], baseMac[2], baseMac[3], baseMac[4], baseMac[5]);

  Serial.print("MAC: ");
  Serial.println(baseMacChr);
  M5.Display.setTextSize(2);
  M5.Display.print("MAC: ");
  M5.Display.println(baseMacChr);

  StaticJsonDocument<192> n_jsondata;

  // SDカードマウント待ち
  while (false == SD.begin(GPIO_NUM_4, SPI, 15000000)) {
    Serial.println("SD Wait...");
    delay(500);
  }

  if (SD.exists(ssid_filename))
  {                                     // ファイルの存在確認（SSID.txt）
    Serial.printf("%s exists.\n", ssid_filename); // ファイルがある場合の処理
    delay(500);
    File myFile = SD.open(ssid_filename, FILE_READ); // 読み取り専用でファイルを開く

    if (myFile)
    { // ファイルが正常に開けた場合
      std::stringstream ss;
      M5.Display.printf("%s Content:\n", ssid_filename);
      while (myFile.available())
      { // ファイル内容を順に変数に格納
        JsonData.concat(myFile.readString());
      }
      myFile.close(); // ファイルのクローズ
      sdstat = 1;
    }
    else
    {
      M5.Display.printf("error opening %s\n", ssid_filename); // ファイルが開けない場合
      sdstat = 0;
    }
  }
  else
  {
    M5.Display.printf("%s doesn't exit.\n", ssid_filename); // ファイルが存在しない場合
    Serial.printf("%s doesn't exit.\n", ssid_filename); // シリアルコンソールへの出力
    sdstat = 0;
  }

  if (sdstat == 1)
  {
    // JSON形式データの読み込み
    DeserializationError error = deserializeJson(n_jsondata, JsonData);

    if (error)
    { // JSON形式データ読み込みエラーの場合
      M5.Display.print(F("deserializeJson() failed: "));
      M5.Display.println(error.f_str()); // エラーメッセージのディスプレイ表示
    }
    else
    {                                           // 正常な場合
      i_ssid = n_jsondata["ssid"].as<String>(); // "ssid"の値を取得
      i_pass = n_jsondata["pass"].as<String>(); // "pass"の値を取得

      Serial.println("Read from JSON Data."); // シリアルコンソールへの出力
      M5.Display.setTextColor(RED);                   // テキストカラーの設定
      M5.Display.print("ID: ");
      M5.Display.println(i_ssid); // "ssid"の値をディスプレイ表示
    }
    M5.Display.print("Conecting Wi-Fi "); // シリアルコンソールへの出力

    char buf_ssid[33], buf_pass[65]; // SSID,パスワードをChar型へ変更
    i_ssid.toCharArray(buf_ssid, 33);
    i_pass.toCharArray(buf_pass, 65);

    WiFi.begin(buf_ssid, buf_pass); // Wi-Fi接続開始
    // Wi-Fi接続の状況を監視（WiFi.statusがWL_CONNECTEDになるまで繰り返し
    while (WiFi.status() != WL_CONNECTED)
    {
      delay(500);
      M5.Display.print(".");
    }

    M5.Display.println(""); // Wi-Fi接続結果をディスププレイへ出力
    M5.Display.println("WiFi connected");
    M5.Display.println("IP address: ");
    M5.Display.println(WiFi.localIP()); // IPアドレスをディスププレイへ出力

    ntp();

    WiFi.disconnect(true); //WiFi切断
    WiFi.mode(WIFI_OFF); //WiFiオフ
  }

  timethread = std::make_shared<std::thread>([&](){
    while (true) {
      tm time;
      getLocalTime(&time);
      {
        std::lock_guard<std::mutex> lock(mtx_);
        timeinfo = time;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
  });

  setupBLE();
}

void loop() {
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  //delay(500);

  {
    tm time;
    {
      std::lock_guard<std::mutex> lock(mtx_);
      time = timeinfo;
    }

    //毎日午前2時に時刻取得。時刻取得に失敗しても動作継続
    if((time.tm_hour == 2)&&(time.tm_min == 0)&&(time.tm_sec == 0)) {
      ntpWithWIFI();
    }

    if (disp_timeinfo.tm_year != time.tm_year || disp_timeinfo.tm_mon != time.tm_mon || disp_timeinfo.tm_mday != time.tm_mday ||
      disp_timeinfo.tm_hour != time.tm_hour || disp_timeinfo.tm_min != time.tm_min || disp_timeinfo.tm_sec != time.tm_sec) {
        disp_timeinfo = time;
        std::string timestr = getLocalTimeAsString();

        // Display Time
        M5.Display.setTextSize(2);
        M5.Display.setTextColor(M5.Display.color565(255, 255, 255)); // 文字色指定
        M5.Display.setCursor(0, 0);                                  // 表示開始位置左上角（X,Y）
        M5.Display.clear();
        M5.Display.print(timestr.c_str());
    }
  }
}