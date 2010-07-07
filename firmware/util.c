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

#include <avr/io.h>
#include <avr/interrupt.h>
#include <util/delay.h>
#include <avr/pgmspace.h>
#include "util.h"

void delay_10us(uint8_t ns)
{
  uint8_t i;
  
  while (ns != 0) {
    ns--;
    for (i=0; i< 30; i++) {
      nop;
    }
  }
}

void delay_s(uint8_t s) {
  while (s--) {
    _delay_ms(1000);
  }
}

void uart_init(uint16_t BRR) {
  /* setup the main UART */
  UBRR0 = BRR;               // set baudrate counter

  UCSR0B = _BV(RXEN0) | _BV(TXEN0);
  UCSR0C = _BV(USBS0) | (3<<UCSZ00);
  DDRD |= _BV(PD1);
  DDRD &= ~_BV(PD0);

}

int uart_putchar(char c)
{
  loop_until_bit_is_set(UCSR0A, UDRE0);
  UDR0 = c;
  return 0;
}

char uart_getchar(void) {
	while (!(UCSR0A & _BV(RXC0)));
	return UDR0;
}

char uart_getch(void) {
	return (UCSR0A & _BV(RXC0));
}

void ROM_putstring(const char *str, uint8_t nl) {
    uint8_t i;

    for (i=0; pgm_read_byte(&str[i]); i++) {
        uart_putchar(pgm_read_byte(&str[i]));
  }
  if (nl) {
        uart_putchar('\n'); uart_putchar('\r');
  }
}

void uart_puts(const char* str)
{
    while(*str)
        uart_putc(*str++);
}


void uart_putc_hex(uint8_t b)
{
    /* upper nibble */
    if((b >> 4) < 0x0a)
        uart_putc((b >> 4) + '0');
    else
        uart_putc((b >> 4) - 0x0a + 'a');

    /* lower nibble */
    if((b & 0x0f) < 0x0a)
        uart_putc((b & 0x0f) + '0');
    else
        uart_putc((b & 0x0f) - 0x0a + 'a');
}

void uart_putw_hex(uint16_t w)
{
    uart_putc_hex((uint8_t) (w >> 8));
    uart_putc_hex((uint8_t) (w & 0xff));
}

void uart_putdw_hex(uint32_t dw)
{
    uart_putw_hex((uint16_t) (dw >> 16));
    uart_putw_hex((uint16_t) (dw & 0xffff));
}

void uart_putw_dec(uint16_t w)
{
    uint16_t num = 10000;
    uint8_t started = 0;

    while(num > 0)
    {
        uint8_t b = w / num;
        if(b > 0 || started || num == 1)
        {
            uart_putc('0' + b);
            started = 1;
        }
        w -= b * num;

        num /= 10;
    }
}

void uart_putdw_dec(uint32_t dw)
{
    uint32_t num = 1000000000;
    uint8_t started = 0;

    while(num > 0)
    {
        uint8_t b = dw / num;
        if(b > 0 || started || num == 1)
        {
            uart_putc('0' + b);
            started = 1;
        }
        dw -= b * num;

        num /= 10;
    }
}
