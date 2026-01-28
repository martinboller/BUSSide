
#include <SoftwareSerial.h>

#include <Boards.h>
#include <pins_arduino.h>
#include "BUSSide.h"
//#include <ESP8266WiFi.h>

#define microsTime() (micros())

void reset_gpios();

int gpio[N_GPIO];
int gpioIndex[N_GPIO] = { D0, D1, D2, D3, D4, D5, D6, D7, D8 };
const char *gpioName[] = { "D0", "D1", "D2", "D3", "D4", "D5", "D6", "D7", "D8" };

byte pins[] = { D0, D1, D2, D3, D4, D5, D6, D7, D8 };
const char * pinnames[] = { "D0", "D1", "D2", "D3", "D4", "D5", "D6", "D7", "D8", };
const byte pinslen = sizeof(pins)/sizeof(pins[0]);  

uint32_t crc_table[16] = {
    0x00000000, 0x1db71064, 0x3b6e20c8, 0x26d930ac,
    0x76dc4190, 0x6b6b51f4, 0x4db26158, 0x5005713c,
    0xedb88320, 0xf00f9344, 0xd6d6a3e8, 0xcb61b38c,
    0x9b64c2b0, 0x86d3d2d4, 0xa00ae278, 0xbdbdf21c
};

unsigned long crc_update(unsigned long crc, byte data)
{
    byte tbl_idx;
    tbl_idx = crc ^ (data >> (0 * 4));
    crc = crc_table[tbl_idx & 0x0f] ^ (crc >> 4);
    tbl_idx = crc ^ (data >> (1 * 4));
    crc = crc_table[tbl_idx & 0x0f] ^ (crc >> 4);
    return crc;
}

unsigned long crc_mem(const char *s, int n)
{
  unsigned long crc = ~0L;
  for (int i = 0; i < n; i++)
    crc = crc_update(crc, s[i]);
  crc = ~crc;
  return crc;
}

void setup() {
  Serial.begin(115200); // Keeping it at 115200 for now since it's working
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, HIGH); // LED Off
  
  // No Serial.print here! Total silence.
}

void reset_gpios()
{
  int gpioIndex[N_GPIO] = { D0, D1, D2, D3, D4, D5, D6, D7, D8 };
  const char *gpioName[N_GPIO] = { "D0", "D1", "D2", "D3", "D4", "D5", "D6", "D7", "D8" };

  for (int i = 0; i < N_GPIO; i++) {
    pinMode(gpioIndex[i], INPUT);
  }
}

uint32_t sequence_number = 1;
uint8_t sync[] = "\xfe\xca";

void send_reply(struct bs_request_s *request, struct bs_reply_s *reply)
{
    Serial.printf("send_reply start");
    Serial.write(sync, 2);
    Serial.flush();
    reply->bs_sequence_number = request->bs_sequence_number;
    reply->bs_checksum = 0;
    reply->bs_checksum = crc_mem((const char *)reply, BS_HEADER_SIZE + reply->bs_payload_length);
    Serial.write((uint8_t *)reply, BS_HEADER_SIZE + reply->bs_payload_length);
    Serial.flush();
    Serial.printf("send_reply end");
}

void FlushIncoming()
{
  while (Serial.available() > 0) {
    (void)Serial.read();
  }
}

void Sync() {
  pinMode(LED_BUILTIN, OUTPUT);
  unsigned long lastBlink = 0;
  
  while (1) {
    yield();

    // Blink LED every 500ms to show we are alive and waiting
    if (millis() - lastBlink > 500) {
      digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));
      lastBlink = millis();
    }

    if (Serial.available() > 0) {
      int ch = Serial.read();
      if (ch == 0xfe) {
        Serial.println("DEBUG: Got FE"); // If you see this, the connection is alive!
        uint32_t start = millis();
        while (Serial.available() == 0 && (millis() - start < 100)) yield();
        if (Serial.read() == 0xca) {
           Serial.println("DEBUG: Synced!");
           return;
        }
      }
    }
  }
}

// void loop() {
//   // Simple Sync check
//   if (Serial.available() >= 2) {
//     byte b1 = Serial.read();
//     byte b2 = Serial.read();
    
//     if (b1 == 0xfe && b2 == 0xca) {
//       // Send the success byte
//       Serial.write(0xaa);
      
//       // Blink to confirm
//       digitalWrite(LED_BUILTIN, LOW);
//       delay(500);
//       digitalWrite(LED_BUILTIN, HIGH);
//     }
//   }
//   yield();
// }

void loop() {
    struct bs_frame_s header;
    struct bs_frame_s *request = NULL;
    struct bs_frame_s *reply = NULL;
    int rv;

    // 1. Wait for the sync sequence
    Sync(); 

    // 2. Read the header
    Serial.setTimeout(1000);
    rv = Serial.readBytes((char *)&header, BS_HEADER_SIZE);
    if (rv < BS_HEADER_SIZE) {
        // Serial.println("Err: Header Timeout");
        FlushIncoming();
        return;
    }

    // 3. Validate Payload Length (Safety Check)
    // ESP8266 has ~80KB RAM total; don't allow massive allocations
    if (header.bs_payload_length > 32768) { 
        // Serial.println("Err: Payload too large");
        FlushIncoming();
        return;
    }

    // 4. Allocate memory on the HEAP
    request = (struct bs_frame_s *)malloc(BS_HEADER_SIZE + header.bs_payload_length);
    if (request == NULL) {
        // Serial.println("Err: Malloc failed");
        FlushIncoming();
        return;
    }

    // 5. Build the request object
    memcpy(request, &header, BS_HEADER_SIZE);
    if (request->bs_payload_length > 0) {
        rv = Serial.readBytes((char *)request->bs_payload, request->bs_payload_length);
        if (rv < (int)request->bs_payload_length) {
            // Serial.println("Err: Payload Timeout");
            free(request);
            FlushIncoming();
            return;
        }
    }

    // 6. Verify Checksum
    uint32_t received_crc = request->bs_checksum;
    request->bs_checksum = 0; // Must be 0 for CRC calculation
    uint32_t calculated_crc = crc_mem((const char *)request, BS_HEADER_SIZE + request->bs_payload_length);

    if (calculated_crc != received_crc) {
        // Serial.printf("Err: CRC Mismatch (Recv: %08X, Calc: %08X)\n", received_crc, calculated_crc);
        free(request);
        return;
    }

    // 7. Sequence Number Check
    if (request->bs_sequence_number <= sequence_number && request->bs_sequence_number != 0) {
        free(request);
        return;
    }
    sequence_number = request->bs_sequence_number;

    // 8. Process Commands
    ESP.wdtFeed(); // Keep the watchdog happy before long operations
    
    switch (request->bs_command) {
        case BS_I2C_DISCOVER_SLAVES: reply = discover_I2C_slaves(request); break;
        case BS_I2C_FLASH_DUMP:      reply = read_I2C_eeprom(request); break;
        case BS_I2C_FLASH:           reply = write_I2C_eeprom(request); break;
        case BS_I2C_DISCOVER:        reply = I2C_active_scan(request); break;
        case BS_SPI_FLASH_DUMP:      reply = read_SPI_flash(request); break;
        case BS_SPI_READID:          reply = SPI_read_id(request); break;
        case BS_JTAG_DISCOVER_PINOUT: reply = JTAG_scan(request); break;
        case BS_ECHO:
            reply = (struct bs_frame_s *)malloc(BS_HEADER_SIZE + request->bs_payload_length);
            if (reply) {
                memcpy(reply, request, BS_HEADER_SIZE + request->bs_payload_length);
                reply->bs_command = BS_REPLY_ECHO;
            }
            break;
        // ... Add other cases as needed ...
        default:
            reply = NULL;
            break;
    }

    // 9. Cleanup and Reply
    reset_gpios(); 
    if (reply != NULL) {
        send_reply(request, reply);
        free(reply);
    }
    
    free(request); // Important: Free the malloc'd memory
    yield();       // Allow ESP8266 background tasks to run
}

    // reset_gpios();
    // Serial.printf("GPIOS Reset");
    // send_reply(request, reply);
    // Serial.printf("Reply Sent");
    // free(reply);
    // Serial.printf("Free");    
//}
