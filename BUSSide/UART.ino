#include <pins_arduino.h>
#include "BUSSide.h"

// Declare globally so it doesn't live on the stack
static SoftwareSerial* globalSer = nullptr;

//#define min(a,b) (((a)<(b))?(a):(b))

uint32_t  usTicks = 0;

static int gpioVal[N_GPIO];

// Direct Register Read for ESP8266 should increase speed
// #define sampleTx(pin) ((GPI & (1 << pin)) != 0)
#define sampleTx(pin) ( (pin == 16) ? (GP16I & 0x01) : (GPI & (1 << pin)) )

struct uartInfo_s {
  int baudRate;
  float microsDelay;
} uartInfo[] = {
  { 300,    3333.3  }, // 0
  { 600,    1666.7  }, // 1
  { 1200,   833.3 }, // 2
  { 2400,   416.7 }, // 3
  { 4800,   208.3 }, // 4
  { 9600,   104.2 }, // 5
  { 19200,  52.1  }, // 6
  { 38400,  26.0  }, // 7
  { 57600,  17.4  }, // 8
  { 115200, 8.68  }, // 9
  { 0, 0 },
};

static int uartSpeedIndex;

static unsigned int findNumberOfUartSpeeds(void)
{
  unsigned int i;

  for (i = 0; uartInfo[i].baudRate; i++);
  return i;
}

// in RAM for speed (be careful there's not much of it)
// static int IRAM_ATTR waitForIdle(int pin)
// {
//   unsigned long startTime;
//   unsigned long bitTime10;
//   int32_t beginTime;

//   beginTime = asm_ccount();
//   bitTime10 = uartInfo[uartSpeedIndex].microsDelay * 10.0;
// start:
//   system_soft_wdt_feed();
//   if ((uint32_t)(asm_ccount() - beginTime)/FREQ >= 10*1000000)
//     return 1;
//   startTime = microsTime();
//   while ((microsTime() - startTime) <  bitTime10) {
//     if (sampleTx(pin) != HIGH)
//       goto start;
//     ;
//   }
//   return 0;
// }

static int IRAM_ATTR waitForIdle(int pin)
{
  // Safety: GPIO16 (D0) uses a different register on ESP8266
  bool isGPIO16 = (pin == 16);
  uint32_t pinMask = (1 << pin);
  
  // 10 bits of IDLE time required to confirm we are between characters
  // FREQ is usually 80 (for 80MHz)
  uint32_t bitTime10Cycles = (uint32_t)(uartInfo[uartSpeedIndex].microsDelay * 10.0f * FREQ);
  
  // Global timeout: 2 seconds is plenty for discovery (reduced from 10)
  uint32_t timeoutCycles = 2 * 1000000 * FREQ; 
  uint32_t startWait = asm_ccount();

start:
  uint32_t checkStart = asm_ccount();
  
  while ((asm_ccount() - checkStart) < bitTime10Cycles) {
    // Check if line is LOW (active)
    bool isLow = isGPIO16 ? !(GP16I & 0x01) : !(GPI & pinMask);
    
    if (isLow) {
      // If line is active, check for global timeout then reset idle timer
      if ((asm_ccount() - startWait) > timeoutCycles) return 1;
      
      // Feed watchdog only when we see activity to keep loop tight
      system_soft_wdt_feed(); 
      goto start;
    }
  }
  
  return 0; // Line is officially IDLE
}

static int IRAM_ATTR buildwidths(int pin, int *widths, int nwidths)
{
  int val;
  int32_t startTime;
  int curVal;

  // Reduced requirement: try to wait for idle, but continue even if not perfectly idle
  waitForIdle(pin); 
  
  // Capture initial state
  widths[0] = sampleTx(pin); 
  val = widths[0];
  startTime = asm_ccount();
  
  for (int i = 1; i < nwidths; i++) {
    uint32_t startCheck = asm_ccount();
    // Watchdog and state change check
    do {
      curVal = sampleTx(pin);
      // Timeout after 1 second of no transitions on a single bit
      if ((uint32_t)(asm_ccount() - startCheck) / FREQ > 1000000) return 1; 
    } while (curVal == val);
    
    uint32_t now = asm_ccount();
    widths[i] = (uint32_t)(now - startTime) / FREQ;
    val = curVal;
    startTime = now;
    system_soft_wdt_feed();
  }
  return 0;
}

// in RAM for speed (be careful there's not much of it)
static unsigned int IRAM_ATTR findminwidth(int *widths, int nwidths)
{
  int minIndex1;
  unsigned int min1;

  minIndex1 = -1;
  for (int i = 2; i < nwidths; i++) {
    if (minIndex1 < 0 || widths[i] < min1) {
      min1 = widths[i];
      minIndex1 = i;
    }
  }
  return min1;
}

// Returns bit width scaled by 100
static uint32_t IRAM_ATTR autobaud_int(int *widths, int nwidths)
{
  uint32_t sumX100 = 0;
  int count = 0;

  // We look at chunks of pulses to find the smallest consistent width
  for (int i = 2; (i + 15) < nwidths; i += 15) {
    uint32_t minw = findminwidth(&widths[i], 15);
    sumX100 += (minw * 100);
    count++;
  }
  
  if (count == 0) return 0;
  return sumX100 / count;
}

static int IRAM_ATTR calcBaud(int pin, int *widths, int nwidths)
{
  uint32_t measuredWidthX100 = autobaud_int(widths, nwidths);
  if (measuredWidthX100 == 0) return -1;

  int bestIndex = -1;
  uint32_t minDiff = 0xFFFFFFFF;

  for (int i = 0; uartInfo[i].baudRate; i++) {
    uint32_t targetWidthX100 = (uint32_t)(uartInfo[i].microsDelay * 100.0f);
    
    // Calculate Absolute Difference
    uint32_t diff = (measuredWidthX100 > targetWidthX100) ? 
                    (measuredWidthX100 - targetWidthX100) : 
                    (targetWidthX100 - measuredWidthX100);

    if (diff < minDiff) {
      minDiff = diff;
      bestIndex = i;
    }
  }
  return bestIndex;
}

// static float autobaud(int pin, int *widths, int nwidths)
// {
//   int sum;
//   int c = 0;

//   sum = 0;
//   for (int i = 2; (i+15) < nwidths; i += 15, c++) {
//     int minwidth;
    
//     minwidth = findminwidth(&widths[i], 15);
//     sum += minwidth;
//   }
//   return (float)sum/(float)c;
// }

// in RAM for speed (be careful there's not much of it)
// Original with Floating Point Math
// static int IRAM_ATTR tryFrameSize(int framesize, int stopbits, int *widths, int nwidths)
// {
//   float width_timepos = 0.05;
//   float bitTime = uartInfo[uartSpeedIndex].microsDelay;
//   float stopTime = bitTime*((float)framesize - (float)stopbits + 0.5);
//   float frameTime = bitTime*(float)framesize;
//   float w;
//   int framingErrors = 0;

//   w = 0.0;
//   for (int i = 2; i < nwidths-1; i++) {
//     if (stopTime >= w && stopTime < (w + widths[i])) {
//       if ((i % 2) != widths[0]) {
//         framingErrors++;
//         if (framingErrors >= 1)
//           return 0;
//       }
//       w = 0.0;
//     } else {
//       w += widths[i];
//     }
//   }
//   return 1;
// }

// in RAM for speed (be careful there's not much of it)
// further optimized by not doing any floating point math, should execute in roughly 1/10th the clock cycles of the original.
static int IRAM_ATTR tryFrameSize(int framesize, int stopbits, int *widths, int nwidths)
{
  uint32_t bitTimeX100 = (uint32_t)(uartInfo[uartSpeedIndex].microsDelay * 100.0f);
  // Measure from the center of the expected Stop Bit
  uint32_t stopTimeX100 = (bitTimeX100 * (framesize - stopbits)) + (bitTimeX100 / 2);
  
  uint32_t wX100 = 0;
  int framingErrors = 0;
  int validFrames = 0;

  // IMPORTANT: widths[0] is the state of the line when buildwidths started.
  // UART Start bits are ALWAYS 0 (LOW). 
  // If widths[0] was LOW, then even indices (2, 4...) are LOW.
  // If widths[0] was HIGH, then even indices (2, 4...) are HIGH.
  int startBitLevel = 0; // Standard UART logic

  for (int i = 2; i < nwidths - 1; i++) {
    uint32_t currentWidthX100 = (uint32_t)widths[i] * 100;
    uint32_t nextWX100 = wX100 + currentWidthX100;

    if (stopTimeX100 >= wX100 && stopTimeX100 < nextWX100) {
      // Logic: The pulse covering the STOP bit position MUST be HIGH.
      // In our toggle array, if widths[0] was HIGH, even indices are LOW. 
      // If widths[0] was LOW, even indices are HIGH.
      bool isHigh = (widths[0] == 0) ? (i % 2 == 0) : (i % 2 != 0);
      
      if (!isHigh) {
        framingErrors++;
        if (framingErrors > (validFrames / 10)) return 0; // Allow 10% jitter/error
      } else {
        validFrames++;
      }
      wX100 = 0; 
    } else {
      wX100 = nextWX100;
    }
  }
  return (validFrames > 2); // Must see at least a few valid frames
}

// static int calcParity(int frameSize, int stopBits, int *widths, int nwidths)
// {
//   float width_timepos = 0.0;
//   float bitTime = uartInfo[uartSpeedIndex].microsDelay;
//   float stopTime = bitTime*((float)frameSize - (float)stopBits + 0.5);
//   float frameTime = bitTime*(float)frameSize;
//   float w;
//   int bits[20];
//   float dataTime = (float)bitTime * 1.5;
//   int onBits = 0;
//   int odd, even;
//   int bitCount;
//   int framingErrors;

//   framingErrors = 0;
//   bitCount = 0;
//   odd = 0;
//   even = 0;
//   w = 0.0;
//   for (int i = 2; i < nwidths-1; i++) {
//     if (dataTime >= w && dataTime < (w + widths[i])) {
//       bits[bitCount] = i % 2;
//       if (i % 2)
//         onBits++;
//       if (bitCount < (frameSize - stopBits)) {
//          dataTime += dataTime;
//       }
//     }
//     if (stopTime >= w && stopTime < (w + widths[i])) {
//       if ((i % 2) != widths[0]) {
// //        Serial.printf("stop error\n");
//         framingErrors++;
//         if (framingErrors >= 1)
//           return 0;
//       }
//       if (onBits & 1)
//         odd++;
//       else
//         even++;
//       w = 0.0;
//       bitCount = 0;
//       dataTime = (float)bitTime * 1.5;
//       onBits = 0;
//     } else {
//       w += widths[i];
//     }
//   }
// //  Serial.printf("odd %i even %i\n", odd, even);
//   if (odd && even && min(odd,even) >= 1)
//     return -1;
//   if (odd)
//     return 1;
//   return 0;
// }


static int IRAM_ATTR calcParity(int frameSize, int stopBits, int *widths, int nwidths)
{
  uint32_t bitTimeX100 = (uint32_t)(uartInfo[uartSpeedIndex].microsDelay * 100.0f);
  
  // Theoretical timing for the center of the first data bit (1.5 bits in)
  uint32_t dataTimeX100 = (bitTimeX100 * 3) / 2;
  // Theoretical timing for the stop bit check
  uint32_t stopTimeX100 = (bitTimeX100 * (frameSize - stopBits)) + (bitTimeX100 / 2);
  
  uint32_t wX100 = 0;
  int onBits = 0;
  int odd = 0, even = 0;
  int bitCount = 0;
  int framingErrors = 0;

  for (int i = 2; i < nwidths - 1; i++) {
    uint32_t pWidthX100 = (uint32_t)widths[i] * 100;
    uint32_t nextWX100 = wX100 + pWidthX100;

    // Check Data Bits: If the "center" of a bit falls within this pulse
    while (dataTimeX100 >= wX100 && dataTimeX100 < nextWX100) {
      if ((i % 2) != widths[0]) { // bit is HIGH
        onBits++;
      }
      bitCount++;
      // Move to the center of the NEXT bit
      dataTimeX100 += bitTimeX100;
    }

    // Check Stop Bit: If this pulse aligns with the expected Stop Bit position
    if (stopTimeX100 >= wX100 && stopTimeX100 < nextWX100) {
      // Validate framing: Stop bit must be HIGH
      if ((i % 2) != widths[0]) {
        framingErrors++;
        if (framingErrors >= 1) return -2; // Frame Error
      }

      // Parity check: Was the total count of '1's (including parity bit) even or odd?
      if (onBits & 1) odd++;
      else even++;

      // Reset for next frame in the width buffer
      wX100 = 0;
      bitCount = 0;
      onBits = 0;
      dataTimeX100 = (bitTimeX100 * 3) / 2;
    } else {
      wX100 = nextWX100;
    }
  }

  if (odd && even) return -1; // Inconsistent (Mixed results suggest No Parity)
  if (odd) return 1;          // Consistently Odd
  return 0;                   // Consistently Even
}

static int frameSize;
static int stopBits;
static int dataBits;
static int parity;
static float bitTime;

#define nwidths 200 //was 200 for accuracy

static int UART_line_settings_direct(struct bs_reply_s *reply, int index, int *widths)
{
  int ret;
  int pin = gpioIndex[index];
  uint32_t *reply_data;

  pinMode(pin, (pin == D0) ? INPUT : INPUT_PULLUP);

  // Pass the externally allocated buffer
  ret = buildwidths(pin, widths, nwidths);
  if (ret) return -6; 

  uartSpeedIndex = calcBaud(pin, widths, nwidths);
  if (uartSpeedIndex < 0) return -1;
  
  bitTime = uartInfo[uartSpeedIndex].microsDelay;

  int detectedFrame = -1;
  int prioritySizes[] = {10, 11, 9, 12, 8, 7, 13};
  
  for (int i = 0; i < 7; i++) {
    if (tryFrameSize(prioritySizes[i], 1, widths, nwidths)) {
      detectedFrame = prioritySizes[i];
      break; 
    }  
    system_soft_wdt_feed(); // Keep alive during heavy math
  }
  
  if (detectedFrame < 0) return -1;
  frameSize = detectedFrame;

  stopBits = tryFrameSize(frameSize, 2, widths, nwidths) ? 2 : 1;
  parity = calcParity(frameSize, stopBits, widths, nwidths);

  // Logic Correction for DataBits
  if (parity < 0) { // None
    parity = -1;
    dataBits = frameSize - 1 - stopBits;
  } else { // Even or Odd
    dataBits = frameSize - 1 - stopBits - 1;
  }

  reply_data = (uint32_t *)&reply->bs_payload[0];
  reply_data[index*5 + 0] = gpio[index];
  reply_data[index*5 + 1] = dataBits;
  reply_data[index*5 + 2] = stopBits;
  reply_data[index*5 + 3] = parity;
  reply_data[index*5 + 4] = uartInfo[uartSpeedIndex].baudRate;
  
  return 0;
}

struct bs_frame_s*
UART_all_line_settings(struct bs_request_s *request)
{
  struct bs_frame_s *reply;
  int *widthsBuffer = (int *)malloc(nwidths * sizeof(int));
  if (widthsBuffer == NULL) return NULL;

  // Ensure we allocate enough space for the full payload
  size_t payloadSize = N_GPIO * 5 * sizeof(uint32_t);
  reply = (struct bs_frame_s *)malloc(BS_HEADER_SIZE + payloadSize);
  if (reply == NULL) {
    free(widthsBuffer);
    return NULL;
  }

  reply->bs_payload_length = payloadSize;
  memset(reply->bs_payload, 0, payloadSize);

  for (int i = 0; i < N_GPIO; i++) {
    // Only probe pins that data_discovery actually flagged as active
    if (gpio[i] > 2) { 
      // Fewer attempts, but more focused
      for (int attempt = 0; attempt < 5; attempt++) {
        system_soft_wdt_feed();
        if (UART_line_settings_direct(reply, i, widthsBuffer) == 0) {
          // Success! Move to next pin
          break; 
        }
        delay(10); // Wait for fresh data transitions
      }
    }
  }

  free(widthsBuffer);
  return reply;
}

// in RAM for speed (be careful there's not much of it)
struct  bs_frame_s* IRAM_ATTR data_discovery(struct bs_request_s *request)
{
  struct bs_frame_s *reply;
  uint32_t *reply_data;
  int32_t startTime;

  reply = (struct bs_frame_s *)malloc(BS_HEADER_SIZE + N_GPIO*4);
  if (reply == NULL)
    return NULL;
  reply_data = (uint32_t *)&reply->bs_payload[0];
  startTime = asm_ccount();
  for (int i = 0; i < N_GPIO; i++) {
    if (gpioIndex[i] == D0)
      pinMode(gpioIndex[i], INPUT);
    else
      pinMode(gpioIndex[i], INPUT_PULLUP);
    gpio[i] = 0;
    gpioVal[i] = sampleTx(gpioIndex[i]);
  }
  do {
    system_soft_wdt_feed();
    for (int i = 0; i < N_GPIO; i++) {
      int curVal;
      
      curVal = sampleTx(gpioIndex[i]);
      if (gpioVal[i] != curVal) {
        gpioVal[i] = curVal;
        gpio[i]++;
      }
    }
  } while ((uint32_t)(asm_ccount() - startTime)/FREQ < (3 * 1000000)); // Changed from 7.5 to 5 seconds
  reply->bs_payload_length = 4*N_GPIO;
  for (int i = 0; i < N_GPIO; i++) {
    reply_data[i] = gpio[i];
  }
  yield();
  return reply;
}

struct bs_frame_s*
UART_passthrough(struct bs_request_s *request)
{
  uint32_t *request_args = (uint32_t *)&request->bs_payload[0];
  int rx_pin = gpioIndex[request_args[0]];
  int tx_pin = gpioIndex[request_args[1]];
  uint32_t baud = request_args[2];

  // At 160MHz, calculate cycles per bit
  // 115200 baud -> ~1388 cycles per bit
  const uint32_t cycles_per_bit = 160000000UL / baud;
  const uint32_t half_bit = cycles_per_bit / 2;

  pinMode(rx_pin, INPUT);
  pinMode(tx_pin, OUTPUT);
  digitalWrite(tx_pin, HIGH);
  delay(20);
  // ACK Sequence (Crucial for the PC tool to sync)
  struct bs_frame_s *reply = (struct bs_frame_s *)malloc(BS_HEADER_SIZE);
  if (reply) {
    memcpy(reply, request, BS_HEADER_SIZE);
    reply->bs_command = BS_REPLY_UART_PASSTHROUGH;
    send_reply(request, reply);
    Serial.flush();
    free(reply);
  }

  while (1) {
    // MANUAL RX SAMPLING (DUT -> PC)
    // Poll the pin. If it's LOW, a start bit has begun.
    if (digitalRead(rx_pin) == LOW) {
      uint32_t start_cycles = asm_ccount();
      uint8_t data = 0;

      noInterrupts(); // Stop everything to ensure perfect timing
      
      // Wait for 1.5 bits to hit the middle of first data bit
      while ((asm_ccount() - start_cycles) < (cycles_per_bit + half_bit));

      for (int i = 0; i < 8; i++) {
        data >>= 1;
        if (digitalRead(rx_pin)) data |= 0x80;
        
        // Wait for the center of the next bit
        // Target = Start + (bit_index + 2) bits + half_bit
        uint32_t target = start_cycles + (cycles_per_bit * (i + 2)) + half_bit;
        while (asm_ccount() < target);
      }
      interrupts();

      // Direct write to Hardware FIFO
      while (((READ_PERI_REG(UART_STATUS(0)) >> UART_TXFIFO_CNT_S) & UART_TXFIFO_CNT) >= 120);
      WRITE_PERI_REG(UART_FIFO(0), data);
    }

    // PC -> DUT
    if (Serial.available() > 0) {
      uint8_t c = Serial.read();
      if (c == 24) break; // Ctrl+X
      
      // Manual TX Bit-bang (more stable than SoftwareSerial at 160MHz)
      noInterrupts();
      uint32_t bit_start = asm_ccount();
      digitalWrite(tx_pin, LOW); // Start
      for(int b=0; b<8; b++) {
        while(asm_ccount() - bit_start < cycles_per_bit * (b+1));
        digitalWrite(tx_pin, (c >> b) & 0x01);
      }
      while(asm_ccount() - bit_start < cycles_per_bit * 9);
      digitalWrite(tx_pin, HIGH); // Stop
      interrupts();
    }

    // 3. Keep the WDT happy
    system_soft_wdt_feed();
  }

  return NULL;
}

// Detecting BUSSide TX pin
struct  bs_frame_s*
UART_discover_tx(struct bs_request_s *request)
{
  uint32_t *request_args, *reply_data;
  struct bs_frame_s *reply;
  int rxpin, txpin;
  int baudrate;
  
  reply = (struct bs_frame_s *)malloc(BS_HEADER_SIZE + 4);
  if (reply == NULL) return NULL;
  
  reply->bs_payload_length = 4;
  reply_data = (uint32_t *)&reply->bs_payload[0];
  request_args = (uint32_t *)&request->bs_payload[0];
  
  rxpin = request_args[0] - 1; 
  baudrate = request_args[1];

  for (txpin = 1; txpin < N_GPIO; txpin++) {
    system_soft_wdt_feed();
    yield(); // Giving the dog a bone
    // Skip listening pin
    if (rxpin == txpin) continue;
    pinMode(gpioIndex[txpin], OUTPUT); // Explicitly claim the pin
    digitalWrite(gpioIndex[txpin], HIGH); // Idle state for UART is HIGH
    
    // Initialize: NodeMCU RX pin (Fixed), NodeMCU TX (Candidate being tested)
    SoftwareSerial ser(gpioIndex[rxpin], gpioIndex[txpin]);
    ser.begin(baudrate);
    
    // Flush any noise/garbage from the buffer before probing
    while (ser.available()) { ser.read(); }

    // Send Probe (Two Carriage Return/linefeeds to be sure)
    ser.print("BUSSide\r\n\r\n");
    
    // Listen Window: Wait up to 500ms for any sign of life
    unsigned long startWait = millis();
    bool activityDetected = false;
    int validChars = 0;

    while (millis() - startWait < 500) {
      // After sending 2 cr/lf, check for good ascii characters from DUT
      // If 2 or more received from DUT consider the pin data was sent on the BUSSide TX
      if (ser.available() > 0) {
        int c = ser.read();
        // Check if the character is printable ASCII or standard whitespace
        if ((c >= 32 && c <= 126) || c == '\r' || c == '\n') {
          validChars++;
        }
        if (validChars >= 2) { // Require two (or more) chararacters to confirm it's not noise
          activityDetected = true;
          break;
        }
      }
      system_soft_wdt_feed(); // Keep the watchdog happy during the wait
    }

    if (activityDetected) {
      reply_data[0] = txpin;
      ser.end();
      pinMode(gpioIndex[txpin], INPUT); // Force neutral state
      delay(1);
      return reply;
    }
    
    ser.end();
    delay(5); // Short delay before next attempt
  }

  reply_data[0] = -1; // No pin responded
  return reply;
  free(reply);
}
