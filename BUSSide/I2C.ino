#include "BUSSide.h"
#include <Wire.h>

struct bs_frame_s*
// ... variable declarations ...
  
// 1. EXTRACT ARGUMENTS: The PC sends 32-bit integers in the payload
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
  slaveAddress = request_args[0]; // I2C 7-bit address
  readsize = request_args[1]; // Total bytes to read
  skipsize = request_args[2]; // Starting memory address offset
  sdaPin = request_args[3] - 1; // Map human-friendly pin to array index
  sclPin = request_args[4] - 1;
  addressLength = request_args[5]; // 1 or 2 bytes for memory addressing

  // 2. ALLOCATE REPLY: Space for header + the requested data
  reply = (struct bs_frame_s *)malloc(BS_HEADER_SIZE + readsize);
  if (reply == NULL)
    return NULL;

  // 2. ALLOCATE REPLY: Space for header + the requested data
  reply->bs_command = BS_REPLY_I2C_FLASH_DUMP;
  reply_data = (uint8_t *)&reply->bs_payload[0];

  // 4. SET MEMORY POINTER: Tell where we want to start reading
  Wire.begin(gpioIndex[sdaPin], gpioIndex[sclPin]);  
  
  Wire.beginTransmission(slaveAddress);
  switch (addressLength) {
    case 2: // For larger EEPROMs (e.g., 24C64+) requiring 16-bit addresses
      Wire.write((skipsize & 0xff00) >> 8); // send the high byte of the EEPROM memory address
    case 1: // For smaller EEPROMs (e.g., 24C02) requiring 8-bit addresses
      Wire.write((skipsize & 0x00ff)); // send the low byte
      break;
    default:
      free(reply);
      return NULL;
  }
  Wire.endTransmission(); // Execute the pointer set

  // 5. BULK READ: Request data in small chunks (8 bytes at a time)
  // This helps avoid overflowing the small internal Wire buffer (32 bytes)
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


int write_byte_I2C_eeprom(uint8_t slaveAddress, uint32_t skipsize, int addressLength, uint32_t val)
{
    // ... (rest of your switch and write logic)
    
    for (int i = 0; i < 100; i++) {
      Wire.beginTransmission(slaveAddress);
      if (Wire.endTransmission() == 0) {
        return 0; // Success
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

void
I2C_active_scan1(struct bs_request_s *request, struct bs_reply_s *reply, int sdaPin, int sclPin)
{
  uint32_t *reply_data;
  int numberOfSlaves;
  
  Wire.begin(gpioIndex[sdaPin], gpioIndex[sclPin]);  
  numberOfSlaves = 0;
  for (int slaveAddress = 0; slaveAddress < 128; slaveAddress++) {
    int rv1, rv2;

    ESP.wdtFeed();
    Wire.beginTransmission(slaveAddress);
    if (Wire.endTransmission() == 0) {
      int n;
      int gotitalready;
      
#define BYTESTOREAD 8
      n = Wire.requestFrom(slaveAddress, BYTESTOREAD);
      if (n != BYTESTOREAD)
        continue;
      ESP.wdtFeed();
      numberOfSlaves++;
    }
  }
  if (numberOfSlaves > 0) {
    int index;
    
    // Simplified: Skip the 10 verification scans for faster discovery
    index = reply->bs_payload_length / 8;
    reply_data = (uint32_t *)&reply->bs_payload[0];
    reply_data[2*index + 0] = sdaPin + 1;
    reply_data[2*index + 1] = sclPin + 1;
    reply->bs_payload_length += 4*2;
  }
}

struct bs_frame_s*
I2C_active_scan(struct bs_request_s *request)
{
  struct bs_frame_s *reply;
  uint32_t *request_args;
  
  reply = (struct bs_reply_s *)malloc(BS_HEADER_SIZE + 4*2*50);
  if (reply == NULL)
    return NULL;

  reply->bs_payload_length = 0;
    
  for (int sda_pin=1; sda_pin < N_GPIO; sda_pin++) {
    ESP.wdtFeed();
    for(int scl_pin = 1; scl_pin < N_GPIO; scl_pin++) {
      ESP.wdtFeed();
      if (sda_pin == scl_pin)
        continue;
      I2C_active_scan1(request, reply, sda_pin, scl_pin);
    }
  }
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
