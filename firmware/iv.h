/***************************************************************************
 Ice Tube Clock firmware August 13, 2009
 (c) 2009 Limor Fried / Adafruit Industries

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
****************************************************************************/


#define halt(x)  while (1)

#define DEBUG 1
#define DEBUGP(x)  if (DEBUG) {putstring_nl(x);}


#define REGION_US 0
#define REGION_EU 1

// date format
#define DATE 0  // mm-dd-yy
#define DAY 1   // thur jan 1

#define DISPLAYSIZE 9

#define MAXSNOOZE 600 // 10 minutes
#define INACTIVITYTIMEOUT 10 // how many seconds we will wait before turning off menus


#define BEEP_8KHZ 5
#define BEEP_4KHZ 10
#define BEEP_2KHZ 20
#define BEEP_1KHZ 40

#define EE_YEAR 1
#define EE_MONTH 2
#define EE_DAY 3
#define EE_HOUR 4
#define EE_MIN 5
#define EE_SEC 6
#define EE_ALARM_HOUR 7 
#define EE_ALARM_MIN 8
#define EE_BRIGHT 9
#define EE_VOLUME 10
#define EE_REGION 11
#define EE_SNOOZE 12

void delay(uint16_t delay);

void (*app_start)(void) = 0x0000;

void clock_init(void);
void initbuttons(void);
void boost_init(uint8_t pwm);
void vfd_init(void);
void speaker_init(void);

void display_time(uint8_t h, uint8_t m, uint8_t s);
void display_date(uint8_t style);
void display_str(char *s);
void display_alarm(uint8_t h, uint8_t m);

void set_time(void);
void set_alarm(void);
void set_date(void);
void set_brightness(void);
void set_volume(void);
void set_region(void);
void set_snooze(void); // not activated by default

void beep(uint16_t freq, uint8_t times);
void tick(void);

uint8_t leapyear(uint16_t y);
void setalarmstate(void);

void setdisplay(uint8_t digit, uint8_t segments);
void vfd_send(uint32_t d);
void spi_xfer(uint8_t c);


// displaymode
#define NONE 99
#define SHOW_TIME 0
#define SHOW_DATE 1
#define SHOW_ALARM 2
#define SET_TIME 3
#define SET_ALARM 4
#define SET_DATE 5
#define SET_BRIGHTNESS 6
#define SET_VOLUME 7
#define SET_REGION 8
#define SHOW_SNOOZE 9
#define SET_SNOOZE 10

// sub-mode settings
#define SHOW_MENU 0
// alarm/time
#define SET_HOUR 1
#define SET_MIN 2
#define SET_SEC 3
// date
#define SET_MONTH 1
#define SET_DAY 2
#define SET_YEAR 3
//brightness
#define SET_BRITE 1
//volume
#define SET_VOL 1
//region
#define SET_REG 1

#define BOOST PD6
#define BOOST_DDR DDRD
#define BOOST_PORT PORTD

#define BUTTON1 PD5
#define BUTTON2 PB0
#define BUTTON3 PD4

#define VFDSWITCH PD3
#define VFDSWITCH_DDR DDRD
#define VFDSWITCH_PORT PORTD
#define VFDCLK PB5
#define VFDCLK_DDR DDRB
#define VFDCLK_PORT PORTB
#define VFDDATA PB3
#define VFDDATA_DDR DDRB
#define VFDDATA_PORT PORTB
#define VFDLOAD PC0
#define VFDLOAD_DDR DDRC
#define VFDLOAD_PORT PORTC
#define VFDBLANK PC3
#define VFDBLANK_DDR DDRC
#define VFDBLANK_PORT PORTC

#define ALARM PD2
#define ALARM_DDR DDRD
#define ALARM_PORT PORTD
#define ALARM_PIN PIND

#define SPK1 PB1
#define SPK2 PB2
#define SPK_PORT PORTB
#define SPK_DDR DDRB

#define SEG_A 19
#define SEG_B 17
#define SEG_C 14
#define SEG_D 13
#define SEG_E 15
#define SEG_F 18
#define SEG_G 16
#define SEG_H 11

#define DIG_1 4
#define DIG_2 12
#define DIG_3 5
#define DIG_4 10
#define DIG_5 6
#define DIG_6 9
#define DIG_7 8
#define DIG_8 7
#define DIG_9 3

#define nop asm("nop")
