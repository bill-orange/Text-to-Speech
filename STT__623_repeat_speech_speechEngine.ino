/*
Speech to Text Oracle Sketch

Must run on ESP32S3 with PSRAM tested with Huge partition scheme and a 16R8 chip

   William E Webb (c) 2926 released under MIT license please observe all included licenses

12/17/2025 Initial untested code mostly by Microsoft Copilot
12/23/2025 Added button activation.  All untested!
12/24/2025 CaptureAndSend completely rewritten.  Tested and it works.
12/26/2024 New I2S drivers New fork for some speaker work which is untested
12/27/2025 New corrected case driven loop
           Working properly but some minor cleanup is still needed.
12/30/2025 Silence Detection added, works 
           Adding equilization, trimming, Adaptive threshold
           Adaptive threshold not fully tested
           Forked for enhancd Jason deserializer (untested)
12/31/2025 Forked for improved Speech Engine
01/01/2026 Added "One‑Deep Playback Queue”
           Forked to remove prints and minors to loop())
           Forked again to Oracle version
01/02/2026 Revised for bugfix in prompt syntax and stability enhancements
           Graphic of oracle added
01/03/2026 Stuttering removed: for (int i = 0; i < 3000; i++) {  // ~5ms at 16kHz
           i2s_channel_write(tx_chan, &silence, sizeof(silence), &written, portMAX_DELAY);
           Untested error function added.
01/04/2026 Adding errorHandler() calls and alternate behavior on long press.
01/05/2025 speechEngine() rewrite for ring‑buffer architecture
01/07/2026 mic code rewritten for ring-buffer architecture and various fixes
01/08/2026 added back in equlizer -- tested
01/08/2026 forked for repeat the prompt.  Workingbut the prompt needs work
01/15/2026 improve word wrap add pre roll loop
01/16/2026 Forked for end of speech detection
01/17/2026 Kill switches for ineffective functions added

*/

#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include "driver/i2s_std.h"
#include "driver/i2s_common.h"
i2s_chan_handle_t rx_chan;  // global RX handle
i2s_chan_handle_t tx_chan;  // Speaker (TX) channel handle
#include "secrets.h"
#include <debounce.h>  // Button debounce

#include <TFT_eSPI.h>           // Hardware-specific library
TFT_eSPI tft = TFT_eSPI();      // For Display
#include "support_functions.h"  // For PNG

enum OracleError {
  ERR_NO_INTERNET,
  ERR_LOST_INTERNET,
  ERR_UNREACHABLE_SERVICE,
  ERR_UNKNOWN
};

// -------------------- STATE MACHINE --------------------
enum State {
  IDLE,
  PROMPT_USER,
  WAIT_PROMPT_DONE,
  RECORDING,
  WAIT_ASR,
  SHOW_TRANSCRIPTION,
  ORACLE_REPLY,
  WAIT_REPLY_DONE
};


enum UploadState {
  UPLOAD_IDLE,
  UPLOAD_START,
  UPLOAD_CONNECTING,
  UPLOAD_SENDING_HEADERS,
  UPLOAD_SENDING_BODY,
  UPLOAD_WAIT_RESPONSE,
  UPLOAD_DONE,
  UPLOAD_ERROR
};

bool DEBUG = false;

UploadState uploadState = UPLOAD_IDLE;

String pendingASR = "";
bool hasPendingASR = false;

WiFiClientSecure uploadClient;
size_t bytesSent = 0;

State state = IDLE;
unsigned long stateStart = 0;

struct UploadJob {
  uint8_t* wavData;
  size_t wavSize;
};

UploadJob uploadQueue[2];  // small queue is enough
int uploadHead = 0;
int uploadTail = 0;

bool queueIsEmpty() {
  return uploadHead == uploadTail;
}
bool queueIsFull() {
  return ((uploadTail + 1) % 2) == uploadHead;
}

SET_LOOP_TASK_STACK_SIZE(14 * 1024);  // needed to handle really long strings

#define ENABLE_TTS       // comment this out to disable speechEngine()
#define USE_LINE_BUFFER  // Enable for faster rendering
#define __FILENAME__ (strrchr(__FILE__, '\\') ? strrchr(__FILE__, '\\') + 1 : __FILE__)

// ---------------------------------------------------------
//             I2S Config
// ---------------------------------------------------------
#define I2S_PORT I2S_NUM_0
#define SAMPLE_RATE 16000
#define SAMPLE_BITS I2S_BITS_PER_SAMPLE_16BIT
#define BUFFER_LEN 1024

// ---------------------------------------------------------
//  AUDIO CAPTURE + SIGNAL ANALYSIS
// ---------------------------------------------------------
int RECORD_SECONDS = 8;  // Max duration of microphone recording window
float gain = 0.6;        // Microphone gain multiplier for RMS analysis
long sumSquares = 0;     // Accumulator for RMS energy calculation
long sampleCount = 0;    // Number of samples collected for RMS

// ---------------------------------------------------------
//  WIFI SELECTION (best AP logic)
// ---------------------------------------------------------
int bestIndex = -1;   // Index of strongest WiFi network found
int bestRSSI = -100;  // RSSI of strongest AP (initialized very low)

// ---------------------------------------------------------
//  BUTTON / USER INPUT
// ---------------------------------------------------------
int ReadySaid = 0;  // Set to 1 when the user presses the activation button
int alternateText = 0;

// ---------------------------------------------------------
//  HARDWARE PINS
// ---------------------------------------------------------
static constexpr int PIN = 19;  // Microphone or I2S data pin (depending on wiring)

// ---------------------------------------------------------
//  TTS PLAYBACK STATE + QUEUE
// ---------------------------------------------------------
volatile bool isPlaying = false;  // True while TTS audio is actively playing
volatile bool hasQueued = false;  // True if a second TTS request is waiting

String queuedPhrase = "";        // Queued TTS text (one-deep queue)
String queuedVoice = "";         // Queued TTS voice model
float queuedSpeed = 1.0;         // Queued TTS playback speed
String queuedInstructions = "";  // Queued TTS style instructions
String oraclePrompt = " ";       // Prompt for Gemini

// ---------------------------------------------------------
//  ORACLE INTRODUCTION + UI BEHAVIOR
// ---------------------------------------------------------
bool oracleIntroduced = false;  // (Legacy) Not used now — replaced by introSpoken
bool introSpoken = false;       // True once the Oracle has spoken its pompous intro

bool showTranscription = true;    // If true, show user’s spoken text on screen
bool transcriptionDrawn = false;  // Prevents flicker: ensures transcription is drawn once
bool tftInitialized = true;
bool ttsAvailable = true;

String lastTranscription = "";  // Stores the most recent Whisper transcription
String spoken = "";             // Global buffer for the current spoken text

// ---------------------------------------------------------
//             Pin mapping (microphome)
// ---------------------------------------------------------
#define I2S_BCK 9
#define I2S_WS 11
#define I2S_SD 14

// ---------------------------------------------------------
//                Gemini Prompt
// ---------------------------------------------------------
String mirrorInstruction =
  "You are Mirror Bot. "
  "Your only function is to repeat the user's text exactly as given, "
  "but with grammar, spelling, or punctuation corrections. "
  "Do not add words. "
  "Do not remove words. "
  "Do not reorder words. "
  "Do not continue sequences such as numbers or lists. "
  "Do not infer intent. "
  "Do not explain anything. "
  "Do not answer questions. "
  "Do not be conversational. "
  "Follow these examples exactly:\n"
  "User: Apple\n"
  "Model: Apple\n"
  "User: How are you today?\n"
  "Model: How are you today?\n"
  "User: one two three four five\n"
  "Model: one two three four five\n"
  "User: I ain't got nuthen\n"
  "Model: I do not have anything.\n"
  "User: I am thinking more harder.\n"
  "Model: I am thinking harder.\n"
  "User: What is these?\n"
  "Model: What are these?\n";

// ---------------------------------------------------------
//           Word Wrap Globals
// ---------------------------------------------------------

bool USE_CIRCULAR_TEXT = true;   // set false to revert to rectangular mode
const int DISPLAY_CX = 120;      // center X of your round TFT
const int DISPLAY_CY = 120;      // center Y
const int DISPLAY_RADIUS = 120;  // radius

// ---------------------------------------------------------
//             PreRoll
// ---------------------------------------------------------
// 200 ms at 8 kHz, 16-bit mono = 1600 samples = 3200 bytes

bool usePreRoll = true;
const int PREROLL_SAMPLES = 3200 * 5;
int16_t preRollBuffer[PREROLL_SAMPLES];
volatile int preRollIndex = 0;

// ---------------------------------------------------------
//             End-of-Speech
// ---------------------------------------------------------
// End-of-speech detection

bool useEndOfSpeech = false;
int silenceChunks = 0;
const int CHUNK_MS = (BUFFER_LEN * 1000) / SAMPLE_RATE;  // e.g. 32 ms
const int SILENCE_MS_REQUIRED = 200;                     // tune 300–600 ms
const int SILENCE_CHUNKS_REQUIRED = SILENCE_MS_REQUIRED / CHUNK_MS;

// ---------------------------------------------------------
// Forward Declaration
// ---------------------------------------------------------
void speechEngine(const char* phrase, const char* voice, float speed, const char* instructions);

/*------------------------------------ PreRoll Helper -------------------------------------*/

void updatePreRoll(int16_t sample) {
  preRollBuffer[preRollIndex] = sample;
  preRollIndex = (preRollIndex + 1) % PREROLL_SAMPLES;
}

/*------------------------------------ Wav Helper -------------------------------------*/

void enqueueWavForUpload(uint8_t* wavData, size_t wavSize) {
  if (queueIsFull()) {
    Serial.println("⚠️ Upload queue full — dropping job");
    free(wavData);
    return;
  }

  uploadQueue[uploadTail].wavData = wavData;
  uploadQueue[uploadTail].wavSize = wavSize;
  uploadTail = (uploadTail + 1) % 2;

  if (DEBUG) Serial.printf("📥 Enqueued WAV upload (%u bytes)\n", wavSize);
}

/* --------------------------------------------------------------------------
   ASYNC WHISPER UPLOADER — NON‑BLOCKING MULTIPART STATE MACHINE
   --------------------------------------------------------------------------

   The uploader runs independently of the main Oracle state machine.
   It never blocks audio playback, UI updates, or user interaction.
   All network operations are broken into small, incremental steps.

   OVERVIEW:

       UPLOAD_IDLE
         ↓ (queue has a job)
       UPLOAD_START
         ↓
       UPLOAD_CONNECTING
         ↓
       UPLOAD_SENDING_HEADERS
         ↓
       UPLOAD_SENDING_BODY
         ↓
       UPLOAD_WAIT_RESPONSE
         ↓
       UPLOAD_DONE → UPLOAD_IDLE

       (Any failure → UPLOAD_ERROR → UPLOAD_IDLE)

   BEHAVIOR:

   • Jobs are enqueued by RECORDING when a WAV buffer is ready.
   • Only one job is processed at a time (two‑slot ring buffer).
   • The uploader uses a persistent WiFiClientSecure connection.
   • Multipart/form‑data boundaries are fully RFC‑compliant.
   • The WAV file is streamed in 2 KB chunks to avoid blocking.
   • The final boundary is sent only after all bytes are written.
   • The uploader waits for Whisper’s JSON response without blocking.
   • When a transcription is extracted, it sets:
         pendingASR = "<text>"
         hasPendingASR = true
     The main loop consumes this in the ASR handoff block.

   GUARANTEES:

   • Never blocks the main loop.
   • Never interferes with TTS playback.
   • Never interrupts the Oracle state machine.
   • Always frees WAV buffers after upload completes.
   • Always returns to UPLOAD_IDLE after success or failure.
   • ASR handoff is decoupled from network timing.
   • Multipart boundaries match header/body/footer exactly.

   ERROR HANDLING:

   • Any connection failure → UPLOAD_ERROR.
   • Any malformed response → UPLOAD_ERROR.
   • Errors never stall the system; the job is discarded safely.
   • The Oracle continues functioning normally even if Whisper fails.

   -------------------------------------------------------------------------- */

/*------------------------------------ Upload State Machine -------------------------------------*/

void processUploadStateMachine() {
  switch (uploadState) {

    // ---------------------------------------------------------------------
    case UPLOAD_IDLE:
      if (!queueIsEmpty()) {
        Serial.println("UPLOAD_IDLE → UPLOAD_START");
        uploadState = UPLOAD_START;
      }
      break;

    // ---------------------------------------------------------------------
    case UPLOAD_START:
      if (DEBUG) Serial.println("STATE: UPLOAD_START");
      bytesSent = 0;
      uploadClient.setInsecure();
      uploadState = UPLOAD_CONNECTING;
      break;

    // ---------------------------------------------------------------------
    case UPLOAD_CONNECTING:
      if (DEBUG) Serial.println("STATE: UPLOAD_CONNECTING");

      if (!uploadClient.connected()) {
        if (DEBUG) Serial.println("  → calling uploadClient.connect(api.openai.com, 443)...");

        if (!uploadClient.connect("api.openai.com", 443)) {
          Serial.println("❌ Connect failed");
          uploadState = UPLOAD_ERROR;
          break;
        }

        if (DEBUG) Serial.println("  ✅ Connected to api.openai.com:443");
      }

      uploadState = UPLOAD_SENDING_HEADERS;
      break;

    // ---------------------------------------------------------------------
    case UPLOAD_SENDING_HEADERS:
      {
        if (DEBUG) Serial.println("STATE: UPLOAD_SENDING_HEADERS");
        UploadJob& job = uploadQueue[uploadHead];

        // IMPORTANT: boundary must match header AND body
        String boundary = "----oracleBoundary123";  // 4 hyphens

        // Multipart header
        String mpHeader =
          "--" + boundary + "\r\n"
                            "Content-Disposition: form-data; name=\"model\"\r\n\r\n"
                            "gpt-4o-mini-transcribe\r\n"
                            "--"
          + boundary + "\r\n"
                       "Content-Disposition: form-data; name=\"file\"; filename=\"speech.wav\"\r\n"
                       "Content-Type: audio/wav\r\n\r\n";

        // Multipart footer
        String mpFooter =
          "\r\n--" + boundary + "--\r\n";

        // Total content length
        size_t contentLength =
          mpHeader.length() + job.wavSize + mpFooter.length();

        // HTTP header
        String header =
          "POST /v1/audio/transcriptions HTTP/1.1\r\n"
          "Host: api.openai.com\r\n"
          "Authorization: Bearer "
          + String(chatGPT_APIKey_txt) + "\r\n"
                                         "Content-Type: multipart/form-data; boundary="
          + boundary + "\r\n"
                       "Content-Length: "
          + String(contentLength) + "\r\n"
                                    "Connection: close\r\n\r\n";

        if (DEBUG) Serial.printf("  → Sending HTTP headers + multipart header (%u bytes body)\n",
                                 job.wavSize);

        uploadClient.print(header);
        uploadClient.print(mpHeader);

        bytesSent = 0;
        uploadState = UPLOAD_SENDING_BODY;
        break;
      }

    // ---------------------------------------------------------------------
    case UPLOAD_SENDING_BODY:
      {
        UploadJob& job = uploadQueue[uploadHead];

        const size_t CHUNK = 2048;
        size_t remaining = job.wavSize - bytesSent;
        size_t n = min(CHUNK, remaining);

        if (n > 0) {
          /*Serial.printf("STATE: UPLOAD_SENDING_BODY — sending %u bytes (remaining %u)\n",
                        (unsigned)n, (unsigned)remaining);*/
          uploadClient.write(job.wavData + bytesSent, n);
          bytesSent += n;
        }

        if (bytesSent >= job.wavSize) {
          if (DEBUG) Serial.println("  → All WAV bytes sent, sending multipart footer");

          String boundary = "----oracleBoundary123";
          String mpFooter = "\r\n--" + boundary + "--\r\n";

          uploadClient.print(mpFooter);

          uploadState = UPLOAD_WAIT_RESPONSE;
        }
        break;
      }

    // ---------------------------------------------------------------------
    case UPLOAD_WAIT_RESPONSE:
      //Serial.println("STATE: UPLOAD_WAIT_RESPONSE");

      if (uploadClient.available()) {
        String response = uploadClient.readString();
        //Serial.println("----- ASR RAW RESPONSE -----");
        //Serial.println(response);

        // Extract transcription
        int idx = response.indexOf("\"text\":");
        if (idx >= 0) {
          int start = response.indexOf("\"", idx + 7) + 1;
          int end = response.indexOf("\"", start);

          if (start > 0 && end > start) {
            pendingASR = response.substring(start, end);
            hasPendingASR = true;
            Serial.printf("📥 Extracted transcription: %s\n", pendingASR.c_str());
          }
        }

        uploadClient.stop();
        free(uploadQueue[uploadHead].wavData);
        uploadHead = (uploadHead + 1) % 2;

        uploadState = UPLOAD_DONE;
      }
      break;

    // ---------------------------------------------------------------------
    case UPLOAD_DONE:
      if (DEBUG) Serial.println("STATE: UPLOAD_DONE → UPLOAD_IDLE");
      uploadState = UPLOAD_IDLE;
      break;

    // ---------------------------------------------------------------------
    case UPLOAD_ERROR:
      Serial.println("STATE: UPLOAD_ERROR — skipping job");
      free(uploadQueue[uploadHead].wavData);
      uploadHead = (uploadHead + 1) % 2;
      uploadState = UPLOAD_IDLE;
      break;
  }
}

/*------------------------------------ Error Handler -------------------------------------*/

void errorHandler(OracleError err) {

  isPlaying = false;
  hasQueued = false;

  String msg;
  String speakMsg;
  bool canSpeak = true;

  switch (err) {
    case ERR_NO_INTERNET:
      msg = "No internet connection.";
      speakMsg = "I sense no connection to the beyond.";
      canSpeak = false;
      tft.fillScreen(0);
      tft.setRotation(2);
      tft.setTextSize(2);
      drawWrappedTextCentered(speakMsg, 10, 110, 200, 20);
      delay(3000);
      break;

    case ERR_LOST_INTERNET:
      msg = "Connection lost.";
      speakMsg = "The link to the ether has been severed.";
      canSpeak = false;
      tft.fillScreen(0);
      tft.setRotation(2);
      tft.setTextSize(2);
      drawWrappedTextCentered(speakMsg, 10, 110, 200, 20);
      delay(3000);
      break;

    case ERR_UNREACHABLE_SERVICE:
      msg = "A service is unreachable.";
      speakMsg = "A distant service refuses to answer.";
      canSpeak = true;
      break;

    default:
      msg = "Unknown error.";
      speakMsg = "An unknown disturbance clouds my vision.";
      canSpeak = true;
      break;
  }

  Serial.println("⚠️ ERROR: " + msg);

  if (canSpeak) {
    isPlaying = true;
    speechEngine(speakMsg.c_str(), "verse", 1.0, "error");
  } else {
    state = IDLE;  // <-- YES, this is the correct place
  }
}

/*------------------------------------ Compute RMS -------------------------------------*/
// Computes RMS of a PCM buffer (16-bit signed samples)

float computeRms(int16_t* samples, int numSamples) {
  if (numSamples <= 0) return 0.0f;

  double sumSq = 0.0;

  for (int i = 0; i < numSamples; i++) {
    float v = (float)samples[i];
    sumSq += v * v;
  }

  float meanSq = sumSq / numSamples;
  return sqrtf(meanSq);
}

/*------------------------------------ Automatic Gain Control (AGC) -------------------------------------*/
// Simple peak-based AGC with smoothing.
// targetLevel: desired peak amplitude (e.g., 12000)
// maxGain: safety limit (e.g., 8.0f)

void applyAGC(int16_t* samples, int numSamples, float targetLevel = 12000.0f, float maxGain = 8.0f) {
  // 1. Find peak amplitude
  float peak = 0.0f;
  for (int i = 0; i < numSamples; i++) {
    float v = fabsf((float)samples[i]);
    if (v > peak) peak = v;
  }

  if (peak < 1.0f) peak = 1.0f;  // avoid divide-by-zero

  // 2. Compute desired gain
  float gain = targetLevel / peak;
  if (gain > maxGain) gain = maxGain;

  // 3. Apply gain with clamping
  for (int i = 0; i < numSamples; i++) {
    float y = samples[i] * gain;

    if (y > 32767.0f) y = 32767.0f;
    if (y < -32768.0f) y = -32768.0f;

    samples[i] = (int16_t)y;
  }
}

/*------------------------------------ Trim Leading & Trailing Silence -------------------------------------*/
// Removes silence at the start and end of the PCM buffer.
// Returns the new number of samples after trimming.

int trimSilence(int16_t* samples, int numSamples, int16_t threshold = 150) {
  int start = 0;
  int end = numSamples - 1;

  // 1. Find first non-silent sample
  while (start < numSamples && abs(samples[start]) < threshold) {
    start++;
  }

  // 2. Find last non-silent sample
  while (end > start && abs(samples[end]) < threshold) {
    end--;
  }

  int newLength = end - start + 1;
  if (newLength < 0) newLength = 0;

  Serial.printf(
    "Trim: start=%d, end=%d, removedStart=%d, removedEnd=%d, newLen=%d\n",
    start,
    end,
    start,
    (numSamples - 1) - end,
    newLength);

  // 3. Move trimmed audio to the front of the buffer
  if (start > 0 && newLength > 0) {
    memmove(samples, samples + start, newLength * sizeof(int16_t));
  }

  return newLength;
}

/*------------------------------------ Adaptive Trim Threshold -------------------------------------*/
// Computes an adaptive silence threshold based on ambient noise.
// Uses the first 300ms of audio as the ambient sample.

float computeAdaptiveThreshold(int16_t* samples, int numSamples) {
  int ambientSamples = 4800;  // 0.3 sec at 16kHz
  if (ambientSamples > numSamples) ambientSamples = numSamples;

  float ambientRMS = computeRms(samples, ambientSamples);

  // Scale ambient noise to a usable threshold
  float threshold = ambientRMS * 8.0f;

  // Clamp to safe bounds
  if (threshold < 80) threshold = 80;
  if (threshold > 300) threshold = 300;

  Serial.printf("Adaptive threshold = %.1f (ambientRMS=%.1f)\n",
                threshold, ambientRMS);

  return threshold;
}

/* ------------------------------------ Portable EQ: Low-pass + Bass Boost---------------------*/
// Portable EQ: Low-pass + Bass Boost
// Applies a simple 16-tap FIR low-pass filter (~2 kHz) and a low-shelf boost.
// -----------------------------------------------------------------------------

void applyEQ(int16_t* samples, int numSamples) {
  // 16-tap low-pass FIR (generated for 16 kHz sample rate, cutoff ~2 kHz)
  static const float fir[16] = {
    -0.0102, -0.0128, 0.0000, 0.0365,
    0.0897, 0.1420, 0.1735, 0.1678,
    0.1180, 0.0305, -0.0739, -0.1672,
    -0.2183, -0.2064, -0.1280, -0.0000
  };

  float z[16] = { 0 };

  for (int i = 0; i < numSamples; i++) {
    // Shift delay line
    for (int j = 15; j > 0; j--) {
      z[j] = z[j - 1];
    }
    z[0] = samples[i];

    // FIR convolution
    float y = 0;
    for (int j = 0; j < 16; j++) {
      y += fir[j] * z[j];
    }

    // Bass boost (simple low-shelf)
    float bass = samples[i] * 0.20f;  // +20% low-end warmth
    y += bass;

    // Clamp
    if (y > 32767.0f) y = 32767.0f;
    if (y < -32768.0f) y = -32768.0f;

    samples[i] = (int16_t)y;
  }
}


/*-------------------------------- Button Handler ----------------------------------------*/
static void buttonHandler(uint8_t btnId, uint8_t btnState) {
  if (btnState == BTN_PRESSED) {
    Serial.println("Pushed button");
    alternateText = 0;

  } else {
    // btnState == BTN_OPEN.
    Serial.println("Released button");
    ReadySaid = 1;
  }
}

static void longPressAndReleaseHandler(uint8_t btnId, uint8_t btnState) {
  if (btnState == BTN_PRESSED) {
    Serial.println("Long Press");
  } else {
    // btnState == BTN_OPEN.
    // Shown when the button is released, but only if held down 3s first.
    Serial.println("Long Press Released");
    ReadySaid = 1;
    alternateText = 1;
  }
}

/*-------------------------------- END Button Handler ------------------------------------*/

static Button myButton(0, buttonHandler);
static Button longPressReleaseBtn(2, longPressAndReleaseHandler);

/*-------------------------------- Display Graphics ----------------------------------------*/
void showGraphic(String(image)) {
  uint32_t t = millis();

  setPngPosition(0, 0);
  String githubURL = GITHUBURL + image;
  const char* URLStr = githubURL.c_str();
  if (DEBUG) Serial.println(URLStr);
  load_png(URLStr);
  t = millis() - t;
  if (DEBUG) Serial.print(t);
  if (DEBUG) Serial.println(" ms to load URL");
}
/*--------------------------------------------------------------------------------------------*/

/*------------------------------- Setup ------------------------------------------------*/
// ==== Setup & Loop ====
void setup() {
  Serial.begin(115200);
  delay(1000);
  pinMode(PIN, INPUT_PULLUP);
  longPressReleaseBtn.setPushDebounceInterval(3000);  // 3 seconds

  tft.begin();
  tft.invertDisplay(1);
  tft.fillScreen(0);
  tft.setRotation(2);
  tft.setTextSize(2);
  drawWrappedTextCentered("Starting ....", 10, 110, 200, 20);
  tft.setTextSize(1);
  connectToBestWiFi();

  fileInfo();
  delay(1000);
  Serial.println("\nWiFi connected!");

  if (psramFound()) {
    if (DEBUG) Serial.printf("PSRAM available: %d bytes\n", ESP.getPsramSize());
  } else {
    Serial.println("No PSRAM detected!");
  }
  if (DEBUG) Serial.println("\nset up microphone next");
  setupI2SMic();
  setupI2SSpeaker();
  Serial.println("\nentering loop");
  state = IDLE;
  stateStart = millis();
}

/* --------------------------------------------------------------------------
   MAIN LOOP — CLEAN REWRITE (with async Whisper upload)
   --------------------------------------------------------------------------

   This version uses a strict, symmetrical, non‑blocking state machine.
   All long‑running operations (TTS playback, Whisper upload, Gemini query)
   are handled asynchronously, and every action has a corresponding WAIT state.

   OVERALL FLOW:

       IDLE
         ↓ (button press)
       INTRO → WAIT_INTRO_DONE
         ↓
       PROMPT_USER → WAIT_PROMPT_DONE
         ↓
       RECORDING
         ├── silence detected
         │       ↓
         │   WAIT_SILENCE_DONE → IDLE
         │
         └── speech detected
                 ↓
             (WAV enqueued for async upload)
                 ↓
             WAIT_ASR   ← Whisper upload + JSON parse happen in background
                 ↓ (ASR handoff fires)
             SHOW_TRANSCRIPTION
                 ↓ (timeout)
             ORACLE_REPLY → WAIT_REPLY_DONE → IDLE

   KEY GUARANTEES:

   • RECORDING runs exactly once per user utterance.
   • WAIT_ASR freezes the state machine until Whisper finishes.
   • ASR handoff always runs before any queued TTS can fire.
   • The TTS queue handler is disabled during WAIT_ASR.
   • No double‑capture, no queue overflow, no premature TTS.
   • All TTS actions have corresponding WAIT states.
   • No state ever falls through into another TTS state.
   • IDLE is only entered when no audio is playing and no ASR is pending.
   • The async uploader never blocks UI, audio playback, or the state machine.

   -------------------------------------------------------------------------- */
/*------------------------------- Loop ------------------------------------------------*/
void loop() {
  pollButtons();
  processUploadStateMachine();

  switch (state) {

      // ---------------------------------------------------------
      //  IDLE — reset cycle, wait for button press
      // ---------------------------------------------------------
    case IDLE:
      // Only accept button press when upload machine is idle
      if (ReadySaid == 1 && uploadState == UPLOAD_IDLE) {
        ReadySaid = 0;

        tft.fillScreen(TFT_BLACK);
        //drawWrappedTextCentered("Start Talking ....",
        //                        10, 110, 200, 20);
        showGraphic("Female_Ham.png");

        speechEngine("Start Talking",
                     "echo",
                     0.90,
                     "Speak with an excessively cheerful, upbeat voice.");

        state = WAIT_PROMPT_DONE;
        stateStart = millis();
      }
      break;

    // ---------------------------------------------------------
    //  WAIT_PROMPT_DONE — wait for TTS to finish
    // ---------------------------------------------------------
    case WAIT_PROMPT_DONE:
      if (!isPlaying) {
        state = RECORDING;
        stateStart = millis();
      }
      break;

    // ---------------------------------------------------------
    //  RECORDING — capture + upload
    // ---------------------------------------------------------
    case RECORDING:
      if (millis() - stateStart > 200) {

        String spoken = captureAndSend();

        if (spoken == "__SILENCE__") {
          tft.fillScreen(TFT_BLACK);
          drawWrappedTextCentered("I didn't hear anything.",
                                  10, 110, 210, 20);

          speechEngine("I didn't hear anything.",
                       "alloy",
                       0.90,
                       "Speak clearly.");

          state = IDLE;
          break;
        }

        if (spoken == "__UPLOAD_QUEUED__") {
          if (DEBUG) Serial.println("⏳ Waiting for ASR result...");
          state = WAIT_ASR;
          stateStart = millis();
          break;
        }

        lastTranscription = spoken;
        state = SHOW_TRANSCRIPTION;
        stateStart = millis();
      }
      break;

    // ---------------------------------------------------------
    //  WAIT_ASR — wait for Whisper to finish
    // ---------------------------------------------------------
    case WAIT_ASR:
      if (hasPendingASR) {
        lastTranscription = pendingASR;
        hasPendingASR = false;

        tft.fillScreen(TFT_BLACK);
        drawWrappedTextCentered(lastTranscription.c_str(),
                                10, 30, 210, 20);

        state = SHOW_TRANSCRIPTION;
        stateStart = millis();
      }
      break;

    // ---------------------------------------------------------
    //  SHOW_TRANSCRIPTION — brief pause
    // ---------------------------------------------------------
    case SHOW_TRANSCRIPTION:
      if (millis() - stateStart > 1200) {
        state = ORACLE_REPLY;
        stateStart = millis();
      }
      break;

    // ---------------------------------------------------------
    //  ORACLE_REPLY — grammar correction
    // ---------------------------------------------------------
    case ORACLE_REPLY:
      {

        float GEMINI_TEMPERATURE = 0.2;
        String reply = askGemini(lastTranscription, mirrorInstruction, GEMINI_TEMPERATURE);

        String safe = sanitizeQuip(reply.c_str());

        /*speechEngine(
        safe.c_str(),
        "alloy",
        0.90,
        "Repeat the input text with the exact same meaning and structure, "
        "but correct grammar, spelling, and obvious mistakes. Do not rewrite, "
        "summarize, expand, or change the intent."
      );
*/
        speechEngine(safe.c_str(), "alloy", 0.90, " remove profanity and substitute something else and speak with calm assurance");
      };

    // ---------------------------------------------------------
    //  WAIT_REPLY_DONE — wait for TTS to finish
    // ---------------------------------------------------------
    case WAIT_REPLY_DONE:
      if (!isPlaying) {
        state = IDLE;
      }
      break;
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

    } else {
      Serial.println("\n❌ Connection failed. No usable network found.");
      errorHandler(ERR_NO_INTERNET);
    }
  } else {
    Serial.println("⚠️ No known networks found.");
    errorHandler(ERR_NO_INTERNET);
  }
}
/*------------------------------------ Button Debounce -------------------------------------*/

static void pollButtons() {
  // update() will call buttonHandler() if PIN transitions to a new state and stays there
  // for multiple reads over 25+ ms.
  myButton.update(digitalRead(PIN));
  longPressReleaseBtn.update(digitalRead(PIN));
}

/*------------------------------------ Mic setup -------------------------------------*/
void setupI2SMic() {

  i2s_chan_config_t chan_cfg = {
    .id = I2S_NUM_0,
    .role = I2S_ROLE_MASTER,
    .dma_desc_num = 8,
    .dma_frame_num = BUFFER_LEN,
    .auto_clear = true
  };

  // Create RX-only channel
  i2s_new_channel(&chan_cfg, NULL, &rx_chan);

  // Use 32-bit slots so we can extract 24-bit mic data
  i2s_std_config_t std_cfg = {
    .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),

    .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(
      I2S_DATA_BIT_WIDTH_32BIT,  // <-- 32-bit frame
      I2S_SLOT_MODE_MONO         // <-- mono
      ),

    .gpio_cfg = {
      .mclk = I2S_GPIO_UNUSED,
      .bclk = (gpio_num_t)I2S_BCK,
      .ws = (gpio_num_t)I2S_WS,
      .dout = I2S_GPIO_UNUSED,
      .din = (gpio_num_t)I2S_SD }
  };

  // Force LEFT channel (your mic outputs on left)
  std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;

  i2s_channel_init_std_mode(rx_chan, &std_cfg);
  i2s_channel_enable(rx_chan);
}

/*------------------------------------ Improved Minimal JSON Parser ------------------------------------------*/

// Extracts ALL meaningful text fields from a Gemini JSON response.
// Handles multiple formats: "text", "output_text", "content", nested parts, etc.
String extractAllTextFields(const String& json) {
  String result = "";
  int searchPos = 0;

  while (true) {
    // Find the next "text" key
    int keyIndex = json.indexOf("\"text\"", searchPos);
    if (keyIndex == -1) break;

    // Ensure this is a real text field, not "parts"
    // Look backwards to see if this is inside a "parts" array
    int partsIndex = json.lastIndexOf("\"parts\"", keyIndex);
    if (partsIndex != -1 && partsIndex > keyIndex - 20) {
      // Skip this occurrence — it's the "parts" key, not a text field
      searchPos = keyIndex + 6;
      continue;
    }

    // Now extract the actual text value
    int colonIndex = json.indexOf(":", keyIndex);
    if (colonIndex == -1) break;

    int startQuote = json.indexOf("\"", colonIndex);
    if (startQuote == -1) break;

    int endQuote = json.indexOf("\"", startQuote + 1);
    if (endQuote == -1) break;

    String chunk = json.substring(startQuote + 1, endQuote);
    chunk.replace("\\n", "\n");
    chunk.replace("\\\"", "\"");
    chunk.replace("\\\\", "\\");

    result += chunk + "\n";

    searchPos = endQuote + 1;
  }

  result.trim();
  return result;
}

// ==== WAV Header Helper ====
void writeWavHeader(uint8_t* header, int dataSize) {
  //Serial.println("inside writewaveHeader");
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
String captureAndSend() {

  Serial.printf("Pre-roll: %s, EOS detection: %s\n",
                usePreRoll ? "ON" : "OFF",
                useEndOfSpeech ? "ON" : "OFF");
  const int totalSamples = SAMPLE_RATE * RECORD_SECONDS;  // e.g. 16000 * 5 = 80000
  const int dataSize = totalSamples * 2;                  // 16-bit samples
  const int wavSize = dataSize + 44;

  // Allocate WAV buffer in PSRAM
  uint8_t* wavBuffer = (uint8_t*)ps_malloc(wavSize);
  if (!wavBuffer) {
    Serial.printf("ps_malloc FAILED for %d bytes\n", wavSize);
    return String("ERROR");
  }

  if (DEBUG) Serial.printf("Allocated %d bytes in PSRAM\n", wavSize);

  // Write WAV header
  writeWavHeader(wavBuffer, dataSize);

  // Pointer to PCM region
  int16_t* samplePtr = (int16_t*)(wavBuffer + 44);

  // ====================================================================================
  // STEP 1: Warm up I2S – fill pre-roll with REAL samples before main capture
  // ====================================================================================
  {
    int16_t warmBuf[BUFFER_LEN];
    size_t bytesReadNow = 0;
    int collected = 0;

    while (collected < PREROLL_SAMPLES) {

      // Read a full DMA-sized block
      i2s_channel_read(
        rx_chan,
        warmBuf,
        BUFFER_LEN * sizeof(int16_t),
        &bytesReadNow,
        portMAX_DELAY);

      int samplesRead = bytesReadNow / sizeof(int16_t);
      if (samplesRead <= 0) {
        // Defensive: avoid tight spin if something goes weird
        delay(1);
        continue;
      }

      // Feed pre-roll until we've collected PREROLL_SAMPLES
      for (int i = 0; i < samplesRead && collected < PREROLL_SAMPLES; i++) {
        if (usePreRoll) {
          updatePreRoll(warmBuf[i]);
        }
        collected++;
      }
    }
  }

  // Health check for Preroll
  
  if (usePreRoll && DEBUG) {
    // Compute RMS of the pre-roll buffer
    double sumSq = 0.0;
    for (int i = 0; i < PREROLL_SAMPLES; i++) {
      int16_t s = preRollBuffer[i];
      sumSq += (double)s * (double)s;
    }
    double rms = sqrt(sumSq / PREROLL_SAMPLES);

    Serial.printf("Pre-roll RMS = %.2f (%d samples)\n", rms, PREROLL_SAMPLES);
  }

  // ====================================================================================
  // STEP 2: Copy fresh pre-roll into WAV buffer
  // ====================================================================================
  int prerollToCopy = min(PREROLL_SAMPLES, totalSamples);
  int idx = preRollIndex;  // oldest sample in ring

  int baseIndex = 0;  // <-- declare first, default to 0
  int64_t speechSum = 0;
  long speechCount = 0;

  if (usePreRoll) {
    for (int n = 0; n < prerollToCopy; n++) {
      int16_t raw = preRollBuffer[idx];
      idx = (idx + 1) % PREROLL_SAMPLES;

      samplePtr[n] = raw << 5;

      // If you ever want pre-roll in RMS again, re-enable these:
      // speechCount++;
      // speechSum += (int64_t)raw * raw;
    }
    baseIndex = prerollToCopy;
  } else {
    baseIndex = 0;  // no pre-roll, start writing at samplePtr[0]
  }

  // IMPORTANT: captureSamples must use baseIndex, not prerollToCopy
  int captureSamples = totalSamples - baseIndex;
  if (DEBUG) Serial.printf("Capture window: baseIndex=%d, captureSamples=%d, total=%d\n",
                           baseIndex, captureSamples, totalSamples);

  // Silence threshold used for chunk-level and full-buffer detection
  const float SILENCE_THRESHOLD = 110.0f;
  // ====================================================================================
  // STEP 3: Capture audio + RMS + write AFTER pre-roll region
  // ====================================================================================
  int32_t i2sBlock[BUFFER_LEN];
  int16_t rawBlock[BUFFER_LEN];

  // End-of-speech detection
  int silenceChunks = 0;

  // Duration of one chunk in ms
  const int CHUNK_MS = (BUFFER_LEN * 1000) / SAMPLE_RATE;  // e.g. ~32 ms

  // How long silence must persist to stop early
  const int SILENCE_MS_REQUIRED = 450;  // tune 300–600 ms
  const int SILENCE_CHUNKS_REQUIRED = SILENCE_MS_REQUIRED / CHUNK_MS;

  for (int i = 0; i < captureSamples; i += BUFFER_LEN) {

    int samplesThisChunk = min(BUFFER_LEN, captureSamples - i);
    size_t bytesThisChunk = samplesThisChunk * sizeof(int32_t);

    uint8_t* dest = (uint8_t*)i2sBlock;
    size_t remaining = bytesThisChunk;

    while (remaining > 0) {
      size_t bytesReadNow = 0;

      esp_err_t err = i2s_channel_read(
        rx_chan,
        dest,
        remaining,
        &bytesReadNow,
        portMAX_DELAY);

      if (err != ESP_OK) {
        Serial.printf("I2S read error: %d\n", err);
        break;
      }

      if (bytesReadNow == 0) {
        Serial.println("0-byte read, skipping");
        break;
      }

      int samples = bytesReadNow / 4;
      int32_t* s32 = (int32_t*)dest;

      // Convert 32-bit I2S → 16-bit
      for (int j = 0; j < samples; j++) {
        int32_t sample32 = s32[j];
        rawBlock[j] = sample32 >> 14;
      }

      // -----------------------------
      // Compute chunk RMS (for end-of-speech detection)
      // -----------------------------
      int64_t chunkSum = 0;
      for (int j = 0; j < samples; j++) {
        chunkSum += (int64_t)rawBlock[j] * (int64_t)rawBlock[j];
      }
      float chunkRMS = sqrt((float)chunkSum / samples);

      // -----------------------------
      // End-of-speech detection
      // -----------------------------
      if (useEndOfSpeech) {
        if (chunkRMS < SILENCE_THRESHOLD) {
          silenceChunks++;
        } else {
          silenceChunks = max(0, silenceChunks - 1);
          // reset when speech resumes
        }
      }

      if (useEndOfSpeech && silenceChunks >= SILENCE_CHUNKS_REQUIRED) {
        Serial.println("Detected end of speech — stopping early.");
        goto END_CAPTURE;
      }

      // RMS + pre-roll update + store
      for (int j = 0; j < samples; j++) {
        int16_t raw = rawBlock[j];

        if (usePreRoll) {
          updatePreRoll(raw);
        }

        speechSum += (int64_t)raw * (int64_t)raw;
        speechCount++;

        int writeIndex = baseIndex + i + j;
        samplePtr[writeIndex] = raw << 5;
      }

      dest += bytesReadNow;
      remaining -= bytesReadNow;
    }
  }
END_CAPTURE:
  // Zero-pad the remainder of the buffer after early stop
  for (int k = speechCount; k < totalSamples; k++) {
    samplePtr[k] = 0;
  }

  // ====================================================================================
  // STEP 4: Compute RMS
  // ====================================================================================
  float speechRMS = 0.0f;

  if (speechCount == 0) {
    Serial.println("No valid samples — forcing speechRMS=0");
  } else {
    speechRMS = sqrt((float)speechSum / speechCount);
  }

  Serial.printf("Speech RMS = %.2f (%ld samples)\n", speechRMS, speechCount);

  // ====================================================================================
  // STEP 5: Silence detection
  // ====================================================================================

  if (speechRMS < SILENCE_THRESHOLD) {
    Serial.println("Silence detected — skipping Whisper.");
    free(wavBuffer);
    return String("__SILENCE__");
  }

  // ====================================================================================
  // STEP 6: EQ, AGC, trimming
  // ====================================================================================
  //applyEQ(samplePtr, totalSamples);
  //applyAGC(samplePtr, totalSamples);

  applyEQ(samplePtr + baseIndex, captureSamples);
  applyAGC(samplePtr + baseIndex, captureSamples);

  float trimThreshold = computeAdaptiveThreshold(samplePtr, totalSamples);
  int trimmedSamples = trimSilence(samplePtr, totalSamples);
  (void)trimThreshold;
  (void)trimmedSamples;

  // ====================================================================================
  // STEP 7: Enqueue WAV for async upload
  // ====================================================================================
  enqueueWavForUpload(wavBuffer, wavSize);
  if (DEBUG) Serial.println("📥 Enqueued WAV for async upload");

  return String("__UPLOAD_QUEUED__");
}

/*----------------------------- Text Wrapper -------------------------------------------*/

//  functions moved to support_functions.h

void drawWrappedTextCentered(
  const String& text,
  int x, int y,
  int maxWidth,
  int lineHeight) {
  if (USE_CIRCULAR_TEXT) {
    // x and maxWidth are ignored in circular mode
    drawWrappedTextCircle(
      text,
      DISPLAY_CX,
      DISPLAY_CY,
      DISPLAY_RADIUS,
      y,
      lineHeight);
  } else {
    // original rectangular behavior
    drawWrappedTextRect(text, x, y, maxWidth, lineHeight);
  }
}

/*---------------------------- Speaker Setup  ------------------------------------------*/

void setupI2SSpeaker() {

  if (DEBUG) Serial.println("Init I2S speaker...");

  i2s_chan_config_t chan_cfg = {
    .id = I2S_NUM_1,  // mic should be on NUM_0
    .role = I2S_ROLE_MASTER,
    .dma_desc_num = 8,
    .dma_frame_num = 256,
    .auto_clear = true
  };

  esp_err_t err = i2s_new_channel(&chan_cfg, &tx_chan, NULL);
  if (DEBUG) Serial.printf("i2s_new_channel (TX) -> %d, handle=%p\n", err, tx_chan);
  if (err != ESP_OK) {
    Serial.printf("❌ i2s_new_channel failed: %d\n", err);
    tx_chan = NULL;
    return;
  }

  i2s_std_config_t std_cfg = {
    .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(24000),
    .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(
      I2S_DATA_BIT_WIDTH_16BIT,
      I2S_SLOT_MODE_MONO),
    .gpio_cfg = {
      .mclk = I2S_GPIO_UNUSED,
      .bclk = (gpio_num_t)20,  // BCLK
      .ws = (gpio_num_t)6,     // LRCLK
      .dout = (gpio_num_t)21,  // DIN
      .din = I2S_GPIO_UNUSED }
  };

  std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;

  esp_err_t err2 = i2s_channel_init_std_mode(tx_chan, &std_cfg);
  if (DEBUG) Serial.printf("init_std_mode (TX) -> %d\n", err2);
  if (err2 != ESP_OK) {
    Serial.printf("❌ init_std_mode failed: %d\n", err2);
    tx_chan = NULL;
    return;
  }

  esp_err_t err3 = i2s_channel_enable(tx_chan);
  Serial.printf("channel_enable (TX) -> %d\n", err3);
  if (err3 != ESP_OK) {
    Serial.printf("❌ channel_enable failed: %d\n", err3);
    tx_chan = NULL;
    return;
  }

  if (DEBUG) Serial.println("✅ I2S speaker init OK");
}

/*---------------------------- Speech Engine  ------------------------------------------*/
void speechEngine(const char* phrase, const char* voice, float speed, const char* instructions) {

  // ---------------------------------------------------------------------------
  // BUSY CHECK — queue if audio is already playing
  // ---------------------------------------------------------------------------
  if (isPlaying) {
    if (DEBUG) Serial.println("🔁 Playback busy — queuing next phrase");
    queuedPhrase = phrase;
    queuedVoice = voice;
    queuedSpeed = speed;
    queuedInstructions = instructions;
    hasQueued = true;
    return;
  }
  isPlaying = true;

  if (DEBUG) Serial.println("📡 Sending POST to OpenAI TTS (PCM streaming)...");

  WiFiClientSecure client;
  client.setInsecure();

  if (!client.connect("api.openai.com", 443)) {
    Serial.println("❌ TLS connection failed");
    errorHandler(ERR_UNREACHABLE_SERVICE);
    isPlaying = false;
    return;
  }

  // ---------------------------------------------------------------------------
  // BUILD JSON PAYLOAD
  // ---------------------------------------------------------------------------
  String payload = "{"
                   "\"model\": \"gpt-4o-mini-tts\","
                   "\"input\": \""
                   + String(phrase) + "\","
                                      "\"voice\": \""
                   + String(voice) + "\","
                                     "\"speed\": "
                   + String(speed, 2) + ","
                                        "\"instructions\": \""
                   + String(instructions) + "\","
                                            "\"response_format\": \"pcm\""
                                            "}";

  // ---------------------------------------------------------------------------
  // SEND HTTP REQUEST
  // ---------------------------------------------------------------------------
  String request =
    "POST /v1/audio/speech HTTP/1.1\r\n"
    "Host: api.openai.com\r\n"
    "Authorization: Bearer "
    + String(chatGPT_APIKey_txt) + "\r\n"
                                   "Content-Type: application/json\r\n"
                                   "Content-Length: "
    + String(payload.length()) + "\r\n"
                                 "Connection: close\r\n\r\n";

  client.print(request);
  client.print(payload);

  if (DEBUG) Serial.println("⏳ Waiting for response...");

  // ---------------------------------------------------------------------------
  // WAIT FOR RESPONSE (10s timeout)
  // ---------------------------------------------------------------------------
  unsigned long startWait = millis();
  while (client.connected() && !client.available()) {
    if (millis() - startWait > 10000) {
      Serial.println("❌ Timeout waiting for response");
      client.stop();
      errorHandler(ERR_UNREACHABLE_SERVICE);
      isPlaying = false;
      return;
    }
    delay(1);
  }

  // ---------------------------------------------------------------------------
  // SKIP HTTP HEADERS
  // ---------------------------------------------------------------------------
  while (client.available()) {
    String line = client.readStringUntil('\n');
    if (line == "\r" || line.length() == 0) break;  // end of headers
  }

  if (DEBUG) Serial.println("📥 Downloading full PCM response into buffer...");

  // ---------------------------------------------------------------------------
  // PHASE 1: DOWNLOAD + DECODE ALL PCM INTO RAM
  // ---------------------------------------------------------------------------
  uint8_t buf[1024];

  std::vector<int16_t> pcm;
  pcm.reserve(32768);

  uint8_t leftover = 0;
  bool hasLeftover = false;
  size_t totalBytes = 0;

  while (client.connected()) {

    // 1) Read chunk size line (hex) — tolerant version
    String sizeLine = client.readStringUntil('\n');
    sizeLine.trim();

    // Skip empty lines or garbage (Cloudflare sometimes injects these)
    if (sizeLine.length() == 0) {
      delay(1);
      continue;
    }

    // Validate hex
    bool isHex = true;
    for (int i = 0; i < sizeLine.length(); i++) {
      if (!isxdigit(sizeLine[i])) {
        isHex = false;
        break;
      }
    }

    // If not hex, skip it and keep reading
    if (!isHex) {
      if (DEBUG) {
        // Serial.printf("⚠️ Skipping non-hex chunk header: '%s'\n", sizeLine.c_str());
      }
      continue;
    }

    long chunkSize = strtol(sizeLine.c_str(), nullptr, 16);

    // End of stream
    if (chunkSize <= 0) {
      if (DEBUG) Serial.println("🔚 End of chunks");
      break;
    }

    long remaining = chunkSize;

    // 2) Read exactly chunkSize bytes and decode to int16_t
    while (remaining > 0 && client.connected()) {
      int toRead = remaining > (long)sizeof(buf) ? sizeof(buf) : (int)remaining;
      int len = client.read(buf, toRead);
      if (len <= 0) {
        delay(1);
        continue;
      }

      remaining -= len;
      totalBytes += len;

      int i = 0;

      // Handle leftover from previous chunk safely
      if (hasLeftover) {
        uint8_t high = buf[0];

        if (high == '\r' || high == '\n') {
          // Boundary noise — discard leftover, skip this byte
          i = 1;
          hasLeftover = false;
        } else {
          int16_t sample = (int16_t)((high << 8) | leftover);
          if (gain != 1.0f) {
            sample = (int16_t)(sample * gain);
          }
          pcm.push_back(sample);

          i = 1;
          hasLeftover = false;
        }
      }

      // Process full pairs
      for (; i + 1 < len; i += 2) {
        int16_t sample = (int16_t)((buf[i + 1] << 8) | buf[i]);
        if (gain != 1.0f) {
          sample = (int16_t)(sample * gain);
        }
        pcm.push_back(sample);
      }

      // If we ended on an odd byte, save it for the next iteration
      if ((len - i) == 1) {
        leftover = buf[i];
        hasLeftover = true;
      } else {
        hasLeftover = false;
      }
    }

    // 3) Consume CRLF after each chunk
    if (client.available()) {
      char c1 = client.read();
      if (c1 == '\r' && client.available()) {
        client.read();  // consume '\n'
      }
    }
  }

  client.stop();

  Serial.printf("✅ Download complete: %u bytes raw, %u samples decoded\n",
                (unsigned)totalBytes, (unsigned)pcm.size());

  if (pcm.empty()) {
    Serial.println("❌ No PCM data received");
    errorHandler(ERR_UNREACHABLE_SERVICE);
    isPlaying = false;
    return;
  }

  // ---------------------------------------------------------------------------
  // PHASE 2: PLAY BACK FROM RAM TO I2S (NO NETWORK DEPENDENCY)
  // ---------------------------------------------------------------------------
  Serial.println("🔊 Playing buffered PCM to I2S...");

  // Optional: DMA FLUSH BEFORE PLAYBACK to avoid pops
  int16_t silence = 0;
  size_t written;
  for (int i = 0; i < 400; i++) {  // ~25 ms at 16 kHz
    i2s_channel_write(tx_chan, &silence, sizeof(silence), &written, portMAX_DELAY);
  }

  // Stream all buffered samples to I2S at full speed
  for (size_t n = 0; n < pcm.size(); ++n) {
    int16_t sample = pcm[n];
    i2s_channel_write(tx_chan, &sample, sizeof(sample), &written, portMAX_DELAY);
  }

  // Optional: small trailing silence to clear DMA
  int16_t silence2 = 0;
  size_t written2;
  for (int i = 0; i < 200; i++) {
    i2s_channel_write(tx_chan, &silence2, sizeof(silence2), &written2, portMAX_DELAY);
  }

  if (DEBUG) Serial.println("🎧 Playback finished from buffer");
  isPlaying = false;

  // ---------------------------------------------------------------------------
  // QUEUED PHRASE FLUSH
  // ---------------------------------------------------------------------------
  if (hasQueued) {
    if (DEBUG) Serial.println("▶️ Playing queued phrase");
    hasQueued = false;
    speechEngine(queuedPhrase.c_str(), queuedVoice.c_str(), queuedSpeed, queuedInstructions.c_str());
  }
}


/*---------------------------- File information  ------------------------------------------*/
void fileInfo() {  // uesful to figure our what software is running

  tft.fillScreen(TFT_BLUE);
  tft.setTextColor(TFT_WHITE);  // Print to TFT display, White color
  tft.setTextSize(1);
  tft.drawString("           Speech to Text", 8, 50);
  tft.drawString("         Google Translate", 10, 60);
  tft.setTextSize(1);
  tft.drawString(__FILENAME__, 30, 90);
  tft.drawString(__DATE__, 35, 125);
  tft.drawString(__TIME__, 125, 125);
  tft.drawString("___________________________________", 10, 145);
  tft.setTextSize(2);
  tft.drawString("Press Screen", 50, 165);
  tft.drawString(" to Start!", 50, 185);
  //delay(3000);
}

// ---------------------------------------------------------
//  JSON ESCAPER — makes prompts safe for Gemini
// ---------------------------------------------------------
String jsonEscape(const String& s) {
  String out;
  out.reserve(s.length() * 1.2);

  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    switch (c) {
      case '\"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if ((uint8_t)c < 0x20) {
          char buf[7];
          snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out += c;
        }
    }
  }
  return out;
}

/*---------------------------- Ask Gemini  ------------------------------------------*/
// ---------------------------------------------------------
//  HARDENED GEMINI CALL
// ---------------------------------------------------------
String askGemini(const String& userText,
                 const String& instruction,
                 float temperature) {

  HTTPClient http;

  String url =
    "https://generativelanguage.googleapis.com/v1/models/gemini-2.0-flash:generateContent?key="
    + String(Gemini_APIKey);

  // Escape user text
  String safeUser = jsonEscape(userText);

  // Build full prompt (instruction + user input + Model cue)
  String fullPrompt =
    instruction + "User: " + safeUser + "\n"
                                        "Model:";

  // Escape for JSON
  String safePrompt = jsonEscape(fullPrompt);

  // Build JSON payload
  String payload =
    "{"
    "\"contents\":[{"
    "\"role\":\"user\","
    "\"parts\":[{\"text\":\""
    + safePrompt + "\"}]"
                   "}],"
                   "\"generationConfig\":{"
                   "\"temperature\":"
    + String(temperature, 3) + "}"
                               "}";

  http.begin(url);
  http.addHeader("Content-Type", "application/json");

  int httpResponseCode = http.POST(payload);
  String response = "Error";

  if (httpResponseCode == 200) {
    String raw = http.getString();
    String extracted = extractAllTextFields(raw);

    if (extracted.length() > 0) {
      response = extracted;
    } else {
      Serial.println("❌ Parse Error: No text fields found.");
      Serial.println(raw);
      response = "Parse Error: No text fields found.";
      errorHandler(ERR_UNKNOWN);
    }

  } else {
    String errorBody = http.getString();
    Serial.println("❌ Gemini Error " + String(httpResponseCode));
    Serial.println("Server Response: " + errorBody);
    response = "HTTP Error: " + String(httpResponseCode);
  }

  http.end();

  if (httpResponseCode < 200 || httpResponseCode >= 300) {
    errorHandler(ERR_UNKNOWN);
  }

  return response;
}

/*------------------------------- Sanitize ------------------------------------------------*/

char* sanitizeQuip(const char* input) {
  String safeQuip = String(input);
  safeQuip.replace("\\", "\\\\");
  safeQuip.replace("\"", "\\\"");
  safeQuip.replace("\n", " ");

  int len = safeQuip.length() + 1;
  char* result = (char*)malloc(len);
  if (result) {
    safeQuip.toCharArray(result, len);
  }
  return result;
}
