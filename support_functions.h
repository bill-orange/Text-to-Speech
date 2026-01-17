#include <math.h>

#include "pngle.h"
// was 16 WEW 04/04/2025

#define LINE_BUF_SIZE 128  // pixel = 524, 16 = 406, 32 = 386, 64 = 375, 128 = 368, 240 = 367, no draw = 324 (51ms v 200ms)
int16_t px = 0, sx = 0; 
int16_t py = 0, sy = 0;
uint8_t pc = 0;
uint16_t lbuf[LINE_BUF_SIZE];

int16_t png_dx = 0, png_dy = 0;

// Vertical offset for first text line (portable tuning hook)
const int TEXT_Y_OFFSET = 0;   // set to 0 for this sketch; tweak later if needed

// Define corner position
void setPngPosition(int16_t x, int16_t y) {
  png_dx = x;
  png_dy = y;
}

// Draw pixel - called by pngle
void pngle_on_draw(pngle_t *pngle, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint8_t rgba[4]) {
  uint16_t color = (rgba[0] << 8 & 0xf800) | (rgba[1] << 3 & 0x07e0) | (rgba[2] >> 3 & 0x001f);

#if !defined(USE_ADAFRUIT_GFX) && defined(USE_LINE_BUFFER)
  color = (color << 8) | (color >> 8);
#endif

  if (rgba[3] > 127) {  // Transparency threshold (no blending yet...)

#ifdef USE_LINE_BUFFER  // This must handle skipped pixels in transparent PNGs
    if (pc >= LINE_BUF_SIZE) {
#ifdef USE_ADAFRUIT_GFX
      tft.drawRGBBitmap(png_dx + sx, png_dy + sy, lbuf, LINE_BUF_SIZE, 1);
#else
      tft.pushImage(png_dx + sx, png_dy + sy, LINE_BUF_SIZE, 1, lbuf);
#endif
      px = x;
      sx = x;
      sy = y;
      pc = 0;
    }

    if ((x == px) && (sy == y) && (pc < LINE_BUF_SIZE)) {
      px++;
      lbuf[pc++] = color;
    } else {
#ifdef USE_ADAFRUIT_GFX
      tft.drawRGBBitmap(png_dx + sx, png_dy + sy, lbuf, pc, 1);
#else
      tft.pushImage(png_dx + sx, png_dy + sy, pc, 1, lbuf);
#endif
      px = x;
      sx = x;
      sy = y;
      pc = 0;
      px++;
      lbuf[pc++] = color;
    }
#else
    tft.drawPixel(x, y, color);
#endif
  }
}


void load_png(const char *url) {
  HTTPClient http;

  http.begin(url);
 
  int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    Serial.printf("HTTP ERROR: %d\n", httpCode);
    http.end();
    return;
  }
  int total = http.getSize();

  WiFiClient *stream = http.getStreamPtr();
  stream->setTimeout(0);   //   this shall improve readByte() time.

  pngle_t *pngle = pngle_new();
  pngle_set_draw_callback(pngle, pngle_on_draw);

  uint8_t buf[1024];
  int remain = 0;
  int len;
  uint32_t timeout = 0;

#if !defined(USE_ADAFRUIT_GFX) && !defined(USE_LINE_BUFFER)
  tft.startWrite();  // Crashes Adafruit_GFX
#endif
  while (http.connected() && (total > 0 || total == -1)) {
    size_t size = stream->available();

    if (timeout > 40000) break;
    if (!size) {
      delay(2);
      timeout++;
      continue;
    }
    if (size > sizeof(buf) - remain) size = sizeof(buf) - remain;

    if ((len = stream->readBytes(buf + remain, size)) > 0) {
      int fed = pngle_feed(pngle, buf, remain + len);
      if (fed < 0) {
        Serial.printf("ERROR: %s\n", pngle_error(pngle));
        break;
      }
      remain = remain + len - fed;
      if (remain > 0) memmove(buf, buf + fed, remain);
      total -= len;
    }
  }
#ifdef USE_LINE_BUFFER
  // Draw any remaining pixels
  if (pc) {
#ifdef USE_ADAFRUIT_GFX
    tft.drawRGBBitmap(png_dx + sx, png_dy + sy, lbuf, pc, 1);
#else
    tft.pushImage(png_dx + sx, png_dy + sy, pc, 1, lbuf);
#endif
    pc = 0;
  }
#endif
#if !defined(USE_ADAFRUIT_GFX) && !defined(USE_LINE_BUFFER)
  tft.endWrite();
#endif
  pngle_destroy(pngle);
  http.end();
}

void drawWrappedTextCircle(
    const String& text,
    int cx, int cy,
    int radius,
    int startY,
    int lineHeight
) {
    // Apply the offset ONCE, right at the start
    int cursorY = startY + TEXT_Y_OFFSET;

    String remaining = text;
    //int cursorY = startY;

    while (remaining.length() > 0) {

        // --- Compute max width for this line based on circle geometry ---
        float dy = abs(cursorY - cy);
        if (dy >= radius) break;  // outside circle

        float maxWidth = 2.0f * sqrtf(radius * radius - dy * dy);

        String line = "";
        String word = "";
        int i = 0;
        int lastBreak = 0;  // index in 'remaining' after the last full word we kept

        while (i < remaining.length()) {
            char c = remaining[i];

            if (c == ' ' || c == '\n') {
                // Candidate line if we add this word
                int w = tft.textWidth(line + word + " ");

                if (w > maxWidth) {
                    // This word doesn't fit on this line; stop BEFORE consuming it
                    break;
                }

                // Word fits: commit it to the line
                line += word + " ";
                word = "";
                lastBreak = i + 1;  // next char after this space/newline

                if (c == '\n') {
                    // Explicit line break: consume newline and stop this line
                    i++;
                    break;
                }
            } else {
                word += c;
            }

            i++;
        }

        // Handle last word if we reached end of string without overflow
        if (i >= remaining.length() && word.length() > 0) {
            if (tft.textWidth(line + word) <= maxWidth) {
                line += word;
                lastBreak = remaining.length();
            } else if (line.length() == 0) {
                // Word longer than maxWidth: force it on this line anyway
                line += word;
                lastBreak = remaining.length();
            }
        }

        // If we never committed a word and line is still empty,
        // avoid infinite loop by forcing progress
        if (line.length() == 0 && word.length() > 0) {
            line = word;
            lastBreak = remaining.length();
        }

        // --- Draw the line centered relative to the circle ---
        int lineWidth = tft.textWidth(line);
        int drawX = cx - (lineWidth / 2);
        tft.drawString(line, drawX, cursorY);

        // Advance to next line
        cursorY += lineHeight;

        // Remove only the characters we actually consumed (up to lastBreak)
        if (lastBreak <= 0 || lastBreak > remaining.length()) {
            break;  // safety: avoid infinite loop
        }
        remaining = remaining.substring(lastBreak);
    }
}

void drawWrappedTextRect(
    const String& text,
    int x, int y,
    int maxWidth,
    int lineHeight
) {
    String currentLine = "";
    String currentWord = "";
    int cursorY = y;

    for (int i = 0; i < text.length(); i++) {
        char c = text[i];

        if (c == ' ' || c == '\n') {
            int lineWidth = tft.textWidth(currentLine + currentWord);

            if (lineWidth > maxWidth) {
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
