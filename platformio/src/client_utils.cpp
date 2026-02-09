/* Client side utilities for esp32-weather-epd.
 * Copyright (C) 2022-2024  Luke Marzen
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

// built-in C++ libraries
#include <cstring>
#include <vector>

// arduino/esp32 libraries
#include <Arduino.h>
#include <esp_sntp.h>
#include <HTTPClient.h>
#include <SPI.h>
#include <time.h>
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

// additional libraries
#include <Adafruit_BusIO_Register.h>
#include <ArduinoJson.h>

// header files
#include "_locale.h"
#include "api_response.h"
#include "aqi.h"
#include "client_utils.h"
#include "config.h"
#include "display_utils.h"
#include "renderer.h"
#include "user_config.h"
#ifndef USE_HTTP
  #include <WiFiClientSecure.h>
#endif

#ifdef USE_HTTP
  static const uint16_t OWM_PORT = 80;
#else
  static const uint16_t OWM_PORT = 443;
#endif

// ── FreeRTOS Task Structures for Parallel Yahoo Finance Fetching ──

// Maximum number of concurrent fetch tasks (limited by SSL/TLS memory requirements)
// Each SSL connection needs ~20-30KB for handshake, certificates, and encryption buffers
#define MAX_CONCURRENT_TASKS 2

// Structure to pass data to Yahoo Finance fetch tasks
typedef struct {
  const char* symbol;        // Yahoo symbol to fetch
  asset_data_t* asset;       // Pointer to asset to populate
  const char* displaySymbol; // Display symbol
  const char* name;          // Asset name
  SemaphoreHandle_t doneSemaphore; // Semaphore to signal completion
  bool success;              // Whether fetch succeeded
} YahooFetchTaskData_t;

/* Power-on and connect WiFi.
 * Takes int parameter to store WiFi RSSI, or “Received Signal Strength
 * Indicator"
 *
 * Returns WiFi status.
 */
wl_status_t startWiFi(int &wifiRSSI)
{
  WiFi.mode(WIFI_STA);
  Serial.printf("%s '%s'", TXT_CONNECTING_TO, WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  // timeout if WiFi does not connect in WIFI_TIMEOUT ms from now
  unsigned long timeout = millis() + WIFI_TIMEOUT;
  wl_status_t connection_status = WiFi.status();

  while ((connection_status != WL_CONNECTED) && (millis() < timeout))
  {
    Serial.print(".");
    delay(50);
    connection_status = WiFi.status();
  }
  Serial.println();

  if (connection_status == WL_CONNECTED)
  {
    wifiRSSI = WiFi.RSSI(); // get WiFi signal strength now, because the WiFi
                            // will be turned off to save power!
    Serial.println("IP: " + WiFi.localIP().toString());
  }
  else
  {
    Serial.printf("%s '%s'\n", TXT_COULD_NOT_CONNECT_TO, WIFI_SSID);
  }
  return connection_status;
} // startWiFi

/* Disconnect and power-off WiFi.
 */
void killWiFi()
{
  WiFi.disconnect();
  WiFi.mode(WIFI_OFF);
} // killWiFi

/* Prints the local time to serial monitor.
 *
 * Returns true if getting local time was a success, otherwise false.
 */
bool printLocalTime(tm *timeInfo)
{
  int attempts = 0;
  while (!getLocalTime(timeInfo) && attempts++ < 3)
  {
    Serial.println(TXT_FAILED_TO_GET_TIME);
    return false;
  }
  Serial.println(timeInfo, "%A, %B %d, %Y %H:%M:%S");
  return true;
} // printLocalTime

/* Waits for NTP server time sync, adjusted for the time zone specified in
 * config.cpp.
 *
 * Returns true if time was set successfully, otherwise false.
 *
 * Note: Must be connected to WiFi to get time from NTP server.
 */
bool waitForSNTPSync(tm *timeInfo)
{
  // Wait for SNTP synchronization to complete
  unsigned long timeout = millis() + NTP_TIMEOUT;
  if ((sntp_get_sync_status() == SNTP_SYNC_STATUS_RESET)
      && (millis() < timeout))
  {
    Serial.print(TXT_WAITING_FOR_SNTP);
    delay(100); // ms
    while ((sntp_get_sync_status() == SNTP_SYNC_STATUS_RESET)
        && (millis() < timeout))
    {
      Serial.print(".");
      delay(100); // ms
    }
    Serial.println();
  }
  return printLocalTime(timeInfo);
} // waitForSNTPSync

/* Perform an HTTP GET request to OpenWeatherMap's "One Call" API
 * If data is received, it will be parsed and stored in the global variable
 * owm_onecall.
 *
 * Returns the HTTP Status Code.
 */
#ifdef USE_HTTP
  int getOWMonecall(WiFiClient &client, owm_resp_onecall_t &r)
#else
  int getOWMonecall(WiFiClientSecure &client, owm_resp_onecall_t &r)
#endif
{
  int attempts = 0;
  bool rxSuccess = false;
  DeserializationError jsonErr = {};
  String uri = "/data/" + OWM_ONECALL_VERSION
               + "/onecall?lat=" + LAT + "&lon=" + LON + "&lang=" + OWM_LANG
               + "&units=standard&exclude=minutely";
#if !DISPLAY_ALERTS
  // exclude alerts
  uri += ",alerts";
#endif

  // This string is printed to terminal to help with debugging. The API key is
  // censored to reduce the risk of users exposing their key.
  String sanitizedUri = OWM_ENDPOINT + uri + "&appid={API key}";

  uri += "&appid=" + OWM_APIKEY;

  Serial.print(TXT_ATTEMPTING_HTTP_REQ);
  Serial.println(": " + sanitizedUri);
  int httpResponse = 0;
  while (!rxSuccess && attempts < 3)
  {
    wl_status_t connection_status = WiFi.status();
    if (connection_status != WL_CONNECTED)
    {
      // -512 offset distinguishes these errors from httpClient errors
      return -512 - static_cast<int>(connection_status);
    }

    HTTPClient http;
    http.setConnectTimeout(HTTP_CLIENT_TCP_TIMEOUT); // default 5000ms
    http.setTimeout(HTTP_CLIENT_TCP_TIMEOUT); // default 5000ms
    http.begin(client, OWM_ENDPOINT, OWM_PORT, uri);
    httpResponse = http.GET();
    if (httpResponse == HTTP_CODE_OK)
    {
      jsonErr = deserializeOneCall(http.getStream(), r);
      if (jsonErr)
      {
        // -256 offset distinguishes these errors from httpClient errors
        httpResponse = -256 - static_cast<int>(jsonErr.code());
      }
      rxSuccess = !jsonErr;
    }
    client.stop();
    http.end();
    Serial.println("  " + String(httpResponse, DEC) + " "
                   + getHttpResponsePhrase(httpResponse));
    ++attempts;
  }

  return httpResponse;
} // getOWMonecall

/* Perform an HTTP GET request to OpenWeatherMap's "Air Pollution" API
 * If data is received, it will be parsed and stored in the global variable
 * owm_air_pollution.
 *
 * Returns the HTTP Status Code.
 */
#ifdef USE_HTTP
  int getOWMairpollution(WiFiClient &client, owm_resp_air_pollution_t &r)
#else
  int getOWMairpollution(WiFiClientSecure &client, owm_resp_air_pollution_t &r)
#endif
{
  int attempts = 0;
  bool rxSuccess = false;
  DeserializationError jsonErr = {};

  // set start and end to appropriate values so that the last 24 hours of air
  // pollution history is returned. Unix, UTC.
  time_t now;
  int64_t end = time(&now);
  // minus 1 is important here, otherwise we could get an extra hour of history
  int64_t start = end - ((3600 * OWM_NUM_AIR_POLLUTION) - 1);
  char endStr[22];
  char startStr[22];
  sprintf(endStr, "%lld", end);
  sprintf(startStr, "%lld", start);
  String uri = "/data/2.5/air_pollution/history?lat=" + LAT + "&lon=" + LON
               + "&start=" + startStr + "&end=" + endStr
               + "&appid=" + OWM_APIKEY;
  // This string is printed to terminal to help with debugging. The API key is
  // censored to reduce the risk of users exposing their key.
  String sanitizedUri = OWM_ENDPOINT +
               "/data/2.5/air_pollution/history?lat=" + LAT + "&lon=" + LON
               + "&start=" + startStr + "&end=" + endStr
               + "&appid={API key}";

  Serial.print(TXT_ATTEMPTING_HTTP_REQ);
  Serial.println(": " + sanitizedUri);
  int httpResponse = 0;
  while (!rxSuccess && attempts < 3)
  {
    wl_status_t connection_status = WiFi.status();
    if (connection_status != WL_CONNECTED)
    {
      // -512 offset distinguishes these errors from httpClient errors
      return -512 - static_cast<int>(connection_status);
    }

    HTTPClient http;
    http.setConnectTimeout(HTTP_CLIENT_TCP_TIMEOUT); // default 5000ms
    http.setTimeout(HTTP_CLIENT_TCP_TIMEOUT); // default 5000ms
    http.begin(client, OWM_ENDPOINT, OWM_PORT, uri);
    httpResponse = http.GET();
    if (httpResponse == HTTP_CODE_OK)
    {
      jsonErr = deserializeAirQuality(http.getStream(), r);
      if (jsonErr)
      {
        // -256 offset to distinguishes these errors from httpClient errors
        httpResponse = -256 - static_cast<int>(jsonErr.code());
      }
      rxSuccess = !jsonErr;
    }
    client.stop();
    http.end();
    Serial.println("  " + String(httpResponse, DEC) + " "
                   + getHttpResponsePhrase(httpResponse));
    ++attempts;
  }

  return httpResponse;
} // getOWMairpollution

/* Fetch cryptocurrency data from CoinGecko API.
 * Populates the crypto page with data for 4 coins.
 *
 * Returns HTTP_CODE_OK on success, error code otherwise.
 */
int fetchCoinGecko(page_data_t &page)
{
  Serial.println("Fetching CoinGecko data...");

  WiFiClientSecure client;
  client.setInsecure(); // CoinGecko uses HTTPS, skip cert verification
  client.setHandshakeTimeout(10); // 10 seconds for TLS handshake

  String ids = String(CRYPTO_1_ID) + "," + CRYPTO_2_ID + ","
             + CRYPTO_3_ID + "," + CRYPTO_4_ID;
  // Pass API key as URL parameter (more reliable than custom header on ESP32)
  String apiKey = COINGECKO_API_KEY;
  String uri = "/api/v3/coins/markets"
               "?vs_currency=" COINGECKO_VS_CURRENCY
               "&ids=" + ids +
               "&sparkline=true"
               "&price_change_percentage=24h,7d,30d,1y";
  if (String(apiKey).length() > 0)
  {
    uri += "&x_cg_demo_api_key=" + apiKey;
  }

  // Initialize assets with user config display info (before fetch, so names
  // show even on failure)
  strncpy(page.assets[0].displaySymbol, CRYPTO_1_SYMBOL, sizeof(page.assets[0].displaySymbol) - 1);
  strncpy(page.assets[1].displaySymbol, CRYPTO_2_SYMBOL, sizeof(page.assets[1].displaySymbol) - 1);
  strncpy(page.assets[2].displaySymbol, CRYPTO_3_SYMBOL, sizeof(page.assets[2].displaySymbol) - 1);
  strncpy(page.assets[3].displaySymbol, CRYPTO_4_SYMBOL, sizeof(page.assets[3].displaySymbol) - 1);

  strncpy(page.assets[0].name, CRYPTO_1_NAME, sizeof(page.assets[0].name) - 1);
  strncpy(page.assets[1].name, CRYPTO_2_NAME, sizeof(page.assets[1].name) - 1);
  strncpy(page.assets[2].name, CRYPTO_3_NAME, sizeof(page.assets[2].name) - 1);
  strncpy(page.assets[3].name, CRYPTO_4_NAME, sizeof(page.assets[3].name) - 1);

  Serial.println("  GET api.coingecko.com" + uri);
  Serial.println("  Free heap before CoinGecko: " + String(ESP.getFreeHeap()));

  int httpResponse = 0;
  int attempts = 0;
  bool rxSuccess = false;

  while (!rxSuccess && attempts < 3)
  {
    HTTPClient http;
    http.setConnectTimeout(HTTP_CLIENT_TCP_TIMEOUT);
    http.setTimeout(HTTP_CLIENT_TCP_TIMEOUT);

    http.begin(client, "api.coingecko.com", 443, uri, true);
    http.addHeader("Accept", "application/json");

    httpResponse = http.GET();
    Serial.println("  CoinGecko response: " + String(httpResponse));

    if (httpResponse == HTTP_CODE_OK)
    {
      // Read full response into String (stream parsing fails on CoinGecko
      // HTTPS responses due to chunked/TLS issues)
      String body = http.getString();
      Serial.println("  CoinGecko body length: " + String(body.length()));
      if (body.length() > 0)
      {
        Serial.println("  CoinGecko body preview: "
                       + body.substring(0, 120));
        if (deserializeCoinGecko(body, page))
        {
          rxSuccess = true;
        }
        else
        {
          Serial.println("  CoinGecko deserialization failed");
          httpResponse = -256;
        }
      }
      else
      {
        Serial.println("  CoinGecko body is empty!");
        httpResponse = -256;
      }
    }
    else
    {
      Serial.println("  CoinGecko HTTP error, attempt "
                     + String(attempts + 1) + "/3");
    }

    client.stop();
    http.end();
    ++attempts;
  }

  return httpResponse;
} // end fetchCoinGecko

/* Fetch financial data from Yahoo Finance chart API for a single symbol.
 * Uses range=1mo&interval=1d to get ~30 days of daily data.
 *
 * Returns HTTP_CODE_OK on success, error code otherwise.
 */
int fetchYahooFinance(const char *symbol, asset_data_t &asset)
{
  Serial.print("  Fetching Yahoo Finance: ");
  Serial.println(symbol);

  WiFiClientSecure client;
  client.setInsecure(); // Yahoo Finance uses HTTPS
  client.setHandshakeTimeout(10); // 10 seconds for TLS handshake

  // URL-encode the symbol (^ needs encoding)
  String encodedSymbol = symbol;
  encodedSymbol.replace("^", "%5E");

  String uri = "/v8/finance/chart/"
             + encodedSymbol + "?range=1mo&interval=1d";

  int httpResponse = 0;
  int attempts = 0;
  bool rxSuccess = false;

  while (!rxSuccess && attempts < 2)
  {
    HTTPClient http;
    http.setConnectTimeout(HTTP_CLIENT_TCP_TIMEOUT);
    http.setTimeout(HTTP_CLIENT_TCP_TIMEOUT);

    http.begin(client, "query1.finance.yahoo.com", 443, uri, true);
    http.addHeader("Accept", "application/json");
    http.addHeader("User-Agent", "ESP32-Ticker/1.0");

    httpResponse = http.GET();
    Serial.println("    Response: " + String(httpResponse));

    if (httpResponse == HTTP_CODE_OK)
    {
      if (deserializeYahooFinance(http.getStream(), asset))
      {
        rxSuccess = true;
      }
      else
      {
        Serial.println("    Yahoo Finance deserialization failed");
        httpResponse = -256;
      }
    }

    client.stop();
    http.end();
    ++attempts;
  }

  return httpResponse;
} // end fetchYahooFinance

/* FreeRTOS task function for parallel Yahoo Finance fetching.
 * Each task fetches data for one symbol and signals completion via semaphore.
 */
static void yahooFetchTask(void* parameter)
{
  YahooFetchTaskData_t* taskData = (YahooFetchTaskData_t*)parameter;

  // Initialize asset with display info
  strncpy(taskData->asset->displaySymbol, taskData->displaySymbol,
          sizeof(taskData->asset->displaySymbol) - 1);
  taskData->asset->displaySymbol[sizeof(taskData->asset->displaySymbol) - 1] = '\0';

  strncpy(taskData->asset->name, taskData->name,
          sizeof(taskData->asset->name) - 1);
  taskData->asset->name[sizeof(taskData->asset->name) - 1] = '\0';

  strncpy(taskData->asset->symbol, taskData->symbol,
          sizeof(taskData->asset->symbol) - 1);
  taskData->asset->symbol[sizeof(taskData->asset->symbol) - 1] = '\0';

  taskData->asset->valid = false;

  // Fetch data from Yahoo Finance
  int result = fetchYahooFinance(taskData->symbol, *taskData->asset);
  taskData->success = (result == HTTP_CODE_OK);

  // Signal completion
  xSemaphoreGive(taskData->doneSemaphore);

  // Delete this task
  vTaskDelete(NULL);
} // end yahooFetchTask

/* Helper function to launch parallel Yahoo Finance fetches with concurrency limit.
 * Launches tasks in batches to avoid overwhelming the system.
 */
static void fetchYahooParallel(const char** symbols, const char** displays,
                               const char** names, asset_data_t* assets,
                               int count, page_data_t &page)
{
  Serial.println("  Starting parallel fetch with " + String(count) + " assets...");

  // Create semaphore for task completion signaling
  SemaphoreHandle_t doneSemaphore = xSemaphoreCreateCounting(count, 0);
  if (doneSemaphore == NULL)
  {
    Serial.println("  ERROR: Failed to create semaphore, falling back to sequential");
    // Fallback to sequential
    for (int i = 0; i < count; ++i)
    {
      strncpy(assets[i].displaySymbol, displays[i], sizeof(assets[i].displaySymbol) - 1);
      strncpy(assets[i].name, names[i], sizeof(assets[i].name) - 1);
      strncpy(assets[i].symbol, symbols[i], sizeof(assets[i].symbol) - 1);
      assets[i].valid = false;
      if (fetchYahooFinance(symbols[i], assets[i]) == HTTP_CODE_OK)
      {
        page.valid = true;
      }
    }
    return;
  }

  // Prepare task data for all assets
  YahooFetchTaskData_t taskDataArray[count];
  for (int i = 0; i < count; ++i)
  {
    taskDataArray[i].symbol = symbols[i];
    taskDataArray[i].asset = &assets[i];
    taskDataArray[i].displaySymbol = displays[i];
    taskDataArray[i].name = names[i];
    taskDataArray[i].doneSemaphore = doneSemaphore;
    taskDataArray[i].success = false;
  }

  // Launch tasks in batches to limit concurrency
  int tasksLaunched = 0;
  page.valid = false;

  while (tasksLaunched < count)
  {
    // Calculate how many tasks to launch in this batch
    int batchSize = min(MAX_CONCURRENT_TASKS, count - tasksLaunched);

    Serial.println("  Launching batch of " + String(batchSize) + " tasks...");

    // Launch this batch of tasks
    for (int i = 0; i < batchSize; ++i)
    {
      int taskIndex = tasksLaunched + i;
      BaseType_t result = xTaskCreate(
        yahooFetchTask,                    // Task function
        ("YF_" + String(taskIndex)).c_str(), // Task name
        8192,                               // Stack size (8KB per task)
        &taskDataArray[taskIndex],          // Task parameter
        1,                                  // Priority
        NULL                                // Task handle (we don't need it)
      );

      if (result != pdPASS)
      {
        Serial.println("  ERROR: Failed to create task " + String(taskIndex));
        taskDataArray[taskIndex].success = false;
        // Signal semaphore anyway so we don't hang
        xSemaphoreGive(doneSemaphore);
      }
    }

    // Wait for this batch to complete
    for (int i = 0; i < batchSize; ++i)
    {
      // Wait up to 15 seconds for each task
      if (xSemaphoreTake(doneSemaphore, pdMS_TO_TICKS(15000)) == pdTRUE)
      {
        int taskIndex = tasksLaunched + i;
        if (taskDataArray[taskIndex].success)
        {
          page.valid = true; // At least one fetch succeeded
        }
      }
      else
      {
        Serial.println("  WARNING: Task " + String(tasksLaunched + i) + " timed out");
      }
    }

    tasksLaunched += batchSize;

    // Small delay between batches to let system stabilize
    if (tasksLaunched < count)
    {
      delay(100);
    }
  }

  // Clean up
  vSemaphoreDelete(doneSemaphore);

  // Count successful fetches
  int successCount = 0;
  for (int i = 0; i < count; ++i)
  {
    if (taskDataArray[i].success)
    {
      successCount++;
    }
  }
  Serial.println("  Parallel fetch complete: " + String(successCount) + "/" + String(count) + " succeeded");
} // end fetchYahooParallel

/* Fetch all financial data for pages 1-4.
 * Populates crypto, indices, commodities, and forex page data.
 */
void fetchAllFinancialData(page_data_t &cryptoPage,
                           page_data_t &indicesPage,
                           page_data_t &commoditiesPage,
                           page_data_t &forexPage)
{
  Serial.println("=== Fetching all financial data ===");

  // ── Page 1: Crypto (single API call for all 4 coins) ──
  fetchCoinGecko(cryptoPage);

  // ── Page 2: Stock Indices (4 Yahoo Finance calls in parallel) ──
  Serial.println("Fetching Stock Indices (parallel)...");
  const char *indexSymbols[]  = {INDEX_1_SYMBOL, INDEX_2_SYMBOL, INDEX_3_SYMBOL, INDEX_4_SYMBOL};
  const char *indexDisplays[] = {INDEX_1_DISPLAY, INDEX_2_DISPLAY, INDEX_3_DISPLAY, INDEX_4_DISPLAY};
  const char *indexNames[]    = {INDEX_1_NAME, INDEX_2_NAME, INDEX_3_NAME, INDEX_4_NAME};
  indicesPage.valid = false;
  fetchYahooParallel(indexSymbols, indexDisplays, indexNames,
                     indicesPage.assets, ASSETS_PER_PAGE, indicesPage);
  indicesPage.lastUpdated = time(nullptr);

  // ── Page 3: Commodities (4 Yahoo Finance calls in parallel) ──
  Serial.println("Fetching Commodities (parallel)...");
  const char *comSymbols[]  = {COMMODITY_1_SYMBOL, COMMODITY_2_SYMBOL, COMMODITY_3_SYMBOL, COMMODITY_4_SYMBOL};
  const char *comDisplays[] = {COMMODITY_1_DISPLAY, COMMODITY_2_DISPLAY, COMMODITY_3_DISPLAY, COMMODITY_4_DISPLAY};
  const char *comNames[]    = {COMMODITY_1_NAME, COMMODITY_2_NAME, COMMODITY_3_NAME, COMMODITY_4_NAME};
  commoditiesPage.valid = false;
  fetchYahooParallel(comSymbols, comDisplays, comNames,
                     commoditiesPage.assets, ASSETS_PER_PAGE, commoditiesPage);
  commoditiesPage.lastUpdated = time(nullptr);

  // ── Page 4: Forex (4 Yahoo Finance calls in parallel) ──
  Serial.println("Fetching Forex (parallel)...");
  const char *fxSymbols[]  = {FX_1_SYMBOL, FX_2_SYMBOL, FX_3_SYMBOL, FX_4_SYMBOL};
  const char *fxDisplays[] = {FX_1_DISPLAY, FX_2_DISPLAY, FX_3_DISPLAY, FX_4_DISPLAY};
  const char *fxNames[]    = {FX_1_NAME, FX_2_NAME, FX_3_NAME, FX_4_NAME};
  forexPage.valid = false;
  fetchYahooParallel(fxSymbols, fxDisplays, fxNames,
                     forexPage.assets, ASSETS_PER_PAGE, forexPage);
  forexPage.lastUpdated = time(nullptr);

  // ── Calculate CAD prices for crypto assets ──
  float usdToCAD = USD_TO_CAD_FALLBACK; // fallback rate
  if (forexPage.valid && forexPage.assets[0].valid) {
    usdToCAD = forexPage.assets[0].price; // USDCAD=X is first forex asset
    Serial.println("Using live USD/CAD rate: " + String(usdToCAD, 4));
  } else {
    Serial.println("Using fallback USD/CAD rate: " + String(usdToCAD, 4));
  }

  for (int i = 0; i < ASSETS_PER_PAGE; ++i) {
    if (cryptoPage.assets[i].valid) {
      cryptoPage.assets[i].price_cad = cryptoPage.assets[i].price * usdToCAD;
    } else {
      cryptoPage.assets[i].price_cad = 0.0f;
    }
  }

  Serial.println("=== Financial data fetch complete ===");
} // end fetchAllFinancialData

/* Prints debug information about heap usage.
 */
void printHeapUsage() {
  Serial.println("[debug] Heap Size       : "
                 + String(ESP.getHeapSize()) + " B");
  Serial.println("[debug] Available Heap  : "
                 + String(ESP.getFreeHeap()) + " B");
  Serial.println("[debug] Min Free Heap   : "
                 + String(ESP.getMinFreeHeap()) + " B");
  Serial.println("[debug] Max Allocatable : "
                 + String(ESP.getMaxAllocHeap()) + " B");
  return;
}

