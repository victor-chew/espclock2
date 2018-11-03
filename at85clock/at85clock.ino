/*
 * at85clock.ino
 *
 * Copyright 2018 Victor Chew
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <limits.h>
#include <avr/io.h>
#include <avr/wdt.h>
#include <avr/sleep.h>
#include <avr/interrupt.h>
#include <EEPROM.h>
#include <Wire.h>

#define TIMER0_PRESCALER 1024
#define TIMER1_PRESCALER 4096
#define OCR0A_DEFVAL ((byte)(F_CPU / (float)TIMER0_PRESCALER * 200/1000.0) - 1)
#define OCR1A_DEFVAL ((byte)(F_CPU / (float)TIMER1_PRESCALER * 1.0) - 1)

#define TIMER0_NOOP 0
#define TIMER0_INC_SEC 1
#define TIMER0_PULSE_SEC 2

#define HH 0
#define MM 1
#define SS 2

#define I2C_SLAVE_ADDR 0x12
#define CMD_START_CLOCK 0x2
#define CMD_STOP_CLOCK 0x4
#define CMD_SET_CLOCK 0x6
#define CMD_SET_NETTIME 0x8

bool hibernate = false;
byte ocr1a_val = OCR1A_DEFVAL;
byte adc_countdown = 5;

// Variables used by ISRs must be marked as volatile
volatile bool timer1 = false;
volatile bool clock_running = false;
volatile bool adc_ready = false;
volatile byte clocktime[3]; // physical clock time
volatile byte nettime[3]; // network clock time
volatile byte msgtime[3]; // time of most recent message
volatile byte timer0_op = TIMER0_NOOP;
volatile byte timer0_tickpin = PB3;
volatile byte msg[5], stats[30];
volatile int period = -1; // interval in secs between two CMD_SET_NETTIME
volatile int vcc = 3300;
volatile int stats_ptr = 0;

void incClockTime(volatile byte& hh, volatile byte& mm, volatile byte& ss) {
  if (++ss >= 60) {
    ss = 0;
    if (++mm >= 60) {
      mm = 0;
      if (++hh >= 12) {
        hh = 0;
      }
    }
  }
}

void startTimer0() {
  // Set prescaler to 1024, thereby starting Timer0
  TCCR0B = bit(CS02) | bit(CS00); 
}

void stopTimer0() {
  // Set prescaler to 0, thereby stopping Timer0
  TCCR0B = 0;
}

// Interrupt service routine for Timer0
// The effective outcome is to toggle tickpin high for 100ms, then rest for 200ms
ISR(TIMER0_COMPA_vect) {
  switch(timer0_op) {
    case TIMER0_INC_SEC:
      digitalWrite(timer0_tickpin, LOW);
      timer0_tickpin = (timer0_tickpin == PB3 ? PB4: PB3);
      stopTimer0();
      timer0_op = TIMER0_NOOP;
      break;

    case TIMER0_PULSE_SEC:
      digitalWrite(timer0_tickpin, LOW);
      stopTimer0();
      timer0_op = TIMER0_NOOP;
      break;
  }
}

// Interrupt service routine for Timer1
ISR(TIMER1_COMPA_vect) {
  if (!clock_running) return;
  incClockTime(nettime[HH], nettime[MM], nettime[SS]);
  if (period >= 0) period += 1;
  timer1 = true;
}

ISR(ADC_vect) {
    adc_ready = true;
}

void requestEvent() {
  memcpy(stats+23, clocktime, sizeof(clocktime));
  memcpy(stats+26, nettime, sizeof(nettime));
  Wire.write(stats + (stats_ptr * 15), 15);
  stats_ptr = (stats_ptr + 1) % 2;
}

void receiveEvent(uint8_t numbytes) {
  if (numbytes > sizeof(msg)) return;
  int idx = 0;
  while(idx < numbytes) {
    if (Wire.available()) msg[idx++] = Wire.read();
  }
  byte checksum = 0xcc;
  for (int i=0; i<numbytes-1; i++) checksum ^= msg[i];
  if (checksum == msg[numbytes-1]) {
    memcpy(msgtime, nettime, sizeof(msgtime));
    memcpy(stats+6, msgtime, sizeof(msgtime));
  } else {
    // Use 0xFF to show the last message has a checksum error
    // This will show up when master requests stats from the slave
    memset(msg, 0xff, sizeof(msg));
  }
  stats[29] = msg[0];
}  

// Move second hand one tick clockwise.
// See: http://www.cibomahto.com/2008/03/controlling-a-clock-with-an-arduino/
void incSecondHand() {
  incClockTime(clocktime[HH], clocktime[MM], clocktime[SS]);
  digitalWrite(timer0_tickpin, HIGH);
  timer0_op = TIMER0_INC_SEC;
  startTimer0();
}

// Pulse the same pin, which will cause the second hand to vibrate but not advance.
// This lets the user know the clock is waiting for network time to catch up.
void pulseSecondHand() {
  if (timer1) {
    digitalWrite(timer0_tickpin, HIGH);
    timer0_op = TIMER0_PULSE_SEC;
    startTimer0();
  }
}

// If clock time != network time, adjust until they match
void synchronizeClock() {
    // Calculate time difference between physical clock and network time in seconds
    long diff = ((clocktime[HH] * 3600L) + (clocktime[MM] * 60L) + clocktime[SS]) - ((nettime[HH] * 3600L) + (nettime[MM] * 60L) + nettime[SS]);
    if (diff == 0) return;
    if (diff < 0) diff = (12L*60*60) + diff;

    // If clock time is ahead about 5 mins, pulse second hand and wait for network time to catch up
    // Otherwise, fastforward clock and try to catch up
    if (diff <= 5*60) pulseSecondHand(); else incSecondHand();
}

// EEPROM: HH MM SS OCR1A TICKPIN CHECKSUM
// CHECKSUM = 0xCC ^ HH ^ MM ^ SS ^ OCR1A ^ TICKPIN
void readEEPROM() {
  byte checksum = 0xcc, buf[5];
  for (int i=0; i<sizeof(buf); i++) {
    buf[i] = stats[16+i] = EEPROM.read(i);
    checksum ^= buf[i];
  }
  byte checksum2 = stats[16+sizeof(buf)] = EEPROM.read(sizeof(buf));
  stats[16+sizeof(buf)+1] = checksum;

  // Good checksum; use values in EEPROM
  if (checksum == checksum2) {
    memcpy(clocktime, buf, sizeof(clocktime));
    memcpy(nettime, buf, sizeof(nettime));
    ocr1a_val = buf[3];
    timer0_tickpin = buf[4];
  } 
  // Bad checksum; use default values
  else {
    memset(clocktime, 0, sizeof(clocktime));
    memset(nettime, 0, sizeof(nettime));
    ocr1a_val = OCR1A_DEFVAL;
    timer0_tickpin = PB3;
  }
}

void writeEEPROM() {
  stats[16] = clocktime[HH];
  stats[17] = clocktime[MM];
  stats[18] = clocktime[SS];
  stats[19] = ocr1a_val;
  stats[20] = timer0_tickpin;
  stats[21] = 0xcc; for (int i=0; i<5; i++) stats[16+5] ^= stats[16+i];
  for (int i=0; i<6; i++) EEPROM.write(i, stats[i+16]);
}

void setup() {
  pinMode(PB1, INPUT_PULLUP); // Unused pin
  pinMode(PB3, OUTPUT); digitalWrite(PB3, LOW);
  pinMode(PB4, OUTPUT); digitalWrite(PB4, LOW);
  memset(msg, 0, sizeof(msg));
  memset(stats, 0, sizeof(stats));
  readEEPROM();
    
  // Reset prescalers for Timer0 and Timer1
  GTCCR |= bit(PSR0) | bit(PSR1);
  
  // Setup Timer0 (but don't run it yet)
  TCCR0A = 0;
  TCCR0B = 0;
  TCNT0  = 0;
  TCCR0A = bit(WGM01); // CTC mode
  OCR0A = OCR0A_DEFVAL;
  
  // Setup Timer1
  TCCR1 = 0;
  TCNT1 = 0;
  OCR1A = OCR1A_DEFVAL;
  OCR1C = OCR1A_DEFVAL;
  TCCR1 = bit(CTC1) | bit(CS13) | bit(CS12) | bit(CS10); // Start Timer1 in CTC mode; prescaler = 4096; 

  // Interrupt on compare match with OCR0A and OCR1A
  TIMSK |= bit(OCIE0A) | bit(OCIE1A);

  // Setup as I2C slave
  Wire.begin(I2C_SLAVE_ADDR);
  Wire.onReceive(receiveEvent);
  Wire.onRequest(requestEvent);
}

void loop() {
  if (timer1 && adc_countdown > 0) adc_countdown -= 1;
  
  // Measure VCC after every Timer1 interrupt
  // Source: http://digistump.com/wiki/digispark/quickref
  if (adc_ready) {
    adc_ready = false;
    vcc = 1125300L / ADC; // Calculate VCC (in mV); 1125300 = 1.1*1023*1000
    stats[14] = vcc;
    stats[15] = vcc >> 8;
    adc_countdown = 5;
  }
  else if (clock_running && !bit_is_set(ADCSRA, ADSC) && adc_countdown == 0) {
    ADMUX = bit(MUX3) | bit(MUX2); // Measure VCC using internal bandgap as reference
    ADCSRA = bit(ADEN) | bit(ADSC) | bit(ADIE) | bit(ADPS2) | bit(ADPS1) | bit(ADPS0); // Start conversion
  }
  
  // Start hibernation when VCC < 3V
  if (vcc < 2700) {
    if (!hibernate) {
      hibernate = true;
      clock_running = false;
      writeEEPROM();
      set_sleep_mode(SLEEP_MODE_PWR_DOWN);
      sleep_enable();
      sleep_cpu();
    }
  }
  // Normal routine when VCC >= 3V
  else {
    hibernate = false;
  
    // Process incoming message from i2c master (if any)
    noInterrupts();
    switch(msg[0]) {
      case CMD_START_CLOCK:
        clock_running = true;
        vcc = 3300;
        break;
      case CMD_STOP_CLOCK:
        clock_running = false;
        break;
      case CMD_SET_CLOCK:
        memcpy(clocktime, msg+1, sizeof(clocktime));
        memcpy(nettime, clocktime, sizeof(nettime));
        break;
      case CMD_SET_NETTIME:
        stats[9] = period; stats[10] = period >> 8;
        // Initialize period after first CMD_SET_NETTIME so that we can start tuning OCR1A
        if (period < 0) period = 0; 
        else if (period >= 60) {
          int8_t hh1 = (msgtime[HH] == 0 ? 12 : msgtime[HH]);
          int8_t hh2 = (msg[1] == 0 ? 12 : msg[1]);
          long curtime = ((hh1 * 3600L) + (msgtime[MM] * 60L) + msgtime[SS]);
          long newtime = ((hh2 * 3600L) + (msg[2] * 60L) + msg[3]);
          int8_t difft = (int8_t)(curtime - newtime);
          int8_t diffc = ((float)difft / period) / (1.0 / F_CPU * TIMER1_PRESCALER);
          int newc = ocr1a_val + diffc;
          if (newc >= 220 && newc <= 254) ocr1a_val = newc;
          OCR1A = ocr1a_val;
          OCR1C = ocr1a_val;
          stats[11] = difft; stats[12] = diffc; stats[13] = ocr1a_val;
          period = 0;
        }
        memcpy(stats, clocktime, sizeof(clocktime));
        memcpy(stats+3, nettime, sizeof(nettime));
        memcpy(nettime, msg+1, sizeof(nettime));
        break;
    }
    // Clear msg after processing
    memset(msg, 0, sizeof(msg));
    interrupts();

    // Synchronize physical clock with network clock
    if (clock_running && timer0_op == TIMER0_NOOP) synchronizeClock();
  }

  // Go back to sleep
  timer1 = false;
  set_sleep_mode(SLEEP_MODE_IDLE);
  sleep_enable();
  sleep_cpu();
}
