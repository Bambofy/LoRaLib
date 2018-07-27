#include "SX127x.h"

SX127x::SX127x(Module* mod) {
  _mod = mod;
}

int16_t SX127x::begin(uint8_t chipVersion, uint8_t syncWord, uint8_t currentLimit, uint16_t preambleLength) {
  // set module properties
  _mod->init(USE_SPI, INT_BOTH);
  
  // try to find the SX127x chip
  uint8_t i = 0;
  bool flagFound = false;
  while((i < 10) && !flagFound) {
    int16_t version = _mod->SPIreadRegister(SX127X_REG_VERSION);
    if(version == chipVersion) {
      flagFound = true;
    } else {
      #ifdef KITELIB_DEBUG
        Serial.print(F("SX127x not found! ("));
        Serial.print(i + 1);
        Serial.print(F(" of 10 tries) SX127X_REG_VERSION == "));
        
        char buffHex[7];
        sprintf(buffHex, "0x%04X", version);
        Serial.print(buffHex);
        Serial.print(F(", expected 0x00"));
        Serial.print(chipVersion, HEX);
        Serial.println();
      #endif
      delay(1000);
      i++;
    }
  }
  if(!flagFound) {
    DEBUG_PRINTLN_STR("No SX127x found!");
    SPI.end();
    return(ERR_CHIP_NOT_FOUND);
  } else {
    DEBUG_PRINTLN_STR("Found SX127x!");
  }
  
  // set LoRa sync word
  int16_t state = SX127x::setSyncWord(syncWord);
  if(state != ERR_NONE) {
    return(state);
  }
  
  // set over current protection
  state = SX127x::setCurrentLimit(currentLimit);
  if(state != ERR_NONE) {
    return(state);
  }
  
  // set preamble length
  state = SX127x::setPreambleLength(preambleLength);
  if(state != ERR_NONE) {
    return(state);
  }
  
  return(ERR_NONE);
}

int16_t SX127x::transmit(String& str) {
  return(SX127x::transmit(str.c_str()));
}

int16_t SX127x::transmit(const char* str) {
  return(SX127x::transmit((uint8_t*)str, strlen(str)));
}

int16_t SX127x::transmit(uint8_t* data, size_t len) {
  // check packet length
  if(len >= 256) {
    return(ERR_PACKET_TOO_LONG);
  }

  // calculate timeout
  uint16_t base = 1;
  float symbolLength = (float)(base << _sf) / (float)_bw;
  float de = 0;
  if(symbolLength >= 0.016) {
    de = 1;
  }
  float ih = (float)_mod->SPIgetRegValue(SX127X_REG_MODEM_CONFIG_1, 0, 0);
  float crc = (float)(_mod->SPIgetRegValue(SX127X_REG_MODEM_CONFIG_2, 2, 2) >> 2);
  float n_pre = (float)_mod->SPIgetRegValue(SX127X_REG_PREAMBLE_LSB);
  float n_pay = 8.0 + max(ceil((8.0 * (float)len - 4.0 * (float)_sf + 28.0 + 16.0 * crc - 20.0 * ih)/(4.0 * (float)_sf - 8.0 * de)) * (float)_cr, 0.0);
  uint32_t timeout = ceil(symbolLength * (n_pre + n_pay + 4.25) * 1000.0);
  
  // set mode to standby
  setMode(SX127X_STANDBY);
  
  // set DIO mapping
  _mod->SPIsetRegValue(SX127X_REG_DIO_MAPPING_1, SX127X_DIO0_TX_DONE, 7, 6);
  
  // clear interrupt flags
  clearIRQFlags();
  
  // set packet length
  _mod->SPIsetRegValue(SX127X_REG_PAYLOAD_LENGTH, len);
  
  // set FIFO pointers
  _mod->SPIsetRegValue(SX127X_REG_FIFO_TX_BASE_ADDR, SX127X_FIFO_TX_BASE_ADDR_MAX);
  _mod->SPIsetRegValue(SX127X_REG_FIFO_ADDR_PTR, SX127X_FIFO_TX_BASE_ADDR_MAX);
  
  // write packet to FIFO
  _mod->SPIwriteRegisterBurst(SX127X_REG_FIFO, data, len);
  
  // start transmission
  setMode(SX127X_TX);
  
  // wait for packet transmission or timeout
  uint32_t start = millis();
  while(!digitalRead(_mod->int0())) {
    if(millis() - start > timeout) {
      clearIRQFlags();
      return(ERR_TX_TIMEOUT);
    }
  }
  uint32_t elapsed = millis() - start;
  
  // update data rate
  dataRate = (len*8.0)/((float)elapsed/1000.0);
  
  // clear interrupt flags
  clearIRQFlags();
  
  return(ERR_NONE);
}

int16_t SX127x::receive(String& str, size_t len) {
  // create temporary array to store received data
  char* data = new char[len];
  int16_t state = SX127x::receive((uint8_t*)data, len);
  
  // if packet was received successfully, copy data into String
  if(state == ERR_NONE) {
    str = String(data);
  }
  
  delete[] data;
  return(state);
}

int16_t SX127x::receive(uint8_t* data, size_t len) {
  // set mode to standby
  setMode(SX127X_STANDBY);
  
  // set DIO pin mapping
  _mod->SPIsetRegValue(SX127X_REG_DIO_MAPPING_1, SX127X_DIO0_RX_DONE | SX127X_DIO1_RX_TIMEOUT, 7, 4);
  
  // clear interrupt flags
  clearIRQFlags();
  
  // set FIFO pointers
  _mod->SPIsetRegValue(SX127X_REG_FIFO_RX_BASE_ADDR, SX127X_FIFO_RX_BASE_ADDR_MAX);
  _mod->SPIsetRegValue(SX127X_REG_FIFO_ADDR_PTR, SX127X_FIFO_RX_BASE_ADDR_MAX);
  
  // set mode to receive
  setMode(SX127X_RXSINGLE);
  
  // wait for packet reception or timeout
  uint32_t start = millis();
  while(!digitalRead(_mod->int0())) {
    if(digitalRead(_mod->int1())) {
      clearIRQFlags();
      return(ERR_RX_TIMEOUT);
    }
  }
  
  // check integrity CRC
  if(_mod->SPIgetRegValue(SX127X_REG_IRQ_FLAGS, 5, 5) == SX127X_CLEAR_IRQ_FLAG_PAYLOAD_CRC_ERROR) {
    return(ERR_CRC_MISMATCH);
  }
  
  // get packet length
  size_t length = len;
  if(_sf != 6) {
    length = _mod->SPIgetRegValue(SX127X_REG_RX_NB_BYTES);
  }
  
  // read packet data
  if(len == 0) {
    // argument len equal to zero indicates String call, which means dynamically allocated data array
    // dispose of the original and create a new one
    delete[] data;
    data = new uint8_t[length];
  }
  _mod->SPIreadRegisterBurst(SX127X_REG_FIFO, length, data);
  
  // add terminating null
  if(len == 0) {
    data[length] = 0;
  }
  
  // update RSSI and SNR
  lastPacketRSSI = -157 + _mod->SPIgetRegValue(SX127X_REG_PKT_RSSI_VALUE);
  int8_t rawSNR = (int8_t)_mod->SPIgetRegValue(SX127X_REG_PKT_SNR_VALUE);
  lastPacketSNR = rawSNR / 4.0;
  
  // clear interrupt flags
  clearIRQFlags();
  
  return(ERR_NONE);
}

int16_t SX127x::scanChannel() {
  // set mode to standby
  setMode(SX127X_STANDBY);
  
  // set DIO pin mapping
  _mod->SPIsetRegValue(SX127X_REG_DIO_MAPPING_1, SX127X_DIO0_CAD_DONE | SX127X_DIO1_CAD_DETECTED, 7, 4);
  clearIRQFlags();
  
  // set mode to CAD
  setMode(SX127X_CAD);
  
  // wait for channel activity detected or timeout
  while(!digitalRead(_mod->int0())) {
    if(digitalRead(_mod->int1())) {
      clearIRQFlags();
      return(PREAMBLE_DETECTED);
    }
  }
  
  // clear interrupt flags
  clearIRQFlags();
  
  return(CHANNEL_FREE);
}

int16_t SX127x::sleep() {
  // set mode to sleep
  return(setMode(SX127X_SLEEP));
}

int16_t SX127x::standby() {
  // set mode to standby
  return(setMode(SX127X_STANDBY));
}

int16_t SX127x::listen() {
  // set mode to standby
  int16_t state = setMode(SX127X_STANDBY);
  
  // set DIO pin mapping
  state |= _mod->SPIsetRegValue(SX127X_REG_DIO_MAPPING_1, SX127X_DIO0_RX_DONE | SX127X_DIO1_RX_TIMEOUT, 7, 4);
  if(state != ERR_NONE) {
    return(state);
  }
  
  // clear interrupt flags
  clearIRQFlags();
  
  // set FIFO pointers
  state = _mod->SPIsetRegValue(SX127X_REG_FIFO_RX_BASE_ADDR, SX127X_FIFO_RX_BASE_ADDR_MAX);
  state |= _mod->SPIsetRegValue(SX127X_REG_FIFO_ADDR_PTR, SX127X_FIFO_RX_BASE_ADDR_MAX);
  if(state != ERR_NONE) {
    return(state);
  }
  
  // set mode to continuous reception
  return(setMode(SX127X_RXCONTINUOUS));
}

int16_t SX127x::setSyncWord(uint8_t syncWord) {
  // set mode to standby
  setMode(SX127X_STANDBY);
  
  // write register 
  return(_mod->SPIsetRegValue(SX127X_REG_SYNC_WORD, syncWord));
}

int16_t SX127x::setCurrentLimit(uint8_t currentLimit) {
  // check allowed range
  if(!(((currentLimit >= 45) && (currentLimit <= 240)) || (currentLimit == 0))) {
    return(ERR_INVALID_CURRENT_LIMIT);
  }
  
  // set mode to standby
  int16_t state = setMode(SX127X_STANDBY);
  
  // set OCP limit
  uint8_t raw;
  if(currentLimit == 0) {
    // limit set to 0, disable OCP
    state |= _mod->SPIsetRegValue(SX127X_REG_OCP, SX127X_OCP_OFF, 5, 5);
  } else if(currentLimit <= 120) {
    raw = (currentLimit - 45) / 5;
    state |= _mod->SPIsetRegValue(SX127X_REG_OCP, SX127X_OCP_ON | raw, 5, 0);
  } else if(currentLimit <= 240) {
    raw = (currentLimit + 30) / 10;
    state |= _mod->SPIsetRegValue(SX127X_REG_OCP, SX127X_OCP_ON | raw, 5, 0);
  }
  
  return(state);
}

int16_t SX127x::setPreambleLength(uint16_t preambleLength) {
  // check allowed range
  if(preambleLength < 6) {
    return(ERR_INVALID_PREAMBLE_LENGTH);
  }
  
  // set preamble length
  int16_t state = _mod->SPIsetRegValue(SX127X_REG_PREAMBLE_MSB, (preambleLength & 0xFF00) >> 8);
  state |= _mod->SPIsetRegValue(SX127X_REG_PREAMBLE_LSB, preambleLength & 0x00FF);
  return(state);
}

int16_t SX127x::setFrequencyRaw(float newFreq) {
  // set mode to standby
  setMode(SX127X_STANDBY);
  
  // calculate register values
  uint32_t base = 1;
  uint32_t FRF = (newFreq * (base << 19)) / 32.0;
  
  // write registers
  int16_t state = _mod->SPIsetRegValue(SX127X_REG_FRF_MSB, (FRF & 0xFF0000) >> 16);
  state |= _mod->SPIsetRegValue(SX127X_REG_FRF_MID, (FRF & 0x00FF00) >> 8);
  state |= _mod->SPIsetRegValue(SX127X_REG_FRF_LSB, FRF & 0x0000FF);
  
  return(state);
}

void SX127x::onReceive(void (*func)(void)) {
  attachInterrupt(digitalPinToInterrupt(_mod->int0()), func, RISING);
}

int16_t SX127x::readData(String& str, size_t len) {
  // create temporary array to store received data
  char* data = new char[len];
  int16_t state = SX127x::readData((uint8_t*)data, len);
  
  // if packet was received successfully, copy data into String
  if(state == ERR_NONE) {
    str = String(data);
  }
  
  delete[] data;
  return(state);
}

int16_t SX127x::readData(uint8_t* data, size_t len) {
  // check integrity CRC
  if(_mod->SPIgetRegValue(SX127X_REG_IRQ_FLAGS, 5, 5) == SX127X_CLEAR_IRQ_FLAG_PAYLOAD_CRC_ERROR) {
    return(ERR_CRC_MISMATCH);
  }
  
  // get packet length
  size_t length = len;
  if(_sf != 6) {
    length = _mod->SPIgetRegValue(SX127X_REG_RX_NB_BYTES);
  }
  
  // read packet data
  if(len == 0) {
    // argument len equal to zero indicates String call, which means dynamically allocated data array
    // dispose of the original and create a new one
    delete[] data;
    data = new uint8_t[length];
  }
  _mod->SPIreadRegisterBurst(SX127X_REG_FIFO, length, data);
  
  // add terminating null
  if(len == 0) {
    data[length] = 0;
  }
  
  // update RSSI and SNR
  lastPacketRSSI = -157 + _mod->SPIgetRegValue(SX127X_REG_PKT_RSSI_VALUE);
  int8_t rawSNR = (int8_t)_mod->SPIgetRegValue(SX127X_REG_PKT_SNR_VALUE);
  lastPacketSNR = rawSNR / 4.0;
  
  // clear interrupt flags
  clearIRQFlags();
  
  return(ERR_NONE);
}

int16_t SX127x::config() {
  // set mode to SLEEP
  int16_t state = setMode(SX127X_SLEEP);
  if(state != ERR_NONE) {
    return(state);
  }
  
  // set LoRa mode
  state = _mod->SPIsetRegValue(SX127X_REG_OP_MODE, SX127X_LORA, 7, 7);
  if(state != ERR_NONE) {
    return(state);
  }
  
  // set mode to STANDBY
  state = setMode(SX127X_STANDBY);
  if(state != ERR_NONE) {
    return(state);
  }
  
  // set LNA gain
  state |= _mod->SPIsetRegValue(SX127X_REG_LNA, SX127X_LNA_GAIN_1 | SX127X_LNA_BOOST_ON);
  if(state != ERR_NONE) {
    return(state);
  }
  
  // turn off frequency hopping
  state = _mod->SPIsetRegValue(SX127X_REG_HOP_PERIOD, SX127X_HOP_PERIOD_OFF);
  if(state != ERR_NONE) {
    return(state);
  }
  
  // set mode to STANDBY
  return(setMode(SX127X_STANDBY));
}

int16_t SX127x::setMode(uint8_t mode) {
  return(_mod->SPIsetRegValue(SX127X_REG_OP_MODE, mode, 2, 0));
}

void SX127x::clearIRQFlags() {
  _mod->SPIwriteRegister(SX127X_REG_IRQ_FLAGS, 0b11111111);
}
