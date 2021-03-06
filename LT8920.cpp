#include <SPI.h>
#include "LT8920.h"
#if __MBED__
SPI spi(PA_7, PA_6, PA_5);
#define transfer write
#else
SPIClass spi(2);
#endif

#define REGISTER_READ 0b10000000  //bin
#define REGISTER_WRITE 0b00000000 //bin
#define REGISTER_MASK 0b01111111  //bin

#define R_CHANNEL 7
#define CHANNEL_RX_BIT 7
#define CHANNEL_TX_BIT 8
#define CHANNEL_MASK 0b01111111 ///bin
#define DEFAULT_CHANNEL 0x31

#define R_CURRENT 9
#define CURRENT_POWER_SHIFT 12
#define CURRENT_POWER_MASK 0b1111000000000000
#define CURRENT_GAIN_SHIFT 7
#define CURRENT_GAIN_MASK 0b0000011110000000

#define R_DATARATE 44
#define DATARATE_MASK 0xFF00
#define DATARATE_1MBPS 0x0101
#define DATARATE_250KBPS 0x0401
#define DATARATE_125KBPS 0x0801
#define DATARATE_62KBPS 0x1001

#define R_SYNCWORD1 36
#define R_SYNCWORD2 37
#define R_SYNCWORD3 38
#define R_SYNCWORD4 39

#define R_PACKETCONFIG 41
#define PACKETCONFIG_CRC_ON 0x8000
#define PACKETCONFIG_SCRAMBLE_ON 0x4000
#define PACKETCONFIG_PACK_LEN_ENABLE 0x2000
#define PACKETCONFIG_FW_TERM_TX 0x1000
#define PACKETCONFIG_AUTO_ACK 0x0800
#define PACKETCONFIG_PKT_FIFO_POLARITY 0x0400

#define R_STATUS 48
#define STATUS_CRC_BIT 15

#define R_FIFO 50
#define R_FIFO_CONTROL 52
void default_rxcb();
void LT8920::dump_register(uint8_t reg)
{
  uint16_t r = readRegister(reg);
  printf("reg: 0x%x , value: 0x%x", reg, r);
}

LT8920::LT8920(const int cs, const int pkt, const int rst)
{
  index = 0;
  _channel = DEFAULT_CHANNEL;
  rxcb=default_rxcb;
#if __MBED__
  _pin_chipselect = (DigitalOut *)pinMode((PinName)cs, OUTPUT);
#ifdef LT8920_USE_INT
  _pin_pktflag = new InterruptIn((PinName)pkt);
#else
  _pin_pktflag = (DigitalIn *)pinMode((PinName)pkt, INPUT);
#endif
  _pin_reset = (DigitalOut *)pinMode((PinName)rst, OUTPUT);
#else
  _pin_chipselect = cs;
  _pin_pktflag = pkt;
  _pin_reset = rst;
  pinMode(_pin_chipselect, OUTPUT);
  pinMode(_pin_pktflag, INPUT);
  pinMode(_pin_reset, OUTPUT);
#ifdef LT8920_USE_INT
  attachInterrupt(_pin_pktflag, rxcb, FALLING);
#endif
#endif
  digitalWrite(_pin_chipselect, HIGH);
}

void LT8920::begin()
{
  if (_pin_reset > 0)
  {
    digitalWrite(_pin_reset, LOW);
    delay(200);
    digitalWrite(_pin_reset, HIGH);
    delay(200);
  }
//setup
#if __MBED__
  spi.format(8, 1);
  spi.frequency(1000000);
#else
  spi.begin();
  spi.setDataMode(1);
  spi.setBitOrder(MSBFIRST);
  spi.setClockDivider(SPI_CLOCK_DIV64);
#endif
  delay(500);
  id2 = readRegister(31);
  id1 = readRegister(30);
  writeRegister(0, 0x6fe0);
  writeRegister(1, 0x5681);
  writeRegister(2, 0x6617);
  writeRegister(4, 0x9cc9); //why does this differ from powerup (5447)
  writeRegister(5, 0x6637); //why does this differ from powerup (f000)
  writeRegister(8, 0x6c90); //power (default 71af) UNDOCUMENTED

  setCurrentControl(0x4, 0x0); // power & gain.

  writeRegister(10, 0x7ffd); //bit 0: XTAL OSC enable
  writeRegister(11, 0x0008); //bit 8: Power down RSSI (0=  RSSI operates normal)
  writeRegister(12, 0x0000);
  writeRegister(13, 0x48bd); //(default 4855)

  writeRegister(22, 0x00ff);
  writeRegister(23, 0x8005); //bit 2: Calibrate VCO before each Rx/Tx enable
  writeRegister(24, 0x0067);
  writeRegister(25, 0x1659);
  writeRegister(26, 0x19e0);
  writeRegister(27, 0x1300); //bits 5:0, Crystal Frequency adjust
  writeRegister(28, 0x1800);

  //fedcba9876543210
  writeRegister(32, 0x4808); //AAABBCCCDDEEFFFG  A preamble length, B, syncword length, c trailer length, d packet type
  //                  E FEC_type, F BRCLK_SEL, G reserved
  //0x5000 = 0101 0000 0000 0000 = preamble 010 (3 bytes), B 10 (48 bits)
  writeRegister(33, 0x3fc7);
  writeRegister(34, 0x2000); //
  writeRegister(35, 0x0300); //POWER mode,  bit 8/9 on = retransmit = 3x (default)
  setSyncWord(0x03805a5a03800380);

  writeRegister(40, 0x4402); //max allowed error bits = 0 (01 = 0 error bits)
  writeRegister(R_PACKETCONFIG,
                PACKETCONFIG_CRC_ON | PACKETCONFIG_AUTO_ACK | PACKETCONFIG_PKT_FIFO_POLARITY |
                    PACKETCONFIG_PACK_LEN_ENABLE);

  writeRegister(42, 0xfdb0);
  writeRegister(43, 0x000f);

  setDataRate(LT8920_62KBPS);

  writeRegister(R_FIFO, 0x0000); //TXRX_FIFO_REG (FIFO queue)

  writeRegister(R_FIFO_CONTROL, 0x8080); //Fifo Rx/Tx queue reset

  delay(200);
  writeRegister(R_CHANNEL, _BV(CHANNEL_TX_BIT)); //set TX mode.  (TX = bit 8, RX = bit 7, so RX would be 0x0080)
  delay(2);
  writeRegister(R_CHANNEL, _channel); // Frequency = 2402 + channel
  startListening();
}

void LT8920::setChannel(uint8_t channel)
{
  _channel = channel;
  writeRegister(R_CHANNEL, (_channel & CHANNEL_MASK));
}

uint8_t LT8920::getChannel()
{
  return _channel;
}

void LT8920::setCurrentControl(uint8_t power, uint8_t gain)
{
  writeRegister(R_CURRENT,
                ((power << CURRENT_POWER_SHIFT) & CURRENT_POWER_MASK) |
                    ((gain << CURRENT_GAIN_SHIFT) & CURRENT_GAIN_MASK));
}

bool LT8920::setDataRate(DataRate rate)
{
  uint16_t newValue;

  switch (rate)
  {
  case LT8920_1MBPS:
    newValue = DATARATE_1MBPS;
    break;
  case LT8920_250KBPS:
    newValue = DATARATE_250KBPS;
    break;
  case LT8920_125KBPS:
    newValue = DATARATE_125KBPS;
    break;
  case LT8920_62KBPS:
    newValue = DATARATE_62KBPS;
    break;
  default:
    return false;
  }

  writeRegister(R_DATARATE, newValue);
  return ((readRegister(R_DATARATE) & DATARATE_MASK) == (newValue & DATARATE_MASK));
}

LT8920::DataRate LT8920::getDataRate()
{
  uint16_t value = readRegister(R_DATARATE) & DATARATE_MASK;
  switch (value)
  {
  case DATARATE_1MBPS:
    return LT8920_1MBPS;
  case DATARATE_250KBPS:
    return LT8920_250KBPS;
  case DATARATE_125KBPS:
    return LT8920_125KBPS;
  case DATARATE_62KBPS:
    return LT8920_62KBPS;
  }
  return LT8920_62KBPS;
}

uint16_t LT8920::readRegister(uint8_t reg)
{
  digitalWrite(_pin_chipselect, LOW);
  spi.transfer(REGISTER_READ | (REGISTER_MASK & reg));
  uint8_t high = spi.transfer(0x00);
  uint8_t low = spi.transfer(0x00);

  digitalWrite(_pin_chipselect, HIGH);

  // Serial.print(reg);
  // Serial.print(" = ");
  // Serial.printf(high << 8 | low);

  return (high << 8 | low);
}

uint8_t LT8920::writeRegister(uint8_t reg, uint16_t data)
{
  uint8_t high = data >> 8;
  uint8_t low = data & 0xFF;

  return writeRegister2(reg, high, low);
}

uint8_t LT8920::writeRegister2(uint8_t reg, uint8_t high, uint8_t low)
{

  // char sbuf[32];
  // sprintf_P(sbuf, PSTR("%d => %02x%02x"), reg, high, low);
  // Serial.printf(sbuf);

  digitalWrite(_pin_chipselect, LOW);
  uint8_t result = spi.transfer(REGISTER_WRITE | (REGISTER_MASK & reg));
  spi.transfer(high);
  spi.transfer(low);

  digitalWrite(_pin_chipselect, HIGH);
  return result;
}

void LT8920::sleep()
{
  //set bit 14 on register 35.
  writeRegister(35, readRegister(35) | _BV(14));
}

void LT8920::whatsUp(Stream &stream)
{
#if !__MBED__
  uint16_t mode = readRegister(R_CHANNEL);
  stream.print("Tx_EN=");
  stream.print((mode & _BV(CHANNEL_TX_BIT)) != false);
  stream.print(" Rx_EN=");
  stream.print((mode & _BV(CHANNEL_RX_BIT)) != false);
  stream.print(" Channel=");
  stream.println(mode & CHANNEL_MASK);

  //read the status register.
  uint16_t state = readRegister(R_STATUS);

  bool crc_error = state & _BV(15);
  bool fec23_error = state & _BV(14);
  uint8_t framer_st = (state & 0b0011111100000000) >> 8;
  bool pkt_flag = state & _BV(6);
  bool fifo_flag = state & _BV(5);

  stream.print("CRC=");
  stream.print(crc_error);
  stream.print(" FEC=");
  stream.print(fec23_error);
  stream.print(" FRAMER_ST=");
  stream.print(framer_st);
  stream.print(" PKT=");
  stream.print(pkt_flag);
  stream.print(" FIFO=");
  stream.println(fifo_flag);

  uint16_t fifo = readRegister(R_FIFO_CONTROL);
  stream.print("FIFO_WR_PTR=");
  stream.print((fifo >> 8) & 0b111111);
  stream.print(" FIFO_RD_PTR=");
  stream.println(fifo & 0b111111);
#endif
}

bool LT8920::available()
{
  //read the PKT_FLAG state; this can also be done with a hard wire.
#if __MBED__
  if (*_pin_pktflag != 1)
#else
  if (digitalRead(_pin_pktflag) != 1)
#endif
  {
    return true;
  }

  return false;
}

int LT8920::read(uint8_t *buffer, size_t maxBuffer)
{
  uint16_t len;
  delay_us(1000);
  uint16_t state = readRegister(R_STATUS);
  if (bitRead(state, STATUS_CRC_BIT) == 0)
  {
    //CRC ok

    uint16_t data = readRegister(R_FIFO);
    uint8_t packetSize = data >> 8;
    if (maxBuffer < packetSize + 1)
    {
      //BUFFER TOO SMALL
      return -2;
    }

    uint8_t pos = 0;
    buffer[pos++] = (data & 0xFF);
    while (pos < packetSize)
    {
      data = readRegister(R_FIFO);
      buffer[pos++] = data >> 8;
      buffer[pos++] = data & 0xFF;
    }

    return packetSize;
  }
  else
  {
    //CRC error
    return -1;
  }
}

void LT8920::startListening()
{
  writeRegister(R_CHANNEL, _channel & CHANNEL_MASK); //turn off rx/tx
  // delay(3);
  writeRegister(R_FIFO_CONTROL, 0x0080);                                     //flush rx
  writeRegister(R_CHANNEL, (_channel & CHANNEL_MASK) | _BV(CHANNEL_RX_BIT)); //enable RX
  // delay(5);
}

/* set the BRCLK_SEL value */
void LT8920::setClock(uint8_t clock)
{
  //register 32, bits 3:1.
  uint16_t val = readRegister(35);
  val &= 0b1111111111110001;
  val |= ((clock & 0x07) << 1);
  ;

  writeRegister(35, val);
}

bool LT8920::sendPacket(uint8_t *data, size_t packetSize)
{
  if (packetSize < 1 || packetSize > 255)
  {
    return false;
  }
#if __MBED__
  _pin_pktflag->disable_irq();
#else
  detachInterrupt(_pin_pktflag);
#endif
  writeRegister(R_CHANNEL, _channel);
  writeRegister(R_FIFO_CONTROL, 0x8080); //flush tx

  //packets are sent in 16bit words, and the first word will be the packet size.
  //start spitting out words until we are done.

  uint8_t pos = 0;
  writeRegister2(R_FIFO, packetSize, data[pos++]);
  while (pos < packetSize)
  {
    uint8_t msb = data[pos++];
    uint8_t lsb = data[pos++];
    while (readRegister(R_STATUS) & _BV(5) == 0)
      ;
    writeRegister2(R_FIFO, msb, lsb);
  }

  writeRegister(R_CHANNEL, (_channel & CHANNEL_MASK) | _BV(CHANNEL_TX_BIT)); //enable TX

//Wait until the packet is sent.
#if __MBED__
  while (*_pin_pktflag == 1)
#else
  while (digitalRead(_pin_pktflag) == 1)
#endif
  {
    //do nothing.
  }
  writeRegister(R_CHANNEL, (_channel & CHANNEL_MASK));
  index++;
#if __MBED__
  _pin_pktflag->enable_irq();
#else
  attachInterrupt(_pin_pktflag, rxcb, FALLING);
#endif
  startListening();
  return true;
}

void LT8920::setSyncWord(uint64_t syncWord)
{
  writeRegister(R_SYNCWORD1, syncWord);
  writeRegister(R_SYNCWORD2, syncWord >> 16);
  writeRegister(R_SYNCWORD3, syncWord >> 32);
  writeRegister(R_SYNCWORD4, syncWord >> 48);
}

void LT8920::setSyncWordLength(uint8_t option)
{
  option &= 0x03;
  option <<= 11;

  writeRegister(32, (readRegister(32) & 0b0001100000000000) | option);
}

uint8_t LT8920::getRSSI()
{
  //RSSI: 15:10
  uint16_t value = readRegister(6);

  return (value >> 10);
}

void LT8920::scanRSSI(uint16_t *buffer, uint8_t start_channel, uint8_t num_channels)
{
  // writeRegister(R_CHANNEL, _BV(CHANNEL_RX_BIT));
  //
  // //add read mode.
  writeRegister(R_FIFO_CONTROL, 0x8080); //flush rx
  // writeRegister(R_CHANNEL, 0x0000);

  //set number of channels to scan.
  writeRegister(42, (readRegister(42) & 0b0000001111111111) | ((num_channels - 1 & 0b111111) << 10));

  //set channel scan offset.
  writeRegister(43, (readRegister(43) & 0b0000000011111111) | ((start_channel & 0b1111111) << 8));
  writeRegister(43, (readRegister(43) & 0b0111111111111111) | _BV(15));

  while (digitalRead(_pin_pktflag) == 0)
  {
  }

  //read the results.
  uint8_t pos = 0;
  while (pos < num_channels)
  {
    uint16_t data = readRegister(R_FIFO);
    buffer[pos++] = data >> 8;
  }
}

LT8920 lt8920(PB12, PC7, PC6);

  void default_rxcb()
  {
    lt8920.len = lt8920.read((uint8_t *)lt8920.buf, 64);
    lt8920.startListening();
  }