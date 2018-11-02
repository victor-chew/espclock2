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

#include <Time.h>
#include <TimeLib.h>

const int BATTERY_CAPS = 2300;
const byte INT_PIN = D1;
const float INT_TO_COULUMB = 0.614439;

bool trigger = false, init_done = false;
unsigned long total_time = 0;
volatile unsigned long num_interrupts = 0;
volatile unsigned long time1 = 0, time2 = 0;

void debug(const char *format, ...) {
  char buf[256];
  va_list ap;
  va_start(ap, format);
  vsnprintf(buf, sizeof(buf), format, ap);
  va_end(ap);
  Serial.println(buf);
}

void handleInterrupt() {
  if (time1 == 0) {
    time1 = millis();
    init_done = true;
  } else {
    num_interrupts++;
    time2 = millis();
    trigger = true;
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(INT_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(INT_PIN), handleInterrupt, FALLING);
} 
 
void loop() {
  if (init_done) {
    Serial.println();
    init_done = false;
  }

  if (trigger) {
    trigger = false;
    unsigned long interval = time2 - time1;
    total_time += interval;
    float ma = INT_TO_COULUMB / interval * 1000.0 * 1000.0;
    float ma_avg = (num_interrupts * INT_TO_COULUMB) / total_time * 1000.0 * 1000.0;
    float lifetime = BATTERY_CAPS / ma_avg / 24.0;
    debug("\ninterval = %ldms; num_interrupts = %ld; ma = %fmA; ma_avg = %fmA; lifetime = %f days",
      interval, num_interrupts, ma, ma_avg, lifetime);
    time1 = time2; 
  }
  else {
    Serial.print(time1 == 0 ? '#' : '.');
  }
  delay(10*1000);
}
