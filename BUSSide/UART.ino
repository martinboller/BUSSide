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
  // Scale by 100 to keep two decimal places of precision without floats
  uint32_t bitTimeX100 = (uint32_t)(uartInfo[uartSpeedIndex].microsDelay * 100.0f);
  
  // theoretical stop bit start time: bitTime * (framesize - stopbits + 0.5)
  // We use (stopbits * 2 - 1) * 50 to represent the "+ 0.5" offset in X100 scale
  uint32_t stopTimeX100 = (bitTimeX100 * (framesize - stopbits)) + (bitTimeX100 / 2);
  
  uint32_t wX100 = 0;
  int framingErrors = 0;

  for (int i = 2; i < nwidths - 1; i++) {
    uint32_t currentWidthX100 = (uint32_t)widths[i] * 100;
    uint32_t nextWX100 = wX100 + currentWidthX100;

    // Check if the current pulse spans across the expected Stop Bit timing
    if (stopTimeX100 >= wX100 && stopTimeX100 < nextWX100) {
      // UART Stop bits must be HIGH. 
      // If widths[0] (start bit level) was HIGH, then even indices are LOW.
      // If widths[0] was LOW, then even indices are HIGH.
      if ((i % 2) != widths[0]) {
        framingErrors++;
        if (framingErrors >= 1) return 0;
      }
      wX100 = 0; // Reset for next frame alignment
    } else {
      wX100 = nextWX100;
    }
  }
  return 1;
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

#define nwidths 20 //was 200 for accuracy

static int UART_line_settings_direct(struct bs_reply_s *reply, int index)
{
  int widths[nwidths];
  char s[100];
  unsigned long timeStart;
  int n;
  int ret;
  int pin;
  uint32_t *reply_data;

  pin = gpioIndex[index];
  if (pin == D0)
    pinMode(pin, INPUT);
  else
    pinMode(pin, INPUT_PULLUP);

  ret = buildwidths(pin, widths, nwidths);
  if (ret) {
    return -6;
  }
  uartSpeedIndex = calcBaud(pin, widths, nwidths);
  if (uartSpeedIndex < 0) {
    return -1;
  }
  bitTime = uartInfo[uartSpeedIndex].microsDelay;

  // Initialize results
  frameSize = -1;
  int detectedFrame = -1;

  if (ret != 0) {
    delay(10); // Wait for activity
  }

  // Check 10 and 11 first because they are the most typical UART traffic by far
  int prioritySizes[] = {10, 11, 9, 8, 7, 12, 13};
  for (int i = 0; i < 7; i++) {
    int testSize = prioritySizes[i];
    if (tryFrameSize(testSize, 1, widths, nwidths)) {
      detectedFrame = testSize;
      break; // Found a valid size for this sample
  }  
    if (detectedFrame != -1) break; // On hit, stop sampling
    yield();
  }
  frameSize = detectedFrame;
  
  if (frameSize < 0) {
    return -1;
  }

  if (tryFrameSize(frameSize, 2, widths, nwidths)){
    stopBits = 2;
  } else {
    stopBits = 1;
  }

  parity = calcParity(frameSize, stopBits, widths, nwidths);
  if (parity == -2) {
    return -1;
  } else if (parity < 0) {
    dataBits = frameSize - stopBits - 1;
  } else {
    dataBits = frameSize - stopBits - 1 - 1;
    if (parity == 0) {
    } else {
    }
  }

  reply_data = (uint32_t *)&reply->bs_payload[0];
  reply_data[index*5 + 0] = gpio[index];
  reply_data[index*5 + 1] = dataBits;
  reply_data[index*5 + 2] = stopBits;
  reply_data[index*5 + 3] = parity;
  reply_data[index*5 + 4] = uartInfo[uartSpeedIndex].baudRate;
  yield();
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
