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

struct bs_frame_s* UART_set_config(struct bs_request_s *request) {
  // Safety: Ensure we actually have data to read
  if (request->bs_payload_length < 4) return NULL;

  uint32_t *args = (uint32_t *)&request->bs_payload[0];
  uint32_t mask = args[0];

  g_dataBits = (mask & 0x0F);
  g_stopBits = (mask >> 4) & 0x0F;
  
  uint32_t p_idx = (mask >> 8) & 0x0F;
  if (p_idx == 1) g_parity = 0;      // Even
  else if (p_idx == 2) g_parity = 1; // Odd
  else g_parity = -1;                // None

  // Allocation check
  struct bs_frame_s *reply = (struct bs_frame_s *)malloc(BS_HEADER_SIZE);
  if (reply) {
    memcpy(reply, request, BS_HEADER_SIZE);
    reply->bs_payload_length = 0;
    reply->bs_command = BS_UART_SET_CONFIG; // Set the command ID for the reply
  }
  return reply;
}

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

// in RAM for speed (be careful there's not much of it)
// further optimized by not doing any floating point math, should execute in roughly 1/10th the clock cycles of the original.
static int IRAM_ATTR tryFrameSize(int framesize, int *widths, int nwidths)
{
  uint32_t bitTimeX100 = (uint32_t)(uartInfo[uartSpeedIndex].microsDelay * 100.0f);
  uint32_t currentTime = 0;
  uint32_t frameStartTime = 0;
  bool inFrame = false;

  int validFrames = 0;
  int framingErrors = 0;

  for (int i = 2; i < nwidths; i++) {
    uint32_t pulseWidth = (uint32_t)widths[i] * 100;
    
    if (!inFrame) {
        // LOW pulse is a potential Start bit
        if (i % 2 == 0) { 
            inFrame = true;
            frameStartTime = currentTime;
        }
    }

    if (inFrame) {
        uint32_t stopPosX100 = (bitTimeX100 * (framesize - 1)) + (bitTimeX100 / 2);
        
        if (currentTime + pulseWidth > frameStartTime + stopPosX100) {
            // Reached the expected stop bit position
            if (i % 2 != 0) { 
                validFrames++; // Line is HIGH = Valid Stop
            } else {
                framingErrors++; // Line is LOW = Error
            }
            inFrame = false; // Synchronize to next falling edge
        }
    }
    currentTime += pulseWidth;
  }
  return (validFrames >= 2 && framingErrors == 0); // Strict error-free requirement
}

static void IRAM_ATTR analyzeFrames(int frameSize, int *widths, int nwidths, 
                                    int *parity7, int *parity8, 
                                    int *b8High, int *b9High, int *b10High, int *b11High,
                                    int *dataVaried) 
{
  uint32_t bitTimeX100 = (uint32_t)(uartInfo[uartSpeedIndex].microsDelay * 100.0f);
  
  int totalFrames = 0;
  int even7 = 0, odd7 = 0, even8 = 0, odd8 = 0;
  int c8 = 0, c9 = 0, c10 = 0, c11 = 0;
  int firstBits[12] = {0};
  
  *dataVaried = 0;
  uint32_t currentTime = 0, frameStartTime = 0;
  bool inFrame = false;

  for (int i = 2; i < nwidths; i++) {
    uint32_t pulseWidth = (uint32_t)widths[i] * 100;
    
    if (!inFrame) {
        if (i % 2 == 0) { inFrame = true; frameStartTime = currentTime; }
    }

    if (inFrame) {
        uint32_t stopPosX100 = (bitTimeX100 * (frameSize - 1)) + (bitTimeX100 / 2);
        
        if (currentTime + pulseWidth > frameStartTime + stopPosX100) {
            if (i % 2 != 0) { // Valid frame! Let's extract bits.
                int bits[12] = {0};
                for (int b = 0; b < 12; b++) {
                    uint32_t sampleTime = frameStartTime + bitTimeX100 * (b + 1) + (bitTimeX100 / 2);
                    uint32_t scanTime = 0;
                    for (int j = 2; j < nwidths; j++) {
                        scanTime += (uint32_t)widths[j] * 100;
                        if (sampleTime < scanTime) {
                            bits[b] = (j % 2 != 0) ? 1 : 0;
                            break;
                        }
                    }
                    if (sampleTime >= scanTime) bits[b] = 1; // Assume idle if out of bounds
                }

                if (totalFrames == 0) {
                    for(int b=0; b<7; b++) firstBits[b] = bits[b];
                } else {
                    for(int b=0; b<7; b++) {
                        if (bits[b] != firstBits[b]) *dataVaried = 1;
                    }
                }

                int ones7 = 0;
                for (int b = 0; b < 7; b++) ones7 += bits[b];
                if ((ones7 + bits[7]) % 2 == 0) even7++; else odd7++;

                int ones8 = ones7 + bits[7];
                if ((ones8 + bits[8]) % 2 == 0) even8++; else odd8++;

                if (bits[7] == 1) c8++;
                if (bits[8] == 1) c9++;
                if (bits[9] == 1) c10++;
                if (bits[10] == 1) c11++;

                totalFrames++;
            }
            inFrame = false;
        }
    }
    currentTime += pulseWidth;
  }

  *parity7 = -1; *parity8 = -1;
  *b8High = 0; *b9High = 0; *b10High = 0; *b11High = 0;

  if (totalFrames >= 3) {
    if (*dataVaried) { // Only trust parity if data payload actually changes
        if (even7 == totalFrames) *parity7 = 0; else if (odd7 == totalFrames) *parity7 = 1; 
        if (even8 == totalFrames) *parity8 = 0; else if (odd8 == totalFrames) *parity8 = 1;
    }
    if (c8 == totalFrames) *b8High = 1;
    if (c9 == totalFrames) *b9High = 1;
    if (c10 == totalFrames) *b10High = 1;
    if (c11 == totalFrames) *b11High = 1;
  }
}

static int IRAM_ATTR calcParity(int frameSize, int stopBits, int *widths, int nwidths)
{
  uint32_t bitTimeX100 = (uint32_t)(uartInfo[uartSpeedIndex].microsDelay * 100.0f);
  uint32_t wX100 = 0;
  int evenMatches = 0, oddMatches = 0, totalFrames = 0;

  for (int i = 2; i < nwidths - 1; i++) {
    uint32_t pWidthX100 = (uint32_t)widths[i] * 100;
    uint32_t nextWX100 = wX100 + pWidthX100;
    uint32_t stopPosX100 = (bitTimeX100 * (frameSize - stopBits)) + (bitTimeX100 / 2);

    if (stopPosX100 >= wX100 && stopPosX100 < nextWX100) {
      if (i % 2 != 0) { // Stop bit must be High
        int onBits = 0;
        int pBitVal = 1;

        // Sample 8 data bits (standard assumption)
        for (int b = 0; b < 8; b++) {
          uint32_t sample = bitTimeX100 * (b + 1) + (bitTimeX100 / 2);
          uint32_t cw = 0;
          for (int j = 2; j <= i; j++) {
            cw += (uint32_t)widths[j] * 100;
            if (sample < cw) {
              if (j % 2 != 0) onBits++;
              break;
            }
          }
        }

        // Sample 9th bit as Parity
        uint32_t pSample = bitTimeX100 * 9 + (bitTimeX100 / 2);
        uint32_t pcw = 0;
        for (int j = 2; j <= i; j++) {
          pcw += (uint32_t)widths[j] * 100;
          if (pSample < pcw) {
            pBitVal = (j % 2 != 0) ? 1 : 0;
            break;
          }
        }

        if ((onBits + pBitVal) % 2 == 0) evenMatches++; else oddMatches++;
        totalFrames++;
      }
      wX100 = 0;
    } else {
      wX100 = nextWX100;
    }
  }

  if (totalFrames < 3) return -1;
  if (evenMatches == totalFrames) return 0;
  if (oddMatches == totalFrames) return 1;
  return -1;
}

static int frameSize;
static int stopBits;
static int dataBits;
static int parity;
static float bitTime;

#define NWIDTHS 300 //was 200
static int UART_line_settings_direct(struct bs_reply_s *reply, int index, int *widths)
{
  int pin = gpioIndex[index];
  uint32_t *reply_data;

  pinMode(pin, (pin == 16) ? INPUT : INPUT_PULLUP);
  if (buildwidths(pin, widths, NWIDTHS)) return -6; 

  uartSpeedIndex = calcBaud(pin, widths, NWIDTHS);
  if (uartSpeedIndex < 0) return -1;
  
  // 1. Detect Smallest Valid Size (SVS) using the new timeline logic
  int svs = -1;
  int trySizes[] = {9, 10, 11, 12}; 
  for (int i = 0; i < 4; i++) {
    // FIX: Removed the extra '1' argument that caused the compiler error
    if (tryFrameSize(trySizes[i], widths, NWIDTHS)) {
      svs = trySizes[i];
      break; 
    }  
  }
  if (svs < 0) return -1;
  frameSize = svs;

  // 2. Extract Statistics
  int parity7, parity8, b8H, b9H, b10H, b11H, dataVaried;
  // FIX: Arguments now match the 10-parameter signature of analyzeFrames
  analyzeFrames(frameSize, widths, NWIDTHS, &parity7, &parity8, &b8H, &b9H, &b10H, &b11H, &dataVaried);

  // 3. The Logic Bridge (Deterministic Mapping)
  // We use the "Always High" bit counts to distinguish 1 vs 2 stop bits
  if (svs == 9) {
      dataBits = 7;
      parity = -1;
      stopBits = b9H ? 2 : 1;
  } 
  else if (svs == 10) {
      if (parity7 >= 0) {
          dataBits = 7;
          parity = parity7;
          stopBits = b10H ? 2 : 1;
      } else {
          dataBits = 8;
          parity = -1;
          stopBits = b10H ? 2 : 1;
      }
  } 
  else if (svs == 11) {
      if (parity8 >= 0) {
          dataBits = 8;
          parity = parity8;
          stopBits = b11H ? 2 : 1;
      } else if (parity7 >= 0) {
          dataBits = 7;
          parity = parity7;
          stopBits = 2; // 7,P,2
      } else {
          dataBits = 8;
          parity = -1;
          stopBits = 2; // 8,N,2
      }
  }
  else if (svs == 12) {
      dataBits = 8;
      parity = (parity8 >= 0) ? parity8 : -1;
      stopBits = 2; 
  }
  else {
      dataBits = 8; stopBits = 1; parity = -1; 
  }

  // Pack the reply
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
  int *widthsBuffer = (int *)malloc(NWIDTHS * sizeof(int));
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

struct bs_frame_s* UART_passthrough(struct bs_request_s *request)
{
  uint32_t *request_args = (uint32_t *)&request->bs_payload[0];
  int rx_pin = gpioIndex[request_args[0]];
  int tx_pin = gpioIndex[request_args[1]];
  
  uint32_t raw_baud = request_args[2];
  uint32_t baud = raw_baud & 0x00FFFFFF; 

  const uint32_t cycles_per_bit = 160000000UL / baud;
  const uint32_t half_bit = cycles_per_bit / 2;

  pinMode(rx_pin, INPUT);
  pinMode(tx_pin, OUTPUT);
  digitalWrite(tx_pin, HIGH);
  delay(20);

  // Use globals or defaults if not set
  int d_bits = g_dataBits ? g_dataBits : 8;
  int parity_m = g_parity; // -1: None, 0: Even, 1: Odd
  int stop_b = g_stopBits ? g_stopBits : 1;

  struct bs_frame_s *reply = (struct bs_frame_s *)malloc(BS_HEADER_SIZE);
  if (reply) {
    memcpy(reply, request, BS_HEADER_SIZE);
    reply->bs_command = BS_REPLY_UART_PASSTHROUGH;
    send_reply(request, reply);
    Serial.flush();
    free(reply);
  }

  while (1) {
    // --- RX Logic (Receive from Device, Send to USB) ---
    if (digitalRead(rx_pin) == LOW) {
      uint32_t start_cycles = asm_ccount();
      uint8_t data = 0;
      // Sample data bits + parity (if applicable)
      int total_to_sample = d_bits + (parity_m >= 0 ? 1 : 0);

      noInterrupts();
      // Wait for Start Bit to pass
      while ((asm_ccount() - start_cycles) < (cycles_per_bit + half_bit));
      
      for (int i = 0; i < total_to_sample; i++) {
        bool bit_val = digitalRead(rx_pin);
        if (i < d_bits && bit_val) {
            data |= (1 << i);
        }
        // Parity bit is sampled but ignored in this simple passthrough logic
        uint32_t target = start_cycles + (cycles_per_bit * (i + 2)) + half_bit;
        while (asm_ccount() < target);
      }
      interrupts();

      while (((READ_PERI_REG(UART_STATUS(0)) >> UART_TXFIFO_CNT_S) & UART_TXFIFO_CNT) >= 120);
      WRITE_PERI_REG(UART_FIFO(0), data);
    }

    // --- TX Logic (Receive from USB, Send to Device) ---
    if (Serial.available() > 0) {
      uint8_t c = Serial.read();
      if (c == 24) break; // Ctrl+X to exit

      int p_bit = 0;
      if (parity_m >= 0) {
        int ones = 0;
        for(int b=0; b < d_bits; b++) if ((c >> b) & 0x01) ones++;
        // Even parity: p_bit makes total ones even. Odd parity: p_bit makes total ones odd.
        p_bit = (parity_m == 0) ? (ones % 2) : !(ones % 2);
      }

      noInterrupts();
      uint32_t bit_start = asm_ccount();
      digitalWrite(tx_pin, LOW); // Start bit
      
      // Data bits
      for(int b=0; b < d_bits; b++) {
        while(asm_ccount() - bit_start < cycles_per_bit * (b + 1));
        digitalWrite(tx_pin, (c >> b) & 0x01);
      }

      int current_bit_offset = d_bits + 1;
      // Parity bit
      if (parity_m >= 0) {
        while(asm_ccount() - bit_start < cycles_per_bit * current_bit_offset);
        digitalWrite(tx_pin, p_bit);
        current_bit_offset++;
      }

      // Stop bits
      while(asm_ccount() - bit_start < cycles_per_bit * current_bit_offset);
      digitalWrite(tx_pin, HIGH); 
      
      if (stop_b >= 2) {
        while(asm_ccount() - bit_start < cycles_per_bit * (current_bit_offset + 1));
      }
      interrupts();
    }
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
  // Map Global Settings to EspSoftwareSerial Config
  EspSoftwareSerial::Config config = SWSERIAL_8N1; 

  if (g_dataBits == 7) {
    if (g_parity == 0)      config = (g_stopBits >= 2) ? SWSERIAL_7E2 : SWSERIAL_7E1;
    else if (g_parity == 1) config = (g_stopBits >= 2) ? SWSERIAL_7O2 : SWSERIAL_7O1;
    else                    config = (g_stopBits >= 2) ? SWSERIAL_7N2 : SWSERIAL_7N1;
  } else { 
    // Default to 8 bits
    if (g_parity == 0)      config = (g_stopBits >= 2) ? SWSERIAL_8E2 : SWSERIAL_8E1;
    else if (g_parity == 1) config = (g_stopBits >= 2) ? SWSERIAL_8O2 : SWSERIAL_8O1;
    else                    config = (g_stopBits >= 2) ? SWSERIAL_8N2 : SWSERIAL_8N1;
  }
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
