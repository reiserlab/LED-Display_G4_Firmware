// PanelTest - drives a Generation-4 16x16 LED display (four 8x8 matrices) over
// SPI through a continuous animation loop:
//
//   1. whole panel on (full brightness)
//   2. horizontal line sweeping top -> bottom
//   3. horizontal line sweeping bottom -> top
//   4. vertical line sweeping left -> right
//   5. vertical line sweeping right -> left
//   6. whole panel fading dark -> brightest
//
// then repeats. See the panel firmware (../../hardware_v0p2/driver_w_comp) for
// the receiving side; this sketch only generates and transmits frames.

#include <SPI.h>
#include <elapsedMillis.h>

// ---------------------------------------------------------------------------
// SPI chip-select pin
// The chip-select macro differs between Arduino cores:
//   Renesas UNO R4 (MINIMA/WIFI/NANO R4) defines PIN_SPI_CS
//   AVR (Uno/Mega/Leonardo) and mbed RP2040 (Pico) define PIN_SPI_SS
// Every core also defines plain SS as a last-resort fallback.
// ---------------------------------------------------------------------------
#ifndef PIN_SPI_CS
  #ifdef PIN_SPI_SS
    #define PIN_SPI_CS PIN_SPI_SS
  #else
    #define PIN_SPI_CS SS
  #endif
#endif

// G4 panels run SPI at 4 MHz, MSB first, mode 0.
SPISettings g4SPI(4000000, MSBFIRST, SPI_MODE0);

// ---------------------------------------------------------------------------
// Panel / packet geometry
//
// The display is a 2x2 tiling of four 8x8 matrices = 16x16 pixels. One SPI
// packet carries all four matrices back to back. Each matrix gets a 33-byte
// message: 1 header byte + 32 data bytes.
//
// 16-level grayscale packs two 4-bit pixels per data byte. The panel firmware
// lays the 32 data bytes out as 4 bytes per matrix row (8 rows), two columns
// per byte: low nibble = even column, high nibble = odd column.
//
// Header byte = grayscale-mode bit | (stretch << 1):
//   bit 0    : 0 = 2-level, 1 = 16-level grayscale
//   bits 1-7 : "stretch" (0-127), how long the one-shot scan dwells per row
// ---------------------------------------------------------------------------
#define MATRIX_SIZE      8                                   // 8x8 pixels per matrix
#define NUM_MATRICES     4
#define MATRIX_MSG_SIZE  33                                  // 1 header + 32 data bytes
#define PACKET_SIZE      (NUM_MATRICES * MATRIX_MSG_SIZE)    // 132 bytes
#define DISPLAY_SIZE     (2 * MATRIX_SIZE)                   // 16x16 pixels overall
#define MAX_LEVEL        15                                  // brightest 16-level gray value

#define GRAY_SCALE_16    1                                   // header bit 0
// "Stretch" (header bits 1-7, 0..127) lengthens how long each row dwells during
// the one-shot scan, so a higher value keeps the panel lit for more of the
// REFRESH_MS window -> higher duty cycle -> less visible flicker, WITHOUT
// sending frames faster (so no desync risk). Each unit adds ~16 us to the scan;
// keep (scan + 0.26 ms receive) safely under REFRESH_MS. Raise gradually until
// flicker is acceptable, then stop before glitches reappear.
#define STRETCH          10                                  // header bits 1-7 (0..127)

// ---------------------------------------------------------------------------
// Timing
// ---------------------------------------------------------------------------
// How long each animation step is shown.
#define STEP_MS    125
// How often the current frame is re-sent to the panel. The panel firmware shows
// each received frame as a SINGLE PWM scan and then blanks until the next frame,
// so we must keep resending to avoid flicker. There is a hard lower bound: a
// frame costs ~0.26 ms to clock in (132 B @ 4 MHz) plus ~0.77 ms to scan
// (~1.3 kHz, 16-level) ~= 1.03 ms. If we resend faster than that the panel is
// still scanning when the next packet starts and reads mid-stream -> erratic
// flicker/desync. So stay safely ABOVE ~1.03 ms; 2 ms leaves clear margin.
// To then close the remaining dark gap, raise STRETCH (below) rather than
// lowering this value.
#define REFRESH_MS 2

// ---------------------------------------------------------------------------
// Animation sequence layout
// Each phase is expressed as a cumulative "end" step so renderStep() can map a
// step index to the right phase. A directional sweep has one step per line
// (DISPLAY_SIZE lines); the fade has one step per gray level (MAX_LEVEL + 1).
// ---------------------------------------------------------------------------
#define STEP_ALLON   1
#define STEP_SWEEP   DISPLAY_SIZE       // lines in one directional sweep
#define STEP_FADE    (MAX_LEVEL + 1)    // gray levels in the fade

#define ALLON_END    (STEP_ALLON)
#define DOWN_END     (ALLON_END + STEP_SWEEP)   // horizontal line, top -> bottom
#define UP_END       (DOWN_END  + STEP_SWEEP)   // horizontal line, bottom -> top
#define RIGHT_END    (UP_END    + STEP_SWEEP)   // vertical line, left -> right
#define LEFT_END     (RIGHT_END + STEP_SWEEP)   // vertical line, right -> left
#define TOTAL_STEPS  (LEFT_END  + STEP_FADE)    // fade dark -> bright, then wrap

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
elapsedMillis stepTimer;            // time since the current step started
elapsedMillis refreshTimer;         // time since the frame was last sent
uint8_t frame[PACKET_SIZE];         // the packet currently being displayed
uint8_t step = 0;                   // current animation step (0 .. TOTAL_STEPS-1)

// ---------------------------------------------------------------------------
// Frame buffer helpers
// ---------------------------------------------------------------------------

// Set one pixel to a 16-level gray value (0..15).
//
// Coordinates: x = column 0..15 (left->right), y = row 0..15 (top->bottom).
// The four matrices tile the display 2x2; the buffer-block -> physical-position
// mapping was derived from the hardware-verified top->bottom row sweep:
//
//     block 0 = top-left      block 2 = top-right
//     block 1 = bottom-left   block 3 = bottom-right
//
// Top/bottom is hardware-verified; left/right is assumed and only mirrors x, so
// if a vertical sweep runs the "wrong" way just swap the left/right blocks.
void setPixel(uint8_t* buf, uint8_t x, uint8_t y, uint8_t level) {
  uint8_t top  = (y < MATRIX_SIZE);
  uint8_t left = (x < MATRIX_SIZE);
  uint8_t block = top ? (left ? 0 : 2) : (left ? 1 : 3);

  uint8_t mRow = y % MATRIX_SIZE;   // row within the matrix (0..7)
  uint8_t mCol = x % MATRIX_SIZE;   // column within the matrix (0..7)

  // 4 data bytes per matrix row, two columns per byte.
  uint8_t pos = block * MATRIX_MSG_SIZE   // start of this matrix's message
              + 1                          // skip its header byte
              + mRow * 4                    // 4 data bytes per row
              + (mCol / 2);                 // which of those 4 bytes

  uint8_t value = level & 0x0F;
  if (mCol & 1) {
    buf[pos] = (buf[pos] & 0x0F) | (value << 4);  // odd column  -> high nibble
  } else {
    buf[pos] = (buf[pos] & 0xF0) | value;         // even column -> low nibble
  }
}

// Write the per-matrix header byte (grayscale mode + stretch) into every block.
void writeHeaders(uint8_t* buf) {
  uint8_t header = GRAY_SCALE_16 | (STRETCH << 1);
  for (uint8_t m = 0; m < NUM_MATRICES; m++) {
    buf[m * MATRIX_MSG_SIZE] = header;
  }
}

// Fill the entire display with one gray level (0..15).
void drawSolid(uint8_t* buf, uint8_t level) {
  for (uint8_t y = 0; y < DISPLAY_SIZE; y++) {
    for (uint8_t x = 0; x < DISPLAY_SIZE; x++) {
      setPixel(buf, x, y, level);
    }
  }
}

// Draw a single fully-lit horizontal line at row y; all other pixels off.
void drawHLine(uint8_t* buf, uint8_t y) {
  drawSolid(buf, 0);
  for (uint8_t x = 0; x < DISPLAY_SIZE; x++) {
    setPixel(buf, x, y, MAX_LEVEL);
  }
}

// Draw a single fully-lit vertical line at column x; all other pixels off.
void drawVLine(uint8_t* buf, uint8_t x) {
  drawSolid(buf, 0);
  for (uint8_t y = 0; y < DISPLAY_SIZE; y++) {
    setPixel(buf, x, y, MAX_LEVEL);
  }
}

// Build the frame for animation step `s` (0 .. TOTAL_STEPS-1).
void renderStep(uint8_t* buf, uint8_t s) {
  if      (s < ALLON_END) drawSolid(buf, MAX_LEVEL);                          // all on
  else if (s < DOWN_END)  drawHLine(buf, s - ALLON_END);                      // top -> bottom
  else if (s < UP_END)    drawHLine(buf, (DISPLAY_SIZE - 1) - (s - DOWN_END));// bottom -> top
  else if (s < RIGHT_END) drawVLine(buf, s - UP_END);                         // left -> right
  else if (s < LEFT_END)  drawVLine(buf, (DISPLAY_SIZE - 1) - (s - RIGHT_END));// right -> left
  else                    drawSolid(buf, s - LEFT_END);                       // dark -> bright
  writeHeaders(buf);
}

// Transmit one full packet to the panel.
void sendFrame(const uint8_t* buf) {
  SPI.beginTransaction(g4SPI);
  digitalWrite(PIN_SPI_CS, LOW);
  for (uint8_t i = 0; i < PACKET_SIZE; i++) {
    SPI.transfer(buf[i]);
  }
  digitalWrite(PIN_SPI_CS, HIGH);
  SPI.endTransaction();
}

// ---------------------------------------------------------------------------
// Arduino entry points
// ---------------------------------------------------------------------------
void setup() {
  pinMode(PIN_SPI_CS, OUTPUT);
  digitalWrite(PIN_SPI_CS, HIGH);   // idle high
  Serial.begin(115200);
  SPI.begin();
  renderStep(frame, step);          // prime the first frame
  Serial.println("PanelTest started");
}

void loop() {
  // Advance the animation on a fixed time base, rebuilding the frame on change.
  if (stepTimer >= STEP_MS) {
    stepTimer = 0;
    step = (step + 1) % TOTAL_STEPS;
    renderStep(frame, step);
  }

  // Continuously resend the current frame so the panel never goes dark.
  if (refreshTimer >= REFRESH_MS) {
    refreshTimer = 0;
    sendFrame(frame);
  }
}
