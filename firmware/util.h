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

#if (F_CPU == 16000000)
#define BRRL_4800 207    // for 16MHZ
#define BRRL_9600 103    // for 16MHZ
#define BRRL_192 52    // for 16MHZ
#elif (F_CPU == 8000000)
#define BRRL_4800 103
#define BRRL_9600 52
#define BRRL_192 26    
#endif

#define NOP asm("nop");
#define uart_putc(c) uart_putchar(c)

void delay_ms(unsigned char ms);
void delay_10us(uint8_t us);
void delay_s(uint8_t s);

int uart_putchar(char c);

char uart_getchar(void);

void uart_init(uint16_t BRR);


void uart_putc_hex(uint8_t b);
void uart_putw_hex(uint16_t w);
void uart_putdw_hex(uint32_t dw);

void uart_putw_dec(uint16_t w);
void uart_putdw_dec(uint32_t dw);
void uart_puts(const char* str);

void RAM_putstring(char *str);
void ROM_putstring(const char *str, uint8_t nl);

#define putstring(x) ROM_putstring(PSTR(x), 0)
#define putstring_nl(x) ROM_putstring(PSTR(x), 1)
#define nop asm volatile ("nop\n\t")
