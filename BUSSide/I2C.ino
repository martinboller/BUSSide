#include "BUSSide.h"
#include <Wire.h>

struct bs_frame_s*
read_I2C_eeprom(struct bs_request_s *request)
{
  struct bs_frame_s *reply;
  uint32_t *request_args;
  uint32_t readsize, count, skipsize;
  uint8_t slaveAddress;
  uint8_t *reply_data;
  uint32_t i;
  int sdaPin, sclPin;
  int addressLength;
  
  request_args = (uint32_t *)&request->bs_payload[0];
  slaveAddress = request_args[0];
  readsize = request_args[1];
  skipsize = request_args[2];
  sdaPin = request_args[3] - 1;
  sclPin = request_args[4] - 1;
  addressLength = request_args[5];

  reply = (struct bs_frame_s *)malloc(BS_HEADER_SIZE + readsize);
  if (reply == NULL)
    return NULL;

  reply->bs_command = BS_REPLY_I2C_FLASH_DUMP;
  reply_data = (uint8_t *)&reply->bs_payload[0];
  
  Wire.begin(gpioIndex[sdaPin], gpioIndex[sclPin]);  
  
  Wire.beginTransmission(slaveAddress);
  switch (addressLength) {
    case 2:
      Wire.write((skipsize & 0xff00) >> 8); // send the high byte of the EEPROM memory address
    case 1:
      Wire.write((skipsize & 0x00ff)); // send the low byte
      break;
    default:
      free(reply);
      return NULL;
  }
  Wire.endTransmission(); 

  i = 0;
  while (readsize > 0) {
    uint32_t gotRead;
    
    gotRead = Wire.requestFrom(slaveAddress, readsize > 8 ? 8 : readsize);
    readsize -= gotRead;
    count = 0;
    while (count < gotRead) {
      if (Wire.available()) {
        uint8_t data;
      
        data = Wire.read();
        reply_data[i++] = data;
        count++;
      }
    }
  }
  reply->bs_payload_length = request_args[1];
  return reply;
}


int
write_byte_I2C_eeprom(uint8_t slaveAddress, uint32_t skipsize, int addressLength, uint32_t val)
{
    Wire.beginTransmission(slaveAddress);
    switch (addressLength) {
      case 2:
        // High byte first
        Wire.write((uint8_t)((skipsize >> 8) & 0xFF));
      case 1:
        Wire.write((skipsize & 0x00ff)); 
        break;
      default:
        Wire.endTransmission();
        return -1;
    }
    Wire.write(val);
    Wire.endTransmission();
    
    for (int i = 0; i < 100; i++) {
      Wire.beginTransmission(slaveAddress);
      if (Wire.endTransmission() == 0) {
        return 0; // Success!
      }
      delay(10);
    }

    return -1; // <--- ADD THIS: Return -1 if the loop times out
}

struct bs_frame_s*
write_I2C_eeprom(struct bs_request_s *request)
{
  uint32_t *request_args;
  struct bs_frame_s *reply;
  uint32_t writesize, skipsize;
  uint8_t slaveAddress;
  uint8_t *request_data;
  int sdaPin, sclPin;
  int addressLength;
  
  request_args = (uint32_t *)&request->bs_payload[0];
  slaveAddress = request_args[0];
  writesize = request_args[1];
  skipsize = request_args[2];
  sdaPin = request_args[3] - 1;
  sclPin = request_args[4] - 1;
  addressLength = request_args[5];
  request_data = (uint8_t *)&request_args[6];

  reply = (struct bs_frame_s *)malloc(BS_HEADER_SIZE);
  if (reply == NULL)
    return NULL;

  reply->bs_command = BS_REPLY_I2C_FLASH;

  Wire.begin(gpioIndex[sdaPin], gpioIndex[sclPin]);  
  while (writesize > 0) {
    int retry, rv;
    
    rv = write_byte_I2C_eeprom(slaveAddress, skipsize, addressLength, request_data[skipsize]);
    retry = 0;
    while (rv && retry < 5) {
      rv = write_byte_I2C_eeprom(slaveAddress, skipsize, addressLength, request_data[skipsize]); 
      delay(50);   
      retry++;
    }
    skipsize++;
    writesize--;
    ESP.wdtFeed();
  }
  reply->bs_payload_length = request_args[1];
  return reply;
}

static void I2C_active_scan1(struct bs_request_s *request, struct bs_frame_s *reply, int sdaPin, int sclPin)
{
    uint32_t *reply_data;
    const int MAX_ENTRIES = 50; 
    
    int sdaGPIO = gpioIndex[sdaPin];
    int sclGPIO = gpioIndex[sclPin];

    // 1. SAFETY CHECK: Ensure lines aren't physically shorted to GND
    // We use pullups and check if the pins actually go HIGH.
    pinMode(sdaGPIO, INPUT_PULLUP);
    pinMode(sclGPIO, INPUT_PULLUP);
    delayMicroseconds(50); 
    
    if (digitalRead(sdaGPIO) == LOW || digitalRead(sclGPIO) == LOW) {
        // Line is stuck LOW (hardware issue or no pullup). 
        // We MUST skip this pair to avoid a library hang.
        return; 
    }

    // 2. INITIALIZE WIRE
    Wire.begin(sdaGPIO, sclGPIO);  
    Wire.setClock(100000);
    
    // Corrected member name for ESP8266 Wire library
    Wire.setTimeout(500); // 500ms timeout

    bool found = false;
    // Scan standard 7-bit address range
    for (int addr = 0x08; addr < 0x78; addr++) {
        Wire.beginTransmission(addr);
        // endTransmission returns 0 on success
        if (Wire.endTransmission() == 0) {
            found = true;
            break; 
        }
    }

    // 3. LOG SUCCESSFUL PIN PAIR
    if (found) {
        int index = reply->bs_payload_length / 8; 
        if (index < MAX_ENTRIES) {
            reply_data = (uint32_t *)&reply->bs_payload[0];
            reply_data[2 * index + 0] = (uint32_t)(sdaPin + 1);
            reply_data[2 * index + 1] = (uint32_t)(sclPin + 1);
            reply->bs_payload_length += 8;
        }
    }
    
    // 4. CLEANUP: Reset pins to floating input state
    // This prevents the I2C hardware logic from "locking" the pins
    pinMode(sdaGPIO, INPUT);
    pinMode(sclGPIO, INPUT);
}

struct bs_frame_s* I2C_active_scan(struct bs_request_s *request)
{
  struct bs_frame_s *reply;
  reply = (struct bs_frame_s *)malloc(BS_HEADER_SIZE + (8 * 50));
  if (reply == NULL) return NULL;

  reply->bs_command = BS_REPLY_I2C_DISCOVER;
  reply->bs_payload_length = 0;
    
  for (int sda_pin = 0; sda_pin < N_GPIO; sda_pin++) {
    ESP.wdtFeed(); // Feed every SDA row
    for(int scl_pin = 0; scl_pin < N_GPIO; scl_pin++) {
      if (sda_pin == scl_pin) continue;
      
      I2C_active_scan1(request, reply, sda_pin, scl_pin);
    }
    
    // Only yield between SDA pin changes to prevent buffer overflow 
    // without breaking the I2C timing inside the SCL loop.
    yield(); 
  }
  Wire.endTransmission(); // Ensure bus is released
    for (int i = 0; i < N_GPIO; i++) {
        pinMode(gpioIndex[i], INPUT); // Force all pins back to high-impedance
    }
    
    // Give the ESP8266 a moment to let background tasks settle 
    // before returning to the main loop.
    delay(50);
  yield();
  return reply;
}
struct bs_frame_s*
discover_I2C_slaves(struct bs_request_s *request)
{
  struct bs_frame_s *reply;
  uint32_t *request_args, *reply_data;
  int sdaPin, sclPin;
  uint32_t count;
  
  request_args = (uint32_t *)&request->bs_payload[0];
  sdaPin = request_args[0] - 1;
  sclPin = request_args[1] - 1;

  reply = (struct bs_frame_s *)malloc(BS_HEADER_SIZE + 128*4);
  if (reply == NULL)
    return NULL;

  reply_data = (uint32_t *)&reply->bs_payload[0];
  
  Wire.begin(gpioIndex[sdaPin], gpioIndex[sclPin]);  
  reply->bs_command = BS_REPLY_I2C_DISCOVER_SLAVES; 
  reply->bs_payload_length = 0;
  count = 0;
  for (int slaveAddress = 0; slaveAddress < 128; slaveAddress++) {
    ESP.wdtFeed();
    Wire.beginTransmission(slaveAddress);
    if (Wire.endTransmission() == 0) { // if (no errors)
      reply_data[count++] = slaveAddress;
      reply->bs_payload_length += 4;
    }
  }
  return reply;
}
