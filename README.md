# ESPCLOCK2
Network-enbling a cheap $2 Ikea analog clock using ESP8266 and ATtiny85.
### Preamble
This is version 2 of my [ESPClock](https://www.randseq.org/2016/10/hacking-analog-clock-to-sync-with-ntp.html) project.

The original project uses the ESP-12E development board to drive a cheap $2 Ikea analog clock. It gets the accurate clock time via NTP, and makes sure the physical clock time is up-to-date. It also automatically detects your current timezone via browser geolocation and deals with daylight saving adjustments with no user intervention.

The main issue with V1 of ESPCLOCK is obviously the huge current draw of the ESP8266, which makes it impractical to use it as a clock driver. The clock lasts only about a day on a 2000+ mAh USB  battery pack.

### Description
In V2 of the project, I am using the [WeMOS D1 Mini](https://wiki.wemos.cc/products:d1:d1_mini) development board, coupled with an [ATtiny85](http://www.atmel.com/devices/attiny85.aspx), which is a very small but power efficent microcontroller. 

The D1 Mini is a ESP8266 development board that is chosen for its reportedly low deep sleep current. It acts as the gateway to the Internet, but will be asleep most of the time to conserve energy.

The diminutive ATtiny85 is the "brain" for the clock. It communicates with the D1 Mini via I2C, and is responsible for adjusting the physical clock if conditions change. It also persists the physical clock time to the EEPROM if the battery voltage drops below a certain level. It does this by being connected to a 0.47F capacitor, which will provide sufficient power to the chip to do its thing when the battery source is depleted or removed.
### Breadboard layout
![ESPCLOCK2 Breadboard Layout](https://1.bp.blogspot.com/-3nCBVQUHuT8/W9vPFRnupZI/AAAAAAAAD0E/vNRnbg6_EngxKUC4YI0NOVqbp1MgFnL4gCLcBGAs/s1600/circuit05.png "ESPCLOCK2 Breadboard Layout")
![ESPCLOCK2 Breadboard](https://1.bp.blogspot.com/-SSWnsH9yHp8/W9vqorXpYUI/AAAAAAAAD0Q/2Kg-jegvrMk_72UMwDycoZ3JP-M1pPB0wCLcBGAs/s1600/IMG_1411.JPG "ESPCLOCK2 Breadboard")
### Lessons learnt:
In the original design, it was thought that the D1 Mini will have a current draw of only 170uA in deep sleep. In fact, this turns out to be 800uA when powered by the 5V pin.

It was thought since the ATtiny85 has a normal power consumption of 2.5mA and a sleep mode consumption of only 0.5¦ÌA, it could  theoretically last for about 200 days on a 2400mAh battery. It turns out we are unable to put the microcontroller to deep sleep because we need to keep the timers running (and also to make ADC measurements of the VCC pin).

The actual measured current consumption of the ATtiny85 is about 1.9mA juggling its various activities and trying to idle sleep when possible. This drastically reduces the lifespan on a 2400mAh battery to only about 50 days (still many times better than V1 though).

Soon after starting on the project, it was discovered that the original method for browser geolocation to identify user location (and hence timezone through Google's Timezone API) [does not work anymore](https://www.randseq.org/2018/08/geolocationgetcurrentposition-only.html) due to a browser policy change that restricts the use of said API to only HTTPS servers. So a new method needs to be found. 

In the end, it was decided to use the Timezone API found in more recent browsers to determine the user's timezone, then to call a simple script running on a server whose clock is NTP-synchronized to return the timezone-adjusted time. This is deemed more effective than having to deal with not only the geolocation API policy changes, but also to translate the location to timezone which will involve acquiring an API key from some third party. The PHP script itself is simple enough to embed in comments within `espclock2.ino`, and should be trivial to implement in other server-side script languages.

### Links:
* [Design](https://www.randseq.org/2017/02/espclock-v20-design.html)
