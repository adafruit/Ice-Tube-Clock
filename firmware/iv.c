/***************************************************************************
 Ice Tube Clock with GPS firmware July 22, 2010
 (c) 2010 Limor Fried / Adafruit Industries
 GPS Capability added by Devlin Thyne

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

#include <avr/io.h>      
#include <string.h>
#include <avr/interrupt.h>   // Interrupts and timers
#include <util/delay.h>      // Blocking delay functions
#include <avr/pgmspace.h>    // So we can store the 'font table' in ROM
#include <avr/eeprom.h>      // Date/time/pref backup in permanent EEPROM
#include <avr/wdt.h>     // Watchdog timer to repair lockups
#include <stdlib.h>

#include "iv.h"
#include "util.h"
#include "fonttable.h"

uint8_t region = REGION_US;

// These variables store the current time.
volatile int8_t time_s, time_m, time_h;
// ... and current date
volatile int8_t date_m, date_d, date_y;

// how loud is the speaker supposed to be?
volatile uint8_t volume;

// whether the alarm is on, going off, and alarm time
volatile uint8_t alarm_on, alarming, alarm_h, alarm_m;

// what is being displayed on the screen? (eg time, date, menu...)
volatile uint8_t displaymode;

// are we in low power sleep mode?
volatile uint8_t sleepmode = 0;

volatile uint8_t timeunknown = 0;        // MEME
volatile uint8_t restored = 0;

// String buffer for processing GPS data:
char strBuffer[BUFFERSIZE];
uint8_t intBufferStatus = 0;

// Variables for the timezone offset if using GPS.
int8_t intTimeZoneHour = -6;  //Because Colorado is my time zone...
uint8_t intTimeZoneMin = 0;


// Our display buffer, which is updated to show the time/date/etc
// and is multiplexed onto the tube
uint8_t display[DISPLAYSIZE]; // stores segments, not values!
uint8_t currdigit = 0;        // which digit we are currently multiplexing

// This table allow us to index between what digit we want to light up
// and what the pin number is on the MAX6921 see the .h for values.
// Stored in ROM (PROGMEM) to save RAM
const uint8_t digittable[] PROGMEM = {
  DIG_9, DIG_8, DIG_7, DIG_6, DIG_5, DIG_4, DIG_3, DIG_2, DIG_1
};
PGM_P digittable_p PROGMEM = digittable;

// This table allow us to index between what segment we want to light up
// and what the pin number is on the MAX6921 see the .h for values.
// Stored in ROM (PROGMEM) to save RAM
const uint8_t segmenttable[] PROGMEM = {
  SEG_H, SEG_G,  SEG_F,  SEG_E,  SEG_D,  SEG_C,  SEG_B,  SEG_A 
};
PGM_P segmenttable_p PROGMEM = segmenttable;

// muxdiv and MUX_DIVIDER divides down a high speed interrupt (31.25KHz)
// down so that we can refresh at about 100Hz (31.25KHz / 300)
// We refresh the entire display at 100Hz so each digit is updated
// 100Hz/DISPLAYSIZE
uint16_t muxdiv = 0;
#define MUX_DIVIDER (300 / DISPLAYSIZE)

// Likewise divides 100Hz down to 1Hz for the alarm beeping
uint16_t alarmdiv = 0;
#define ALARM_DIVIDER 100

// How long we have been snoozing
uint16_t snoozetimer = 0;

// We have a non-blocking delay function, milliseconds is updated by
// an interrupt
volatile uint16_t milliseconds = 0;
void delayms(uint16_t ms) {
  sei();

  milliseconds = 0;
  while (milliseconds < ms);
}

// When the alarm is going off, pressing a button turns on snooze mode
// this sets the snoozetimer off in MAXSNOOZE seconds - which turns on
// the alarm again
void setsnooze(void) {
  //snoozetimer = eeprom_read_byte((uint8_t *)EE_SNOOZE);
  //snoozetimer *= 60; // convert minutes to seconds
  snoozetimer = MAXSNOOZE;
  DEBUGP("snooze");
  display_str("snoozing");
  displaymode = SHOW_SNOOZE;
  delayms(1000);
  displaymode = SHOW_TIME;
}

// we reset the watchdog timer 
void kickthedog(void) {
  
  wdt_reset();

}

// called @ (F_CPU/256) = ~30khz (31.25 khz)
SIGNAL (SIG_OVERFLOW0) {
  // allow other interrupts to go off while we're doing display updates
  sei();

  // kick the dog
  kickthedog();

  // divide down to 100Hz * digits
  muxdiv++;
  if (muxdiv < MUX_DIVIDER)
    return;
  muxdiv = 0;
  // now at 100Hz * digits

  // ok its not really 1ms but its like within 10% :)
  milliseconds++;

  // Cycle through each digit in the display
  if (currdigit >= DISPLAYSIZE)
    currdigit = 0;

  // Set the current display's segments
  setdisplay(currdigit, display[currdigit]);
  // and go to the next
  currdigit++;

  // check if we should have the alarm on
  if (alarming && !snoozetimer) {
    alarmdiv++;
    if (alarmdiv > ALARM_DIVIDER) {
      alarmdiv = 0;
    } else {
      return;
    }
    // This part only gets reached at 1Hz

    // This sets the buzzer frequency
    ICR1 = 250;
    OCR1A = OCR1B = ICR1/2;

    // ok alarm is ringing!
    if (alarming & 0xF0) { // top bit indicates pulsing alarm state
      alarming &= ~0xF0;
      TCCR1B &= ~_BV(CS11); // turn buzzer off!
    } else {
      alarming |= 0xF0;
      TCCR1B |= _BV(CS11); // turn buzzer on!
    }
  }
  
}


// We use the pin change interrupts to detect when buttons are pressed

// These store the current button states for all 3 buttons. We can 
// then query whether the buttons are pressed and released or pressed
// This allows for 'high speed incrementing' when setting the time
volatile uint8_t last_buttonstate = 0, just_pressed = 0, pressed = 0;
volatile uint8_t buttonholdcounter = 0;

// This interrupt detects switches 1 and 3
SIGNAL(SIG_PIN_CHANGE2) {
  PCICR = 0;
  // allow interrupts while we're doing this
  sei();

  if (! (PIND & _BV(BUTTON1))) {
    // button1 is pressed
    if (! (last_buttonstate & 0x1)) { // was not pressed before
      delayms(10);                    // debounce
      if (PIND & _BV(BUTTON1)) {      // filter out bounce
	PCICR = _BV(PCIE0) | _BV(PCIE2);
	return;
      }
      tick();                         // make a noise
      // check if we will snag this button press for snoozing
      if (alarming) {
	// turn on snooze
	setsnooze();
	PCICR = _BV(PCIE0) | _BV(PCIE2);
	return;
      }
      last_buttonstate |= 0x1;
      just_pressed |= 0x1;
      DEBUGP("b1");
    }
  } else {
    last_buttonstate &= ~0x1;
  }

  if (! (PIND & _BV(BUTTON3))) {
    // button3 is pressed
    if (! (last_buttonstate & 0x4)) { // was not pressed before
      delayms(10);                    // debounce
      if (PIND & _BV(BUTTON3)) {      // filter out bounces
	PCICR = _BV(PCIE0) | _BV(PCIE2);
	return;
      }
      buttonholdcounter = 2;          // see if we're press-and-holding
      while (buttonholdcounter) {
	if (PIND & _BV(BUTTON3)) {        // released
	  tick();                         // make a noise
	  last_buttonstate &= ~0x4;
	  // check if we will snag this button press for snoozing
	  if (alarming) {
	    // turn on snooze
	    setsnooze();
	    PCICR = _BV(PCIE0) | _BV(PCIE2);
	    return;
	  }
	  DEBUGP("b3");
	  just_pressed |= 0x4;
	  PCICR = _BV(PCIE0) | _BV(PCIE2);
	  return;
	}
      }
      last_buttonstate |= 0x4;
      pressed |= 0x4;                 // held down
    }
  } else {
    pressed = 0;                      // button released
    last_buttonstate &= ~0x4;
  }
  PCICR = _BV(PCIE0) | _BV(PCIE2);
}

// Just button #2
SIGNAL(SIG_PIN_CHANGE0) {
  PCICR = 0;
  sei();
  if (! (PINB & _BV(BUTTON2))) {
    // button2 is pressed
    if (! (last_buttonstate & 0x2)) { // was not pressed before
      delayms(10);                    // debounce
      if (PINB & _BV(BUTTON2)) {      // filter out bounces
	PCICR = _BV(PCIE0) | _BV(PCIE2);
	return;
      }
      tick();                         // make a noise
      // check if we will snag this button press for snoozing
      if (alarming) {
	setsnooze(); 	// turn on snooze
	PCICR = _BV(PCIE0) | _BV(PCIE2);
	return;
      }
      last_buttonstate |= 0x2;
      just_pressed |= 0x2;
      DEBUGP("b2");
    }
  } else {
    last_buttonstate &= ~0x2;
  }
  PCICR = _BV(PCIE0) | _BV(PCIE2);
}

// This variable keeps track of whether we have not pressed any
// buttons in a few seconds, and turns off the menu display
volatile uint8_t timeoutcounter = 0;

// this goes off once a second
SIGNAL (TIMER2_OVF_vect) {
  CLKPR = _BV(CLKPCE);  //MEME
  CLKPR = 0;

  time_s++;             // one second has gone by

  fix_time();

  // If we're in low power mode we should get out now since the display is off
  if (sleepmode)
    return;
   

  if (displaymode == SHOW_TIME) {
    if (timeunknown && (time_s % 2)) {
      display_str("        ");
    } else {
      display_time(time_h, time_m, time_s);
    }
    if (alarm_on)
      display[0] |= 0x2;
    else 
      display[0] &= ~0x2;
    
  }

  check_alarm(time_h, time_m, time_s);

  if (timeoutcounter)
    timeoutcounter--;
  if (buttonholdcounter)
    buttonholdcounter--;
  if (snoozetimer) {
    snoozetimer--;
    if (snoozetimer % 2) 
      display[0] |= 0x2;
    else
      display[0] &= ~0x2;
  }
}

SIGNAL(SIG_INTERRUPT0) {
  uart_putchar('i');
  uint8_t x = ALARM_PIN & _BV(ALARM);
  sei();
  delayms(10); // wait for debouncing
  if (x != (ALARM_PIN & _BV(ALARM)))
    return;
  setalarmstate();
}



SIGNAL(SIG_COMPARATOR) {
  //DEBUGP("COMP");
  if (ACSR & _BV(ACO)) {
    //DEBUGP("HIGH");
    if (!sleepmode) {
      VFDSWITCH_PORT |= _BV(VFDSWITCH); // turn off display
      VFDCLK_PORT &= ~_BV(VFDCLK) & ~_BV(VFDDATA); // no power to vfdchip
      BOOST_PORT &= ~_BV(BOOST); // pull boost fet low
      SPCR  &= ~_BV(SPE); // turn off spi
      if (restored) {
	eeprom_write_byte((uint8_t *)EE_MIN, time_m);
	eeprom_write_byte((uint8_t *)EE_SEC, time_s);
      }
      DEBUGP("z");
      TCCR0B = 0; // no boost
      volume = 0; // low power buzzer
      PCICR = 0;  // ignore buttons

      app_start();
    }
  } else {
    //DEBUGP("LOW");
    if (sleepmode) {
      if (restored) {
	eeprom_write_byte((uint8_t *)EE_MIN, time_m);
	eeprom_write_byte((uint8_t *)EE_SEC, time_s);
      }
      DEBUGP("WAKERESET"); 
      app_start();
    }
  }
}


/*********************** Main app **********/

uint32_t t;

void gotosleep(void) {
  // battery
  //if (sleepmode) //already asleep?
  //  return;
  //DEBUGP("sleeptime");
  
  sleepmode = 1;
  VFDSWITCH_PORT |= _BV(VFDSWITCH); // turn off display
  SPCR  &= ~_BV(SPE); // turn off spi
  VFDCLK_PORT &= ~_BV(VFDCLK) & ~_BV(VFDDATA); // no power to vfdchip
  BOOST_PORT &= ~_BV(BOOST); // pull boost fet low
  TCCR0B = 0; // no boost
  volume = 0; // low power buzzer
  PCICR = 0;  // ignore buttons

  // sleep time!
  //beep(3520, 1);
  //beep(1760, 1);
  //beep(880, 1);
  // turn beeper off
  PORTB &= ~_BV(SPK1) & ~_BV(SPK2); 
  
  // turn off pullups
  PORTD &= ~_BV(BUTTON1) & ~_BV(BUTTON3);
  PORTB &= ~_BV(BUTTON2);
  DDRD &= ~_BV(BUTTON1) & ~_BV(BUTTON3);
  DDRB &= ~_BV(BUTTON2);
  ALARM_PORT &= ~_BV(ALARM);
  ALARM_DDR &= ~_BV(ALARM);
  

  // reduce the clock speed
  CLKPR = _BV(CLKPCE);
  CLKPR = _BV(CLKPS3);
  
  //  PPR |= _BV(PRUSART0) | _BV(PRADC) | _BV(PRSPI) | _BV(PRTIM1) | _BV(PRTIM0) | _BV(PRTWI);
  PORTC |= _BV(4);  // sleep signal
  SMCR |= _BV(SM1) | _BV(SM0) | _BV(SE); // sleep mode
  asm("sleep"); 
  CLKPR = _BV(CLKPCE);
  CLKPR = 0;
  PORTC &= ~_BV(4);
}
 
 void wakeup(void) {
   if (!sleepmode)
     return;
   CLKPR = _BV(CLKPCE);
   CLKPR = 0;
   DEBUGP("waketime");
   sleepmode = 0;
   // plugged in
   // wait to verify
   _delay_ms(20);
   if (ACSR & _BV(ACO)) 
     return;
   
   // turn on pullups
   initbuttons();

   // turn on boost
   boost_init(eeprom_read_byte((uint8_t *)EE_BRIGHT));

   // turn on vfd control
   vfd_init();

   // turn on display
   VFDSWITCH_PORT &= ~_BV(VFDSWITCH); 
   VFDBLANK_PORT &= ~_BV(VFDBLANK);
   volume = eeprom_read_byte((uint8_t *)EE_VOLUME); // reset
   
   speaker_init();

   kickthedog();

   setalarmstate();

   // wake up sound
   beep(880, 1);
   beep(1760, 1);
   beep(3520, 1);

   kickthedog();
 }


void initbuttons(void) {
    DDRB =  _BV(VFDCLK) | _BV(VFDDATA) | _BV(SPK1) | _BV(SPK2);
    DDRD = _BV(BOOST) | _BV(VFDSWITCH);
    DDRC = _BV(VFDLOAD) | _BV(VFDBLANK) | _BV(4);
    PORTD = _BV(BUTTON1) | _BV(BUTTON3) | _BV(ALARM);
    PORTB = _BV(BUTTON2);

    PCICR = _BV(PCIE0) | _BV(PCIE2);
    PCMSK0 = _BV(PCINT0);
    PCMSK2 = _BV(PCINT21) | _BV(PCINT20);    
}



int main(void) {
  //  uint8_t i;
  uint8_t mcustate;

  // turn boost off
  TCCR0B = 0;
  BOOST_DDR |= _BV(BOOST);
  BOOST_PORT &= ~_BV(BOOST); // pull boost fet low

  // check if we were reset
  mcustate = MCUSR;
  MCUSR = 0;

  uart_putw_hex(mcustate);

  wdt_disable();
  // now turn it back on... 2 second time out
  //WDTCSR |= _BV(WDP0) | _BV(WDP1) | _BV(WDP2);
  //WDTCSR = _BV(WDE);
  wdt_enable(WDTO_2S);
  kickthedog();

  // we lost power at some point so lets alert the user
  // that the time may be wrong (the clock still works)
  timeunknown = 1;

  // have we read the time & date from eeprom?
  restored = 0;

  // setup uart
  uart_init(BRRL_4800);

  //DEBUGP("VFD Clock");
  DEBUGP("!");
  uart_puts("\n\rHello World!\n\r");
  uart_puts("\n\rBuffer size is:\t");
  uart_putw_dec(BUFFERSIZE);
  uart_puts("\n\r");

  //DEBUGP("turning on anacomp");
  // set up analog comparator
  ACSR = _BV(ACBG) | _BV(ACIE); // use bandgap, intr. on toggle!
  _delay_ms(1);
  // settle!
  if (ACSR & _BV(ACO)) {
    // hmm we should not interrupt here
    ACSR |= _BV(ACI);

    // even in low power mode, we run the clock 
    DEBUGP("clock init");
    clock_init();  

  } else {
    // we aren't in low power mode so init stuff

    // init IOs
    initbuttons();
    
    VFDSWITCH_PORT &= ~_BV(VFDSWITCH);
    
    DEBUGP("turning on buttons");
    // set up button interrupts
    DEBUGP("turning on alarmsw");
    // set off an interrupt if alarm is set or unset
    EICRA = _BV(ISC00);
    EIMSK = _BV(INT0);
  
    displaymode = SHOW_TIME;
    DEBUGP("vfd init");
    vfd_init();
    
    DEBUGP("boost init");
    boost_init(eeprom_read_byte((uint8_t *)EE_BRIGHT));
    sei();

    //Load and check the timezone information
    intTimeZoneHour = eeprom_read_byte((uint8_t *)EE_ZONE_HOUR);
    if ( ( 12 < intTimeZoneHour ) || ( -12 > intTimeZoneHour ) )
      intTimeZoneHour = 0;

    intTimeZoneMin = eeprom_read_byte((uint8_t *)EE_ZONE_MIN);
    if ( ( 60 < intTimeZoneMin ) || ( 0 > intTimeZoneMin ) )
      intTimeZoneMin = 0;

    region = eeprom_read_byte((uint8_t *)EE_REGION);
    
    DEBUGP("speaker init");
    speaker_init();

    beep(4000, 1);

    DEBUGP("clock init");
    clock_init();  

    DEBUGP("alarm init");
    setalarmstate();
  }
  DEBUGP("done");
  while (1) {
    //_delay_ms(100);
    kickthedog();
    //uart_putc_hex(ACSR);
    if (ACSR & _BV(ACO)) {
      // DEBUGP("SLEEPYTIME");
      gotosleep();
      continue;
    }
    //DEBUGP(".");
    if (just_pressed & 0x1) {
      just_pressed = 0;
      switch(displaymode) {
      case (SHOW_TIME):
	displaymode = SET_ALARM;
	display_str("set alarm");
	set_alarm();
	break;
      case (SET_ALARM):
	displaymode = SET_TIME;
	display_str("set time");
	set_time();
	timeunknown = 0;
	break;
      case (SET_TIME):
	displaymode = SET_DATE;
	display_str("set date");
	set_date();
	break;
      case (SET_DATE):
	//displaymode = SET_BRIGHTNESS;
	//display_str("set brit");
	//set_brightness();
        displaymode = SET_ZONE;
        display_str("set zone");
        set_timezone();
	break;
      case (SET_ZONE):
        displaymode = SET_BRIGHTNESS;
        display_str("set brit");
        set_brightness();
        break;
      case (SET_BRIGHTNESS):
	displaymode = SET_VOLUME;
	display_str("set vol ");
	set_volume();
	break;
      case (SET_VOLUME):
	displaymode = SET_REGION;
	display_str("set regn");
	set_region();
	break;
	/*
      case (SET_REGION):
	displaymode = SET_SNOOZE;
	display_str("set snoz");
	set_snooze();
	break;
	*/
      default:
	displaymode = SHOW_TIME;
      }
    } else if ((just_pressed & 0x2) || (just_pressed & 0x4)) {
      just_pressed = 0;
      displaymode = NONE;
      display_date(DAY);

      kickthedog();
      delayms(1500);
      kickthedog();

      displaymode = SHOW_TIME;     
    }

    //Check to see if GPS data is ready:
    if ( gpsdataready() ) {
       getgpstime();

    }
 
  }
}

/**************************** SUB-MENUS *****************************/

void set_alarm(void) 
{
  uint8_t mode;
  uint8_t hour, min, sec;
    
  hour = min = sec = 0;
  mode = SHOW_MENU;

  hour = alarm_h;
  min = alarm_m;
  sec = 0;
  
  timeoutcounter = INACTIVITYTIMEOUT;
  
  while (1) {
    if (just_pressed & 0x1) { // mode change
      return;
    }
    if (just_pressed || pressed) {
      timeoutcounter = INACTIVITYTIMEOUT;  
      // timeout w/no buttons pressed after 3 seconds?
    } else if (!timeoutcounter) {
      //timed out!
      displaymode = SHOW_TIME;     
      alarm_h = hour;
      alarm_m = min;
      eeprom_write_byte((uint8_t *)EE_ALARM_HOUR, alarm_h);    
      eeprom_write_byte((uint8_t *)EE_ALARM_MIN, alarm_m);    
      return;
    }
    if (just_pressed & 0x2) {
      just_pressed = 0;
      if (mode == SHOW_MENU) {
	// ok now its selected
	mode = SET_HOUR;
	display_alarm(hour, min);
	display[1] |= 0x1;
	display[2] |= 0x1;	
      } else if (mode == SET_HOUR) {
	mode = SET_MIN;
	display_alarm(hour, min);
	display[4] |= 0x1;
	display[5] |= 0x1;
      } else {
	// done!
	alarm_h = hour;
	alarm_m = min;
	eeprom_write_byte((uint8_t *)EE_ALARM_HOUR, alarm_h);    
	eeprom_write_byte((uint8_t *)EE_ALARM_MIN, alarm_m);    
	displaymode = SHOW_TIME;
	return;
      }
    }
    if ((just_pressed & 0x4) || (pressed & 0x4)) {
      just_pressed = 0;

      if (mode == SET_HOUR) {
	hour = (hour+1) % 24;
	display_alarm(hour, min);
	display[1] |= 0x1;
	display[2] |= 0x1;
      }
      if (mode == SET_MIN) {
	min = (min+1) % 60;
	display_alarm(hour, min);
	display[4] |= 0x1;
	display[5] |= 0x1;
      }

      if (pressed & 0x4)
	delayms(75);
    }
  }
}

void set_time(void) 
{
  uint8_t mode;
  uint8_t hour, min, sec;
    
  hour = time_h;
  min = time_m;
  sec = time_s;
  mode = SHOW_MENU;

  timeoutcounter = INACTIVITYTIMEOUT;
  
  while (1) {
    if (just_pressed & 0x1) { // mode change
      return;
    }
    if (just_pressed || pressed) {
      timeoutcounter = INACTIVITYTIMEOUT;  
      // timeout w/no buttons pressed after 3 seconds?
    } else if (!timeoutcounter) {
      //timed out!
      displaymode = SHOW_TIME;     
      return;
    }
    if (just_pressed & 0x2) {
      just_pressed = 0;
      if (mode == SHOW_MENU) {
	hour = time_h;
	min = time_m;
	sec = time_s;

	// ok now its selected
	mode = SET_HOUR;
	display_time(hour, min, sec);
	display[1] |= 0x1;
	display[2] |= 0x1;	
      } else if (mode == SET_HOUR) {
	mode = SET_MIN;
	display_time(hour, min, sec);
	display[4] |= 0x1;
	display[5] |= 0x1;
      } else if (mode == SET_MIN) {
	mode = SET_SEC;
	display_time(hour, min, sec);
	display[7] |= 0x1;
	display[8] |= 0x1;
      } else {
	// done!
	time_h = hour;
	time_m = min;
	time_s = sec;
	displaymode = SHOW_TIME;
	return;
      }
    }
    if ((just_pressed & 0x4) || (pressed & 0x4)) {
      just_pressed = 0;
      
      if (mode == SET_HOUR) {
	hour = (hour+1) % 24;
	display_time(hour, min, sec);
	display[1] |= 0x1;
	display[2] |= 0x1;
	time_h = hour;
	eeprom_write_byte((uint8_t *)EE_HOUR, time_h);    
      }
      if (mode == SET_MIN) {
	min = (min+1) % 60;
	display_time(hour, min, sec);
	display[4] |= 0x1;
	display[5] |= 0x1;
	eeprom_write_byte((uint8_t *)EE_MIN, time_m);
	time_m = min;
      }
      if ((mode == SET_SEC) ) {
	sec = (sec+1) % 60;
	display_time(hour, min, sec);
	display[7] |= 0x1;
	display[8] |= 0x1;
	time_s = sec;
      }
      
      if (pressed & 0x4)
	delayms(75);
    }
  }
}



void set_date(void) {
  uint8_t mode = SHOW_MENU;

  timeoutcounter = INACTIVITYTIMEOUT;;  

  while (1) {
    if (just_pressed || pressed) {
      timeoutcounter = INACTIVITYTIMEOUT;;  
      // timeout w/no buttons pressed after 3 seconds?
    } else if (!timeoutcounter) {
      //timed out!
      displaymode = SHOW_TIME;     
      return;
    }
    if (just_pressed & 0x1) { // mode change
      return;
    }
    if (just_pressed & 0x2) {

      just_pressed = 0;
      if (mode == SHOW_MENU) {
	// start!
	if (region == REGION_US) {
	  mode = SET_MONTH;
	}
	else {
	  DEBUGP("Set day");
	  mode = SET_DAY;
	}
	display_date(DATE);
	display[1] |= 0x1;
	display[2] |= 0x1;
      } else if (((mode == SET_MONTH) && (region == REGION_US)) ||
		 ((mode == SET_DAY) && (region == REGION_EU))) {
	if (region == REGION_US)
	  mode = SET_DAY;
	else
	  mode = SET_MONTH;
	display_date(DATE);
	display[4] |= 0x1;
	display[5] |= 0x1;
      } else if (((mode == SET_DAY) && (region == REGION_US)) ||
	((mode == SET_MONTH) && (region == REGION_EU))) {
	mode = SET_YEAR;
	display_date(DATE);
	display[7] |= 0x1;
	display[8] |= 0x1;
      } else {
	displaymode = NONE;
	display_date(DATE);
	delayms(1500);
	displaymode = SHOW_TIME;
	return;
      }
    }
    if ((just_pressed & 0x4) || (pressed & 0x4)) {
      just_pressed = 0;
      if (mode == SET_MONTH) {
	date_m++;
	if (date_m >= 13)
	  date_m = 1;
	display_date(DATE);
	if (region == REGION_US) {
	  display[1] |= 0x1;
	  display[2] |= 0x1;
	} else {
	  display[4] |= 0x1;
	  display[5] |= 0x1;
	}
	eeprom_write_byte((uint8_t *)EE_MONTH, date_m);    
      }
      if (mode == SET_DAY) {
	date_d++;
	if (date_d > 31)
	  date_d = 1;
	display_date(DATE);

	if (region == REGION_EU) {
	  display[1] |= 0x1;
	  display[2] |= 0x1;
	} else {
	  display[4] |= 0x1;
	  display[5] |= 0x1;
	}
	eeprom_write_byte((uint8_t *)EE_DAY, date_d);    
      }
      if (mode == SET_YEAR) {
	date_y++;
	date_y %= 100;
	display_date(DATE);
	display[7] |= 0x1;
	display[8] |= 0x1;
	eeprom_write_byte((uint8_t *)EE_YEAR, date_y);    
      }

      if (pressed & 0x4)
	delayms(60);
    }
  }
}

//Function to set the time zone
void set_timezone(void) {
  int8_t hour = intTimeZoneHour;
  uint8_t min = intTimeZoneMin;
  uint8_t mode = SHOW_MENU;
  timeoutcounter = INACTIVITYTIMEOUT;

  while (1) {
    if (just_pressed & 0x1) { // mode change
      return;
    }
    if (just_pressed || pressed) {
      timeoutcounter = INACTIVITYTIMEOUT;  
      // timeout w/no buttons pressed after 3 seconds?
    } else if (!timeoutcounter) {
      //timed out!
      displaymode = SHOW_TIME;     
      return;
    }
    if (just_pressed & 0x2) {
      just_pressed = 0;
      if (mode == SHOW_MENU) {
	// ok now its selected
	mode = SET_HOUR;
	display_timezone(hour, min);
	display[1] |= 0x1;
	display[2] |= 0x1;	
      } else if (mode == SET_HOUR) {
	mode = SET_MIN;
	display_timezone(hour, min);
	display[4] |= 0x1;
	display[5] |= 0x1;
      } else {
	// done!
	displaymode = SHOW_TIME;
	return;
      }
    }
    if ((just_pressed & 0x4) || (pressed & 0x4)) {
      just_pressed = 0;
      
      if (mode == SET_HOUR) {
	hour = ( ( hour + 1 + 12 ) % 25 ) - 12;
	display_timezone(hour, min);
	display[1] |= 0x1;
	display[2] |= 0x1;
        intTimeZoneHour = hour;
	eeprom_write_byte((uint8_t *)EE_ZONE_HOUR, hour);
	//Debugging:
	uart_puts("\n\rTimezone offset hour:\t");
	uart_putw_dec(hour);
      }
      if (mode == SET_MIN) {
	min = ( min + 1 ) % 60;
	display_timezone(hour, min);
	display[4] |= 0x1;
	display[5] |= 0x1;
        intTimeZoneMin = min;
	eeprom_write_byte((uint8_t *)EE_ZONE_MIN, min);
      }
      
      if (pressed & 0x4)
	delayms(75);
    }
  }
}

void set_brightness(void) {
  uint8_t mode = SHOW_MENU;
  uint8_t brightness;

  timeoutcounter = INACTIVITYTIMEOUT;;  
  brightness = eeprom_read_byte((uint8_t *)EE_BRIGHT);

  while (1) {
    if (just_pressed || pressed) {
      timeoutcounter = INACTIVITYTIMEOUT;;  
      // timeout w/no buttons pressed after 3 seconds?
    } else if (!timeoutcounter) {
      //timed out!
      displaymode = SHOW_TIME;     
      eeprom_write_byte((uint8_t *)EE_BRIGHT, brightness);
      return;
    }
    if (just_pressed & 0x1) { // mode change
      return;
    }
    if (just_pressed & 0x2) {

      just_pressed = 0;
      if (mode == SHOW_MENU) {
	// start!
	mode = SET_BRITE;
	// display brightness
	display_str("brite ");
	display[7] = pgm_read_byte(numbertable_p + (brightness / 10)) | 0x1;
	display[8] = pgm_read_byte(numbertable_p + (brightness % 10)) | 0x1;
      } else {	
	displaymode = SHOW_TIME;
	eeprom_write_byte((uint8_t *)EE_BRIGHT, brightness);
	return;
      }
    }
    if ((just_pressed & 0x4) || (pressed & 0x4)) {
      just_pressed = 0;
      if (mode == SET_BRITE) {
	brightness += 5;
	if (brightness > 91)
	  brightness = 30;
	display[7] = pgm_read_byte(numbertable_p + (brightness / 10)) | 0x1;
	display[8] = pgm_read_byte(numbertable_p + (brightness % 10)) | 0x1;
	if (brightness <= 30) {
	  OCR0A = 30; 
	} else if (brightness <= 35) {
	  OCR0A = 35;
	} else if (brightness <= 40) {
	  OCR0A = 40;
	} else if (brightness <= 45) {
	  OCR0A = 45;
	} else if (brightness <= 50) {
	  OCR0A = 50;
	} else if (brightness <= 55) {
	  OCR0A = 55;
	} else if (brightness <= 60) {
	  OCR0A = 60;
	} else if (brightness <= 65) {
	  OCR0A = 65;
	} else if (brightness <= 70) {
	  OCR0A = 70;
	} else if (brightness <= 75) {
	  OCR0A = 75;
	} else if (brightness <= 80) {
	  OCR0A = 80;
	} else if (brightness <= 85) {
	  OCR0A = 85;
	} else if (brightness <= 90) {
	  OCR0A = 90;
	} else {
	  OCR0A = 30;
	}
      }
    }
  }
}


void set_volume(void) {
  uint8_t mode = SHOW_MENU;
  uint8_t volume;

  timeoutcounter = INACTIVITYTIMEOUT;;  
  volume = eeprom_read_byte((uint8_t *)EE_VOLUME);

  while (1) {
    if (just_pressed || pressed) {
      timeoutcounter = INACTIVITYTIMEOUT;;  
      // timeout w/no buttons pressed after 3 seconds?
    } else if (!timeoutcounter) {
      //timed out!
      displaymode = SHOW_TIME;     
      return;
    }
    if (just_pressed & 0x1) { // mode change
      return;
    }
    if (just_pressed & 0x2) {
      just_pressed = 0;
      if (mode == SHOW_MENU) {
	// start!
	mode = SET_VOL;
	// display volume
	if (volume) {
	  display_str("vol high");
	  display[5] |= 0x1;
	} else {
	  display_str("vol  low");
	}
	display[6] |= 0x1;
	display[7] |= 0x1;
	display[8] |= 0x1;
      } else {	
	displaymode = SHOW_TIME;
	return;
      }
    }
    if (just_pressed & 0x4) {
      just_pressed = 0;
      if (mode == SET_VOL) {
	volume = !volume;
	if (volume) {
	  display_str("vol high");
	  display[5] |= 0x1;
	} else {
	  display_str("vol  low");
	}
	display[6] |= 0x1;
	display[7] |= 0x1;
	display[8] |= 0x1;
	eeprom_write_byte((uint8_t *)EE_VOLUME, volume);
	speaker_init();
	beep(4000, 1);
      }
    }
  }
}




void set_region(void) {
  uint8_t mode = SHOW_MENU;

  timeoutcounter = INACTIVITYTIMEOUT;;  
  region = eeprom_read_byte((uint8_t *)EE_REGION);

  while (1) {
    if (just_pressed || pressed) {
      timeoutcounter = INACTIVITYTIMEOUT;;  
      // timeout w/no buttons pressed after 3 seconds?
    } else if (!timeoutcounter) {
      //timed out!
      displaymode = SHOW_TIME;     
      return;
    }
    if (just_pressed & 0x1) { // mode change
      return;
    }
    if (just_pressed & 0x2) {
      just_pressed = 0;
      if (mode == SHOW_MENU) {
	// start!
	mode = SET_REG;
	// display region
	if (region == REGION_US) {
	  display_str("usa-12hr");
	} else {
	  display_str("eur-24hr");
	}
      } else {	
	displaymode = SHOW_TIME;
	return;
      }
    }
    if (just_pressed & 0x4) {
      just_pressed = 0;
      if (mode == SET_REG) {
	region = !region;
	if (region == REGION_US) {
	  display_str("usa-12hr");
	} else {
	  display_str("eur-24hr");
	}
	eeprom_write_byte((uint8_t *)EE_REGION, region);
      }
    }
  }
}


/*
void set_snooze(void) {
  uint8_t mode = SHOW_MENU;
  uint8_t snooze;

  timeoutcounter = INACTIVITYTIMEOUT;;  
  snooze = eeprom_read_byte((uint8_t *)EE_SNOOZE);

  while (1) {
    if (just_pressed || pressed) {
      timeoutcounter = INACTIVITYTIMEOUT;;  
      // timeout w/no buttons pressed after 3 seconds?
    } else if (!timeoutcounter) {
      //timed out!
      displaymode = SHOW_TIME;     
      return;
    }
    if (just_pressed & 0x1) { // mode change
      return;
    }
    if (just_pressed & 0x2) {

      just_pressed = 0;
      if (mode == SHOW_MENU) {
	// start!
	mode = SET_SNOOZE;
	// display snooze
	display_str("   minut");
	display[1] = pgm_read_byte(numbertable_p + (snooze / 10)) | 0x1;
	display[2] = pgm_read_byte(numbertable_p + (snooze % 10)) | 0x1;
      } else { 
	displaymode = SHOW_TIME;
	return;
      }
    }
    if ((just_pressed & 0x4) || (pressed & 0x4)) {
      just_pressed = 0;
      if (mode == SET_SNOOZE) {
        snooze ++;
	if (snooze >= 100)
	  snooze = 0;
	display[1] = pgm_read_byte(numbertable_p + (snooze / 10)) | 0x1;
	display[2] = pgm_read_byte(numbertable_p + (snooze % 10)) | 0x1;
	eeprom_write_byte((uint8_t *)EE_SNOOZE, snooze);
      }

      if (pressed & 0x4)
	delayms(75);

    }
  }
}
*/


/**************************** RTC & ALARM *****************************/
void clock_init(void) {
  // we store the time in EEPROM when switching from power modes so its
  // reasonable to start with whats in memory
  time_h = eeprom_read_byte((uint8_t *)EE_HOUR) % 24;
  time_m = eeprom_read_byte((uint8_t *)EE_MIN) % 60;
  time_s = eeprom_read_byte((uint8_t *)EE_SEC) % 60;

  /*
    // if you're debugging, having the makefile set the right
    // time automatically will be very handy. Otherwise don't use this
  time_h = TIMEHOUR;
  time_m = TIMEMIN;
  time_s = TIMESEC + 10;
  */

  // Set up the stored alarm time and date
  alarm_m = eeprom_read_byte((uint8_t *)EE_ALARM_MIN) % 60;
  alarm_h = eeprom_read_byte((uint8_t *)EE_ALARM_HOUR) % 24;

  date_y = eeprom_read_byte((uint8_t *)EE_YEAR) % 100;
  date_m = eeprom_read_byte((uint8_t *)EE_MONTH) % 13;
  date_d = eeprom_read_byte((uint8_t *)EE_DAY) % 32;

  restored = 1;

  // Turn on the RTC by selecting the external 32khz crystal
  // 32.768 / 128 = 256 which is exactly an 8-bit timer overflow
  ASSR |= _BV(AS2); // use crystal
  TCCR2B = _BV(CS22) | _BV(CS20); // div by 128
  // We will overflow once a second, and call an interrupt

  // enable interrupt
  TIMSK2 = _BV(TOIE2);

  // enable all interrupts!
  sei();
}

// This turns on/off the alarm when the switch has been
// set. It also displays the alarm time
void setalarmstate(void) {
  if (ALARM_PIN & _BV(ALARM)) { 
    // Don't display the alarm/beep if we already have
    if  (!alarm_on) {
      // alarm on!
      alarm_on = 1;
      // reset snoozing
      snoozetimer = 0;
      // show the status on the VFD tube
      display_str("alarm on");
      // its not actually SHOW_SNOOZE but just anything but SHOW_TIME
      displaymode = SHOW_SNOOZE;
      delayms(1000);
      // show the current alarm time set
      display_alarm(alarm_h, alarm_m);
      delayms(1000);
      // after a second, go back to clock mode
      displaymode = SHOW_TIME;
    }
  } else {
    if (alarm_on) {
      // turn off the alarm
      alarm_on = 0;
      snoozetimer = 0;
      if (alarming) {
	// if the alarm is going off, we should turn it off
	// and quiet the speaker
	DEBUGP("alarm off");
	alarming = 0;
	TCCR1B &= ~_BV(CS11); // turn it off!
	PORTB |= _BV(SPK1) | _BV(SPK2);
      } 
    }
  }
}

// This will calculate leapyears, give it the year
// and it will return 1 (true) or 0 (false)
uint8_t leapyear(uint16_t y) {
  return ( (!(y % 4) && (y % 100)) || !(y % 400));
}


/**************************** SPEAKER *****************************/
// Set up the speaker to prepare for beeping!
void speaker_init(void) {

  // read the preferences for high/low volume
  volume = eeprom_read_byte((uint8_t *)EE_VOLUME);

  // We use the built-in fast PWM, 8 bit timer
  PORTB |= _BV(SPK1) | _BV(SPK2); 

  // Turn on PWM outputs for both pins
  TCCR1A = _BV(COM1B1) | _BV(COM1B0) | _BV(WGM11);
  if (volume) {
    TCCR1A |= _BV(COM1A1);
  } 
  TCCR1B = _BV(WGM13) | _BV(WGM12);

  // start at 4khz:  250 * 8 multiplier * 4000 = 8mhz
  ICR1 = 250;
  OCR1B = OCR1A = ICR1 / 2;
}

// This makes the speaker tick, it doesnt use PWM
// instead it just flicks the piezo
void tick(void) {
  TCCR1A = 0;
  TCCR1B = 0;

  // Send a pulse thru both pins, alternating
  SPK_PORT |= _BV(SPK1);
  SPK_PORT &= ~_BV(SPK2);
  delayms(10);
  SPK_PORT |= _BV(SPK2);
  SPK_PORT &= ~_BV(SPK1);
  delayms(10);
  // turn them both off
  SPK_PORT &= ~_BV(SPK1) & ~_BV(SPK2);

  TCCR1A = _BV(COM1A1) | _BV(COM1B1) | _BV(COM1B0) | _BV(WGM11);
  TCCR1B = _BV(WGM13) | _BV(WGM12);
}

// We can play short beeps!
void beep(uint16_t freq, uint8_t times) {
  // set the PWM output to match the desired frequency
  ICR1 = (F_CPU/8)/freq;
  // we want 50% duty cycle square wave
  OCR1A = OCR1B = ICR1/2;
   
  while (times--) {
    TCCR1B |= _BV(CS11); // turn it on!
    // beeps are 200ms long on
    _delay_ms(200);
    TCCR1B &= ~_BV(CS11); // turn it off!
    PORTB &= ~_BV(SPK1) & ~_BV(SPK2);
    // beeps are 200ms long off
    _delay_ms(200);
  }
  // turn speaker off
  PORTB &= ~_BV(SPK1) & ~_BV(SPK2);
}



/**************************** BOOST *****************************/

// We control the boost converter by changing the PWM output
// pins
void boost_init(uint8_t brightness) {
  // Set PWM value, don't set it so high that
  // we could damage the MAX chip or display
  if (brightness > 90)
    brightness = 90;

  // Or so low its not visible
  if (brightness < 30)
    brightness = 30;

  if (brightness <= 30) {
    OCR0A = 30; 
  } else if (brightness <= 35) {
    OCR0A = 35;
  } else if (brightness <= 40) {
    OCR0A = 40;
  } else if (brightness <= 45) {
    OCR0A = 45;
  } else if (brightness <= 50) {
    OCR0A = 50;
  } else if (brightness <= 55) {
    OCR0A = 55;
  } else if (brightness <= 60) {
    OCR0A = 60;
  } else if (brightness <= 65) {
    OCR0A = 65;
  } else if (brightness <= 70) {
    OCR0A = 70;
  } else if (brightness <= 75) {
    OCR0A = 75;
  } else if (brightness <= 80) {
    OCR0A = 80;
  } else if (brightness <= 85) {
    OCR0A = 85;
  } else if (brightness <= 90) {
    OCR0A = 90;
  } else {
    OCR0A = 30;
  }

  // fast PWM, set OC0A (boost output pin) on match
  TCCR0A = _BV(WGM00) | _BV(WGM01);  

  // Use the fastest clock
  TCCR0B = _BV(CS00);
 
  TCCR0A |= _BV(COM0A1);
  TIMSK0 |= _BV(TOIE0); // turn on the interrupt for muxing
  sei();
}

/**************************** DISPLAY *****************************/

// We can display the current date!
void display_date(uint8_t style) {

  // This type is mm-dd-yy OR dd-mm-yy depending on our pref.
  if (style == DATE) {
    display[0] = 0;
    display[6] = display[3] = 0x02;     // put dashes between num

    if (region == REGION_US) {
      // mm-dd-yy
      display[1] = pgm_read_byte(numbertable_p + (date_m / 10));
      display[2] = pgm_read_byte(numbertable_p + (date_m % 10));
      display[4] = pgm_read_byte(numbertable_p + (date_d / 10));
      display[5] = pgm_read_byte(numbertable_p + (date_d % 10));
    } else {
      // dd-mm-yy
      display[1] = pgm_read_byte(numbertable_p + (date_d / 10));
      display[2] = pgm_read_byte(numbertable_p + (date_d % 10));
      display[4] = pgm_read_byte(numbertable_p + (date_m / 10));
      display[5] = pgm_read_byte(numbertable_p + (date_m % 10));
    }
    // the yy part is the same
    display[7] = pgm_read_byte(numbertable_p + (date_y / 10));
    display[8] = pgm_read_byte(numbertable_p + (date_y % 10));

  } else if (style == DAY) {
    // This is more "Sunday June 21" style

    uint16_t month, year;
    uint8_t dotw;

    // Calculate day of the week
    
    month = date_m;
    year = 2000 + date_y;
    if (date_m < 3)  {
      month += 12;
      year -= 1;
    }
    dotw = (date_d + (2 * month) + (6 * (month+1)/10) + year + (year/4) - (year/100) + (year/400) + 1) % 7;

    // Display the day first
    display[8] = display[7] = 0;
    switch (dotw) {
    case 0:
      display_str("sunday"); break;
    case 1:
      display_str("monday"); break;
    case 2:
      display_str("tuesday"); break;
    case 3:
      display_str("wednsday"); break;
    case 4:
      display_str("thursday"); break;
    case 5:
      display_str("friday"); break;
    case 6:
      display_str("saturday"); break;
    }
    
    // wait one seconds about
    delayms(1000);

    // Then display the month and date
    display[6] = display[5] = display[4] = 0;
    switch (date_m) {
    case 1:
      display_str("jan"); break;
    case 2:
      display_str("feb"); break;
    case 3:
      display_str("march"); break;
    case 4:
      display_str("april"); break;
    case 5:
      display_str("may"); break;
    case 6:
      display_str("june"); break;
    case 7:
      display_str("july"); break;
    case 8:
      display_str("augst"); break;
    case 9:
      display_str("sept"); break;
    case 10:
      display_str("octob"); break;
    case 11:
      display_str("novem"); break;
    case 12:
      display_str("decem"); break;
    }
    display[7] = pgm_read_byte(numbertable_p + (date_d / 10));
    display[8] = pgm_read_byte(numbertable_p + (date_d % 10));
    
  }
}

// This displays a time on the clock
void display_time(uint8_t h, uint8_t m, uint8_t s) {
  
  // seconds and minutes are at the end
  display[8] =  pgm_read_byte(numbertable_p + (s % 10));
  display[7] =  pgm_read_byte(numbertable_p + (s / 10));
  display[6] = 0;
  display[5] =  pgm_read_byte(numbertable_p + (m % 10));
  display[4] =  pgm_read_byte(numbertable_p + (m / 10)); 
  display[3] = 0;

  // check euro (24h) or US (12h) style time
  if (region == REGION_US) {
    display[2] =  pgm_read_byte(numbertable_p + ( (((h+11)%12)+1) % 10));
    display[1] =  pgm_read_byte(numbertable_p + ( (((h+11)%12)+1) / 10));

    // We use the '*' as an am/pm notice
    if (h >= 12)
      display[0] |= 0x1;  // 'pm' notice
    else 
      display[0] &= ~0x1;  // 'pm' notice
  } else {
    display[2] =  pgm_read_byte(numbertable_p + ( (h%24) % 10));
    display[1] =  pgm_read_byte(numbertable_p + ( (h%24) / 10));
  }
}

// Kinda like display_time but just hours and minutes
void display_alarm(uint8_t h, uint8_t m){ 
  display[8] = 0;
  display[7] = 0;
  display[6] = 0;
  display[5] = pgm_read_byte(numbertable_p + (m % 10));
  display[4] = pgm_read_byte(numbertable_p + (m / 10)); 
  display[3] = 0;

  // check euro or US style time
  if (region == REGION_US) {
    if (h >= 12) {
      display[0] |= 0x1;  // 'pm' notice
      display[7] = pgm_read_byte(alphatable_p + 'p' - 'a');
    } else {
      display[7] = pgm_read_byte(alphatable_p + 'a' - 'a');
      display[0] &= ~0x1;  // 'am' notice
    }
    display[8] = pgm_read_byte(alphatable_p + 'm' - 'a');

    display[2] =  pgm_read_byte(numbertable_p + ( (((h+11)%12)+1) % 10));
    display[1] =  pgm_read_byte(numbertable_p + ( (((h+11)%12)+1) / 10));
  } else {
      display[2] =  pgm_read_byte(numbertable_p + ( (((h+23)%24)+1) % 10));
    display[1] =  pgm_read_byte(numbertable_p + ( (((h+23)%24)+1) / 10));
  }
}

// Kinda like display_time but just hours and minutes allows negative hours.
void display_timezone(int8_t h, uint8_t m){ 
  display[8] = pgm_read_byte(alphatable_p + 'c' - 'a');
  display[7] = pgm_read_byte(alphatable_p + 't' - 'a');
  display[6] = pgm_read_byte(alphatable_p + 'u' - 'a');
  display[5] = pgm_read_byte(numbertable_p + (m % 10));
  display[4] = pgm_read_byte(numbertable_p + (m / 10)); 
  display[3] = 0;
  display[2] = pgm_read_byte(numbertable_p + (abs(h) % 10));
  display[1] = pgm_read_byte(numbertable_p + (abs(h) / 10));
  // We use the '-' as a negative sign
  if (h >= 0)
    display[0] &= ~0x2;  // positive numbers, implicit sign
  else 
    display[0] |= 0x2;  // negative numbers, display negative sign

}

// display words (menus, prompts, etc)
void display_str(char *s) {
  uint8_t i;

  // don't use the lefthand dot/slash digit
  display[0] = 0;

  // up to 8 characters
  for (i=1; i<9; i++) {
    // check for null-termination
    if (s[i-1] == 0)
      return;

    // Numbers and leters are looked up in the font table!
    if ((s[i-1] >= 'a') && (s[i-1] <= 'z')) {
      display[i] =  pgm_read_byte(alphatable_p + s[i-1] - 'a');
    } else if ((s[i-1] >= '0') && (s[i-1] <= '9')) {
      display[i] =  pgm_read_byte(numbertable_p + s[i-1] - '0');
    } else {
      display[i] = 0;      // spaces and other stuff are ignored :(
    }
  }
}

/************************* LOW LEVEL DISPLAY ************************/

// Setup SPI
void vfd_init(void) {
  SPCR  = _BV(SPE) | _BV(MSTR) | _BV(SPR0);
}

// This changes and updates the display
// We use the digit/segment table to determine which
// pins on the MAX6921 to turn on
void setdisplay(uint8_t digit, uint8_t segments) {
  uint32_t d = 0;  // we only need 20 bits but 32 will do
  uint8_t i;

  // Set the digit selection pin
  d |= _BV(pgm_read_byte(digittable_p + digit));

  
  // Set the individual segments for this digit
  for (i=0; i<8; i++) {
    if (segments & _BV(i)) {
      t = 1;
      t <<= pgm_read_byte(segmenttable_p + i);
      d |= t;
    }
  }

  // Shift the data out to the display
  vfd_send(d);
}

// send raw data to display, its pretty straightforward. Just send 32 bits via SPI
// the bottom 20 define the segments
void vfd_send(uint32_t d) {
  // send lowest 20 bits
  cli();       // to prevent flicker we turn off interrupts
  spi_xfer(d >> 16);
  spi_xfer(d >> 8);
  spi_xfer(d);

  // latch data
  VFDLOAD_PORT |= _BV(VFDLOAD);
  VFDLOAD_PORT &= ~_BV(VFDLOAD);
  sei();
}

// Send 1 byte via SPI
void spi_xfer(uint8_t c) {

  SPDR = c;
  while (! (SPSR & _BV(SPIF)));
}

//GPS serial data handling functions:

//Check to see if there is any serial data.
uint8_t gpsdataready(void) {

  return (UCSR0A & _BV(RXC0));

}


void getgpstime(void) {

  uint8_t intOldHr = 0;
  uint8_t intOldMin = 0;
  uint8_t intOldSec = 0;

  char charReceived = UDR0;

  char *strPointer1;
  char strTime[7];
  char strDate[7];
  
  //If the buffer has not been started because a '$' has not been encountered
  //but a '$' is just now encountered, then start filling the buffer.
  if ( ( 0 == intBufferStatus ) && ( '$' == charReceived ) ) {
    intBufferStatus = 1;
    strncat(strBuffer, &charReceived, 1);
    return;
  }

  //If the buffer has started to fill...
  if ( 0 != intBufferStatus ) {
    //If for some reason, the buffer is full, clear it, and start over.
    if ( ! ( ( strlen(strBuffer) < BUFFERSIZE ) ) ) {
      memset( strBuffer, 0, BUFFERSIZE );
      intBufferStatus = 0;
      return;
    }
    //If the buffer has 6 characters in it, it is time to check to see if it is 
    //the line we are looking for that starts with "$GPRMC"
    else if ( 6 == strlen(strBuffer) ) {
      //If the buffer does contain the characters we are looking for,
      //then update the status, add to the buffer, and then return for more.
      if ( 0 == strcmp( strBuffer, "$GPRMC" ) ) {
        //uart_puts("\n\r$GPRMC Found \n\r");
        intBufferStatus = 2;
        strncat(strBuffer, &charReceived, 1);
        return;
      }
      //If the buffer does not contain the characters we are looking for,
      //then clear the buffer and start over..
      else {
        //uart_puts("\n\r$GPRMC Not Found:\t\t");
        //uart_puts(strBuffer);
        memset( strBuffer, 0, BUFFERSIZE );
        intBufferStatus = 0;
        return;
      }
    }

    //If the asterix at the start of the checksum at the end of the line is encountered,
    //then parse the buffer.
    else if ( '*' == charReceived ) {
      //If the buffer status indicates we have not already found the
      //needed start of the string, then start over.
      if ( 2 != intBufferStatus ) {
        memset( strBuffer, 0, BUFFERSIZE );
        intBufferStatus = 0;
        return;
      }
      //If the buffer status indicates we have already found the needed start of the string,
      //then go on to parse the buffer.
      else {
        //Parse the buffer here...
        //Let's test to see if this works:
        uart_puts("\n\r");
        uart_puts(strBuffer);

        //Find the first comma:
        strPointer1 = strchr( strBuffer, ',');

        //Copy the section of memory in the buffer that contains the time.
        memcpy( strTime, strPointer1 + 1, 6 );
        //add a null character to the end of the time string.
        strTime[6] = 0;



        //Find eight more commas to get the date:
        for ( int i = 0; i < 8; i++ ) {
          strPointer1 = strchr( strPointer1 + 1, ',');
        }

        //Copy the section of memory in the buffer that contains the date.
        memcpy( strDate, strPointer1 + 1, 6 );
        //add a null character to the end of the date string.
        strDate[6] = 0;

        //The GPS unit will not have the proper date unless it has received a time update.
        //NOTE: at the turn of the century, the clock will not get updates from GPS
        //for as many years as the value of PROGRAMMING_YEAR
        if ( PROGRAMMING_YEAR <= ( ( (strDate[4] - '0') * 10 ) ) + (strDate[5] - '0') ) {
          //Get the 'old' values of the time:
          intOldHr = time_h;
          intOldMin = time_m;
          intOldSec = time_s;

          //Change the time:
          setgpstime(strTime);
          //Change the date:
          setgpsdate(strDate);

	  //Gussy up the time and date, make the numbers come out right:
          fix_time();

          //Turn the two time values into minutes past midnight
          uint16_t timeMinutes = ((time_h * 60) + (time_m));
          uint16_t oldTimeMinutes = ((intOldHr * 60) + (intOldMin));

          int8_t intTempHr = time_h;
          int8_t intTempMin = time_m;

          //If midnight happened between the old time and the new time
          //and we did not just go back in time...
          if ( ( 0 > (int16_t)( timeMinutes - oldTimeMinutes ) )
               && ( (timeMinutes + 1440) >= oldTimeMinutes )
               && ( abs( timeMinutes + 1440 - oldTimeMinutes ) < abs( timeMinutes - oldTimeMinutes ) ) ) {
            timeMinutes += 1440;
            intTempHr += 24;
          }

          if ( timeMinutes > oldTimeMinutes ) {

            //Count backwards in time to the old time, checking the alarm for each minute.
            for ( ; intTempHr >= intOldHr; intTempHr-- ) {
              for ( ; intTempMin >= 0; intTempMin-- ) {
                check_alarm( (uint8_t)intTempHr, (uint8_t)intTempMin, 0 );
              }
              intTempMin = 59;
            }

          }

        }

        //We've done what we needed to do, so start over.
        memset( strBuffer, 0, BUFFERSIZE );
        intBufferStatus = 0;
        return;
      }
    }
    //If nothing else was found, add to the buffer.
    else {
      strncat(strBuffer, &charReceived, 1);
    }


  }

}

//Set the time with a string taken from GPS data:
void setgpstime(char* str) {
  uint8_t intTempHr = 0;
  uint8_t intTempMin = 0;
  uint8_t intTempSec = 0;

  intTempHr = (str[0] - '0') * 10;
  intTempHr = intTempHr + (str[1] - '0');

  intTempMin = (str[2] - '0') * 10;
  intTempMin = intTempMin + (str[3] - '0');

  intTempSec = (str[4] - '0') * 10;
  intTempSec = intTempSec + (str[5] - '0');

  time_h = intTempHr + intTimeZoneHour;

  //If the time zone offset is negative, then subtract minutes
  if ( 0 > intTimeZoneHour )
    time_m = intTempMin - intTimeZoneMin;
  else
    time_m = intTempMin + intTimeZoneMin;

  time_s = intTempSec;

}

//Set the date with a string taken from GPS data:
void setgpsdate(char* str) {
  uint8_t intTempDay = 0;
  uint8_t intTempMon = 0;
  uint8_t intTempYr = 0;

  intTempDay = (str[0] - '0') * 10;
  intTempDay = intTempDay + (str[1] - '0');

  intTempMon = (str[2] - '0') * 10;
  intTempMon = intTempMon + (str[3] - '0');

  intTempYr = (str[4] - '0') * 10;
  intTempYr = intTempYr + (str[5] - '0');

  timeunknown = 0;
  restored = 0;

  date_d = intTempDay;
  date_m = intTempMon;
  date_y = intTempYr;

}

//Checks the alarm against the passed time.
void check_alarm(uint8_t h, uint8_t m, uint8_t s) {

  if (alarm_on && (alarm_h == h) && (alarm_m == m) && (0 == s)) {
    DEBUGP("alarm on!");
    alarming = 1;
    snoozetimer = 0;
  }

}


//Fixes the time variables whenever time is changed
void fix_time(void) {

  // a minute!
  if (time_s >= 60) {
    time_s = time_s - 60;
    time_m++;
  }
  // If someone decides to make offset seconds with a negative number...
  if (time_s < 0) {
    time_s =  60 + time_s;
    time_m--;
  }

  // an hour...
  if (time_m >= 60) {
    time_m = time_m - 60;
    time_h++; 
    // let's write the time to the EEPROM
    eeprom_write_byte((uint8_t *)EE_HOUR, time_h);
    eeprom_write_byte((uint8_t *)EE_MIN, time_m);
  }
  // When offsets create negative minutes...
  if (time_m < 0) {
    time_m = 60 + time_m;
    time_h--; 
    eeprom_write_byte((uint8_t *)EE_HOUR, time_h);
    eeprom_write_byte((uint8_t *)EE_MIN, time_m);
  }

  // a day....
  if (time_h >= 24) {
    time_h = time_h - 24;
    date_d++;
    eeprom_write_byte((uint8_t *)EE_DAY, date_d);
  }
  // When offsets create negative hours...
  if (time_h < 0) {
    time_h = 24 + time_h;
    date_d--;
    eeprom_write_byte((uint8_t *)EE_DAY, date_d);
  }
  
  //if (! sleepmode) {
  //  uart_putw_dec(time_h);
  //  uart_putchar(':');
  //  uart_putw_dec(time_m);
  //  uart_putchar(':');
  //  uart_putw_dec(time_s);
  //  putstring_nl("");
  //}
  

  // a full month!
  // we check the leapyear and date to verify when it's time to roll over months
  if ((date_d > 31) ||
      ((date_d == 31) && ((date_m == 4)||(date_m == 6)||(date_m == 9)||(date_m == 11))) ||
      ((date_d == 30) && (date_m == 2)) ||
      ((date_d == 29) && (date_m == 2) && !leapyear(2000+date_y))) {
    date_d = 1;
    date_m++;
    eeprom_write_byte((uint8_t *)EE_MONTH, date_m);
  }
  // When offsets create negative days...
  if (date_d < 1) {
    //Find which month we are going back to:
    switch (date_m) {
      case 1: //January -> December
      case 2: //February -> January
      case 4: //April -> March
      case 6: //June -> May
      case 8: //August -> July
      case 9: //September -> August
      case 11: //November -> October
        date_d = 31 + date_d;
        date_m--;
        break;

      case 5: //May -> April
      case 7: //July -> June
      case 10: //October -> September
      case 12: //December -> November
        date_d = 30 + date_d;
        date_m--;
        break;

      case 3: //March -> February, the fun case
        //If we are in a leapyear, February is 29 days long...
        if ( leapyear(2000+date_y) )
          date_d = 29 + date_d;
        else //otherwise, it is 28 days long...
          date_d = 28 + date_d;
        date_m--;
        break;
      default:
        date_d = 1;
        break;
    }

    eeprom_write_byte((uint8_t *)EE_MONTH, date_m);
  }
  
  // HAPPY NEW YEAR!
  if (date_m >= 13) {
    date_y++;
    date_m = 1;
    eeprom_write_byte((uint8_t *)EE_YEAR, date_y);
  }
  //This takes away the years and is cheaper than any cream you can buy...
  if (date_m < 1) {
    date_m = 12 + date_m;
    date_y--;
    eeprom_write_byte((uint8_t *)EE_MONTH, date_m);
    eeprom_write_byte((uint8_t *)EE_YEAR, date_y);
  }


}
