/*
Speech to Text Test Sketch

Must run on ESP32S3 with PSRAM tested with Huge partition scheme and a 16R8 chip

   William E Webb (c) released under MIT license please observe all included licenses

I2S Bus Sharing Notes
---------------------

Unlike I2C, I2S is not a multi-drop bus. It has clear signal directions.

Signals you CAN share:
- BCLK (bit clock): Shared. Both mic (RX) and DAC (TX) can use the same bit clock
  if they operate at the same sample rate and word length.
- LRCLK / WS (word select): Shared. Same condition as BCLK.
- MCLK (master clock, if used): Optional. Can be shared by all I2S slaves.

Signals you MUST keep separate:
- SDIN (microphone data into ESP32): Connect mic SD to ESP32 I2S data_in pin.
- SDOUT (ESP32 data out to DAC): Connect ESP32 I2S data_out pin to DAC SD.
  These cannot be shared because they drive in opposite directions.

12/17/2025 Initial untested code mostly by Microsoft Copilot
12/23/2025 Added button activation.  All untested!
12/24/2025 CaptureAndSend completely rewritten.  Tested and it works.

*/

#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include "driver/i2s.h"
#include "secrets.h"
#include <debounce.h>  // Button debounce

#include <TFT_eSPI.h>           // Hardware-specific library
TFT_eSPI tft = TFT_eSPI();      // For Display
#include "support_functions.h"  // For PNG

SET_LOOP_TASK_STACK_SIZE(14 * 1024);  // needed to handle really long strings

#define USE_LINE_BUFFER  // Enable for faster rendering
#define __FILENAME__ (strrchr(__FILE__, '\\') ? strrchr(__FILE__, '\\') + 1 : __FILE__)

// ==== Wi-Fi & API Config ====

// ==== I2S Config ====
#define I2S_PORT I2S_NUM_0
#define SAMPLE_RATE 16000
#define SAMPLE_BITS I2S_BITS_PER_SAMPLE_16BIT
#define BUFFER_LEN 1024
int RECORD_SECONDS = 5;
int bestIndex = -1;
int bestRSSI = -100;
int ReadySaid = 0;
static constexpr int PIN = 19;

// Pin mapping (adjust to your wiring)
#define I2S_BCK 9
#define I2S_WS 11
#define I2S_SD 14


/*-------------------------------- Button Handler ----------------------------------------*/
static void buttonHandler(uint8_t btnId, uint8_t btnState) {
  if (btnState == BTN_PRESSED) {
    Serial.println("Pushed button");

  } else {
    // btnState == BTN_OPEN.
    Serial.println("Released button");
    ReadySaid = 1;
  }
}

/*-------------------------------- END Button Handler ------------------------------------*/

static Button myButton(0, buttonHandler);

/*-------------------------------- Display Graphics ----------------------------------------*/
void showGraphic(String(image)) {
  uint32_t t = millis();

  setPngPosition(0, 0);
  String githubURL = GITHUBURL + image;
  const char* URLStr = githubURL.c_str();
  Serial.println(URLStr);
  load_png(URLStr);
  t = millis() - t;
  Serial.print(t);
  Serial.println(" ms to load URL");
}
/*--------------------------------------------------------------------------------------------*/

/*------------------------------- Setup ------------------------------------------------*/
// ==== Setup & Loop ====
void setup() {
  Serial.begin(115200);
  delay(1000);
  pinMode(PIN, INPUT_PULLUP);

  tft.begin();
  tft.invertDisplay(1);
  tft.fillScreen(0);
  tft.setRotation(2);
  tft.setTextSize(2);
  drawWrappedTextCentered("Starting ....", 10, 100, 200, 20);
  tft.setTextSize(1);
  connectToBestWiFi();
  
  fileInfo();
  delay(1000);
  Serial.println("\nWiFi connected!");

  if (psramFound()) {
    Serial.printf("PSRAM available: %d bytes\n", ESP.getPsramSize());
  } else {
    Serial.println("No PSRAM detected!");
  }

  Serial.println("\nset up microphone next");
  setupI2SMic();
  Serial.println("\nentering loop");
    
}

/*------------------------------- Loop ------------------------------------------------*/
void loop() {
  pollButtons();  // Poll your buttons every loop
  if (ReadySaid == 1) {
    Serial.println("Recording...");
    tft.fillScreen(TFT_BLUE);
  tft.setTextColor(TFT_WHITE);  // Print to TFT display, White color
  tft.setTextSize(2);
  drawWrappedTextCentered("Start Talking ....", 10, 100, 200, 20);
    captureAndSend();
    delay(10000);  // wait before next capture
    ReadySaid = 0;
  }
  yield();
}

/*------------------------------- Light weight multiWiFi------------------------------------------------*/

void connectToBestWiFi() {
  Serial.println("📡 Scanning for available networks...");
  int n = WiFi.scanNetworks();

  for (int i = 0; i < n; ++i) {
    for (int j = 0; j < numNetworks; ++j) {
      if (WiFi.SSID(i) == ssidList[j] && WiFi.RSSI(i) > bestRSSI) {
        bestRSSI = WiFi.RSSI(i);
        bestIndex = j;
      }
    }
  }

  if (bestIndex != -1) {
    Serial.printf("🔌 Connecting to %s (RSSI: %d)\n", ssidList[bestIndex], bestRSSI);
    WiFi.begin(ssidList[bestIndex], passList[bestIndex]);

    unsigned long startAttemptTime = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - startAttemptTime < 10000) {
      delay(250);
      Serial.print(".");
    }

    if (WiFi.status() == WL_CONNECTED) {
      Serial.printf("\n✅ Connected to %s\n", ssidList[bestIndex]);
      Serial.print("🌐 IP Address: ");
      Serial.println(WiFi.localIP());
      showGraphic("WiFiConnected.png");
      delay(3000);
    } else {
      Serial.println("\n❌ Connection failed. No usable network found.");
    }
  } else {
    Serial.println("⚠️ No known networks found.");
  }
}
/*------------------------------------ Button Debounce -------------------------------------*/

static void pollButtons() {
  // update() will call buttonHandler() if PIN transitions to a new state and stays there
  // for multiple reads over 25+ ms.
  myButton.update(digitalRead(PIN));
}

/*------------------------------------ I2S Setup ------------------------------------------*/
// ==== I2S Setup ====
void setupI2SMic() {
  i2s_config_t i2s_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = SAMPLE_RATE,
    .bits_per_sample = SAMPLE_BITS,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 8,
    .dma_buf_len = BUFFER_LEN
  };

  i2s_pin_config_t pin_config = {
    .bck_io_num = I2S_BCK,
    .ws_io_num = I2S_WS,
    .data_out_num = I2S_PIN_NO_CHANGE,
    .data_in_num = I2S_SD
  };

  i2s_driver_install(I2S_PORT, &i2s_config, 0, NULL);
  i2s_set_pin(I2S_PORT, &pin_config);
}

/*------------------------------------ Minimal JSON Parser ------------------------------------------*/

String extractTextField(const String& json) {
  int keyIndex = json.indexOf("\"text\"");
  if (keyIndex == -1) return "";

  int colonIndex = json.indexOf(":", keyIndex);
  if (colonIndex == -1) return "";

  int startQuote = json.indexOf("\"", colonIndex);
  if (startQuote == -1) return "";

  int endQuote = json.indexOf("\"", startQuote + 1);
  if (endQuote == -1) return "";

  return json.substring(startQuote + 1, endQuote);
}


// ==== WAV Header Helper ====
void writeWavHeader(uint8_t* header, int dataSize) {
  Serial.println("inside writewaveHeader");
  uint32_t fileSize = dataSize + 36;
  memcpy(header, "RIFF", 4);
  *(uint32_t*)(header + 4) = fileSize;
  memcpy(header + 8, "WAVEfmt ", 8);
  *(uint32_t*)(header + 16) = 16;  // PCM header size
  *(uint16_t*)(header + 20) = 1;   // PCM format
  *(uint16_t*)(header + 22) = 1;   // Mono
  *(uint32_t*)(header + 24) = SAMPLE_RATE;
  *(uint32_t*)(header + 28) = SAMPLE_RATE * 2;
  *(uint16_t*)(header + 32) = 2;   // Block align
  *(uint16_t*)(header + 34) = 16;  // Bits per sample
  memcpy(header + 36, "data", 4);
  *(uint32_t*)(header + 40) = dataSize;
}

/*------------------------------------ Capture Audio and Send ------------------------------------------*/
// ==== Capture Audio and Send ====
void captureAndSend() {
  const int totalSamples = SAMPLE_RATE * RECORD_SECONDS;
  const int dataSize = totalSamples * 2;  // 16-bit samples
  const int wavSize = dataSize + 44;

  // Allocate buffer in PSRAM
  uint8_t* wavBuffer = (uint8_t*)ps_malloc(wavSize);
  if (!wavBuffer) {
    Serial.printf("ps_malloc FAILED for %d bytes\n", wavSize);
    return;
  }

  Serial.printf("Allocated %d bytes in PSRAM\n", wavSize);

  // Write WAV header
  writeWavHeader(wavBuffer, dataSize);

  // Capture audio
  size_t bytesRead;
  int16_t* samplePtr = (int16_t*)(wavBuffer + 44);

  for (int i = 0; i < totalSamples; i += BUFFER_LEN) {
    int samplesThisChunk = min(BUFFER_LEN, totalSamples - i);
    size_t bytesThisChunk = samplesThisChunk * sizeof(int16_t);

    i2s_read(I2S_PORT, samplePtr + i, bytesThisChunk, &bytesRead, portMAX_DELAY);
  }

  Serial.println("Capture done, freeing buffer soon...");

  // -------------------------------
  //  HTTPS CHUNKED UPLOAD BEGINS
  // -------------------------------

  WiFiClientSecure secure;
  secure.setInsecure();  // MUST be before connect()

  if (!secure.connect("api.openai.com", 443)) {
    Serial.println("TLS connect failed");
    free(wavBuffer);
    return;
  }

  String boundary = "boundary123";

  // Multipart header (before WAV data)
  String mpHeader =
    "--" + boundary + "\r\n"
                      "Content-Disposition: form-data; name=\"model\"\r\n\r\n"
                      "gpt-4o-mini-transcribe\r\n"
                      "--"
    + boundary + "\r\n"
                 "Content-Disposition: form-data; name=\"file\"; filename=\"speech.wav\"\r\n"
                 "Content-Type: audio/wav\r\n\r\n";

  // HTTP request header
  String request =
    "POST /v1/audio/transcriptions HTTP/1.1\r\n"
    "Host: api.openai.com\r\n"
    "Authorization: Bearer "
    + String(chatGPT_APIKey_txt) + "\r\n"
                                   "Content-Type: multipart/form-data; boundary="
    + boundary + "\r\n"
                 "Transfer-Encoding: chunked\r\n"
                 "Connection: close\r\n\r\n";

  // Send HTTP header
  secure.print(request);

  // Send multipart header as chunk
  secure.printf("%X\r\n", mpHeader.length());
  secure.print(mpHeader);
  secure.print("\r\n");
  secure.flush();

  // Send WAV data in chunks
  const size_t CHUNK = 4096;
  for (size_t i = 0; i < wavSize; i += CHUNK) {
    size_t n = min(CHUNK, wavSize - i);
    secure.printf("%X\r\n", n);
    secure.write(wavBuffer + i, n);
    secure.print("\r\n");
    secure.flush();
  }

  // Final boundary
  String tail = "\r\n--" + boundary + "--";
  secure.printf("%X\r\n", tail.length());
  secure.print(tail);
  secure.print("\r\n");
  secure.flush();

  // End of chunked transfer
  secure.print("0\r\n\r\n");
  secure.flush();

  // Wait for server response
  String payload;
  unsigned long start = millis();
  while (!secure.available() && millis() - start < 5000) {
    delay(10);
  }

  while (secure.available()) {
    payload += secure.readString();
  }

  Serial.println("Response:");
  Serial.println(payload);

  // Extract transcription
  String text = extractTextField(payload);
  Serial.println("Transcribed text: " + text);
  tft.fillScreen(TFT_BLUE);
  tft.setTextColor(TFT_WHITE);  // Print to TFT display, White color
  tft.setTextSize(2);
  //String transcript = "This is a long sentence that needs to wrap cleanly on a small round display.";

  drawWrappedTextCentered(text, 10, 100, 200, 20);

  secure.stop();
  free(wavBuffer);
}
void drawWrappedTextCentered(const String& text, int x, int y, int maxWidth, int lineHeight) {
  String currentLine = "";
  String currentWord = "";
  int cursorY = y;

  for (int i = 0; i < text.length(); i++) {
    char c = text[i];

    if (c == ' ' || c == '\n') {
      int lineWidth = tft.textWidth(currentLine + currentWord);

      if (lineWidth > maxWidth) {
        // Center this line
        int centeredX = x + (maxWidth - tft.textWidth(currentLine)) / 2;
        tft.drawString(currentLine, centeredX, cursorY);
        cursorY += lineHeight;

        currentLine = currentWord + " ";
      } else {
        currentLine += currentWord + " ";
      }

      currentWord = "";

      if (c == '\n') {
        int centeredX = x + (maxWidth - tft.textWidth(currentLine)) / 2;
        tft.drawString(currentLine, centeredX, cursorY);
        cursorY += lineHeight;
        currentLine = "";
      }
    } else {
      currentWord += c;
    }
  }

  // Draw any remaining text
  if (currentWord.length() > 0) {
    if (tft.textWidth(currentLine + currentWord) > maxWidth) {
      int centeredX = x + (maxWidth - tft.textWidth(currentLine)) / 2;
      tft.drawString(currentLine, centeredX, cursorY);
      cursorY += lineHeight;

      int centeredX2 = x + (maxWidth - tft.textWidth(currentWord)) / 2;
      tft.drawString(currentWord, centeredX2, cursorY);
    } else {
      String finalLine = currentLine + currentWord;
      int centeredX = x + (maxWidth - tft.textWidth(finalLine)) / 2;
      tft.drawString(finalLine, centeredX, cursorY);
    }
  }
}


/*---------------------------- File information  ------------------------------------------*/
void fileInfo() {  // uesful to figure our what software is running

  tft.fillScreen(TFT_BLUE);
  tft.setTextColor(TFT_WHITE);  // Print to TFT display, White color
  tft.setTextSize(1);
  tft.drawString("       Speech to Text", 8, 60);
  tft.drawString("       Google Translate", 10, 70);
  tft.setTextSize(1);
  tft.drawString(__FILENAME__, 30, 110);
  tft.drawString(__DATE__, 35, 140);
  tft.drawString(__TIME__, 125, 140);
  tft.drawString("Press Button to Start!", 50, 190);
  //delay(3000);
}