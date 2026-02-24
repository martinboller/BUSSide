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

  if (waitForIdle(pin))
    return 1;
  widths[0] = 1;
  val = 1;
  startTime = asm_ccount();
  for (int i = 1; i < nwidths; i++) {
    int32_t newStartTime;

    do {
      system_soft_wdt_feed();
      curVal = sampleTx(pin);
    } while (curVal == val && (uint32_t)(asm_ccount() - startTime)/FREQ < 5*1000000);
    if (curVal == val)
      return 1;
      
    newStartTime = asm_ccount();
    val = curVal;
    widths[i] = (uint32_t)(newStartTime - startTime)/FREQ;
    startTime = newStartTime;
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

// in RAM for speed (be careful there's not much of it)
// further optimized by not doing any floating point math, should execute in roughly 1/10th the clock cycles of the original.
static int IRAM_ATTR tryFrameSize(int framesize, int stopbits, int *widths, int nwidths, float bTime) {
  int matches = 0;
  int total = 0;
  uint32_t bitTimeX100 = (uint32_t)(bTime * 100.0f);

  for (int i = 2; i < nwidths - framesize - 2; i++) {
    // Start bit must be an Even index (LOW state)
    if ((i % 2) != 0) continue; 

    bool stopValid = true;
    for (int s = 0; s < stopbits; s++) {
      int bitPos = framesize - 1 - s;
      uint32_t target = (bitTimeX100 * bitPos) + (bitTimeX100 / 2);
      uint32_t elapsed = 0;
      int pIdx = i;
      while (pIdx < nwidths && elapsed + (uint32_t)widths[pIdx]*100 <= target) {
        elapsed += (uint32_t)widths[pIdx] * 100;
        pIdx++;
      }
      
      // If the Stop bit position lands on an Even index, it's LOW (Error)
      if ((pIdx % 2) == 0) { 
        stopValid = false;
        break;
      }
    }

    if (stopValid) matches++;
    total++;
    if (total > 30) break;
  }
  
  if (total == 0) return 0;
  return ((float)matches / total > 0.80); 
}

// Helper Function for calcParity
static bool isBitAlwaysHigh(int bitNum, int *widths, int nwidths, float bTime) {
  int highCount = 0;
  int total = 0;
  uint32_t bitTimeX100 = (uint32_t)(bTime * 100.0f);

  for (int i = 2; i < nwidths - 15; i++) {
    if ((i % 2) != 0) continue; // Find start bit (LOW)
    
    uint32_t target = (bitTimeX100 * bitNum) + (bitTimeX100 / 2);
    uint32_t elapsed = 0;
    int pIdx = i;
    while (pIdx < nwidths && elapsed + (uint32_t)widths[pIdx]*100 <= target) {
      elapsed += (uint32_t)widths[pIdx] * 100;
      pIdx++;
    }
    
    if ((pIdx % 2) != 0) highCount++; // Odd index = HIGH state
    total++;
    
    i = pIdx; 
    if (total > 40) break;
  }
  
  if (total == 0) return false;
  // Use a 90% threshold to account for slight sampling jitter
  return ((float)highCount / total) > 0.90; 
}

static int calcParity(int frameSize, int stopBits, int *widths, int nwidths, float bTime) {
  int evenMatches = 0, oddMatches = 0, totalFrames = 0;
  
  // Example: 11-bit frame (8E1). Parity is at bit index 9.
  int parityBitIndex = frameSize - stopBits - 1; 
  uint32_t bitTimeX100 = (uint32_t)(bTime * 100.0f);

  for (int i = 2; i < nwidths - frameSize; i++) {
    if ((i % 2) != 0) continue; // Find start bit (LOW)
    
    int bits[15];
    uint32_t frameElapsedX100 = 0;
    int pIdx = i;

    // Read bits up to and including the parity bit
    for (int b = 1; b <= parityBitIndex; b++) {
      uint32_t targetCenter = (bitTimeX100 * b) + (bitTimeX100 / 2);
      while (pIdx < nwidths && frameElapsedX100 + (uint32_t)widths[pIdx]*100 <= targetCenter) {
        frameElapsedX100 += (uint32_t)widths[pIdx] * 100;
        pIdx++;
      }
      // 1 if HIGH (Odd index), 0 if LOW (Even index)
      bits[b] = ((pIdx % 2) != 0) ? 1 : 0; 
    }

    // Count Total 1s (Data + Parity)
    int totalOnes = 0;
    for (int k = 1; k <= parityBitIndex; k++) {
      if (bits[k]) totalOnes++;
    }

    // Even parity definition: Sum of 1s in Data+Parity is an Even number
    if (totalOnes % 2 == 0) evenMatches++;
    else oddMatches++;

    totalFrames++;
    i = pIdx;
    if (totalFrames > 35) break;
  }

  if (totalFrames < 5) return -1;
  float evenRate = (float)evenMatches / totalFrames;
  float oddRate = (float)oddMatches / totalFrames;

  if (evenRate > 0.70) return 0; // Even
  if (oddRate > 0.70) return 1;  // Odd
  return -1; // None
}

static int frameSize;
static int stopBits;
static int dataBits;
static int parity;
static float bitTime;

#define nwidths 200 //was 200 for accuracy

static int UART_line_settings_direct(struct bs_reply_s *reply, int index) {
  int widths[nwidths];
  int pin = gpioIndex[index];
  if (buildwidths(pin, widths, nwidths)) return -6;

  uartSpeedIndex = calcBaud(pin, widths, nwidths);
  if (uartSpeedIndex < 0) return -1;
  float bTime = uartInfo[uartSpeedIndex].microsDelay;

  // Determine ACTUAL Frame Size by finding the first guaranteed Stop bit
  int actualFrameSize = 10; // Default to 10 (8N1 size)

  if (isBitAlwaysHigh(8, widths, nwidths, bTime)) {
    actualFrameSize = 9;  // 7N1 (Bit 8 is Stop)
  } else if (isBitAlwaysHigh(9, widths, nwidths, bTime)) {
    actualFrameSize = 10; // 8N1, 7E1, 7O1 (Bit 9 is Stop)
  } else if (isBitAlwaysHigh(10, widths, nwidths, bTime)) {
    actualFrameSize = 11; // 8E1, 8O1, 8N2 (Bit 10 is Stop)
  }

  int dBits = 8, sBits = 1, pMode = 255;

  // Evaluate based on the locked-in Frame Size
  if (actualFrameSize == 11) {
    // 11 bits: Must be 8E1, 8O1, or 8N2
    int p = calcParity(11, 1, widths, nwidths, bTime);
    if (p >= 0) {
      dBits = 8; pMode = p; sBits = 1;
    } else {
      dBits = 8; pMode = 255; sBits = 2; // Math failed, assume 8N2
    }
  } 
  else if (actualFrameSize == 10) {
    // 10 bits: Must be 7E1, 7O1, or 8N1
    int p = calcParity(10, 1, widths, nwidths, bTime);
    if (p >= 0) {
      dBits = 7; pMode = p; sBits = 1;   // Math passed, it's 7-bit + Parity
    } else {
      dBits = 8; pMode = 255; sBits = 1; // Math failed, it's 8N1
    }
  } 
  else if (actualFrameSize == 9) {
    // 9 bits: Must be 7N1
    dBits = 7; pMode = 255; sBits = 1;
  } 
  else {
    // Fallback
    dBits = 8; pMode = 255; sBits = 1;
  }

  // Final sanity cap
  if (dBits > 8) dBits = 8;

  // Pack result in reply_data
  uint32_t *reply_data = (uint32_t *)&reply->bs_payload[0];
  reply_data[index*5 + 0] = gpio[index];
  reply_data[index*5 + 1] = dBits;
  reply_data[index*5 + 2] = sBits;
  reply_data[index*5 + 3] = pMode;
  reply_data[index*5 + 4] = uartInfo[uartSpeedIndex].baudRate;
  
  return 0;
}

struct bs_frame_s* 
UART_all_line_settings(struct bs_request_s *request)
{
  struct bs_frame_s *reply;
  int u = 0;

  reply = (struct bs_frame_s *)malloc(BS_HEADER_SIZE + 4*5*N_GPIO);
  if (reply == NULL)
    return NULL;
  reply->bs_payload_length = 4*5*N_GPIO;
  memset(reply->bs_payload, 0, 4*5*N_GPIO);
  for (int i = 0; i < N_GPIO; i++) {
    if (gpio[i] > 100) {
      u++;
      for (int attempt = 0; attempt < 3; attempt++) {
        int ret;
        system_soft_wdt_feed();
        
        ret = UART_line_settings_direct(reply, i);
        if (ret == 0) {
          u++;
          break;
        } else {
          if (ret == -6) { // Timeout
            break;
          }
        }
      }
    }
  }
  yield();
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

struct bs_frame_s* UART_passthrough(struct bs_request_s *request) {
    uint32_t *request_args = (uint32_t *)&request->bs_payload[0];
    int rx = gpioIndex[request_args[0]];
    int tx = gpioIndex[request_args[1]];
    int baud = request_args[2];

    // Clean up and Start
    if (swSerPtr) { swSerPtr->end(); delete swSerPtr; }
    swSerPtr = new SoftwareSerial(rx, tx, false);
    swSerPtr->begin(baud);

    // Send BS_REPLY_UART_PASSTHROUGH to Python so the terminal opens
    struct bs_frame_s *reply = (struct bs_frame_s *)malloc(BS_HEADER_SIZE);
    if (reply) {
        memcpy(reply, request, BS_HEADER_SIZE);
        reply->bs_command = BS_REPLY_UART_PASSTHROUGH;
        send_reply(request, reply);
        Serial.flush(); 
        free(reply);
    }

    // LOCKDOWN LOOP
    while (true) {
        if (swSerPtr->available()) {
            Serial.write(swSerPtr->read());
        }

        if (Serial.available()) {
            byte c = Serial.read();
            
            // ESCAPE SEQUENCE: Ctrl+X (ASCII 24)
            if (c == 24) {
                Serial.println(F("BUSSIDE_EXIT_REBOOT"));
                Serial.flush();
                delay(500);
                //ESP.restart(); // This is the only 100% reliable way to clear interrupts
                return NULL;
            }
            
            swSerPtr->write(c);
        }
        // Keep the system from crashing
        yield();
    }
    
    return NULL; // Never reached
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
