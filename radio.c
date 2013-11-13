/*
 * Library to control Silicon Laboratories SI6342 radio
 * Author: Adrian Studer
 */

#include <msp430.h>
#include <inttypes.h>

#include "radio.h"
#include "radio_config.h"
//#include "radio_config_Si4362 with CRC.h"
#include "spi.h"

#define	SPI_NSEL	BIT4	// chip select pin 1.4

#define SPI_ON		P1OUT &= ~SPI_NSEL;			// turn SPI on (NSEL=0)
#define SPI_OFF		P1OUT |= SPI_NSEL;			// turn SPI off (NSEL=1)

#define GPIO_0		BIT0	// 2.0 sync word, high when detected
#define GPIO_1		BIT1	// 2.1 CTS, when low, chip is busy/not ready (default)
#define GPIO_2		BIT2	// 2.2 RX data clock
#define GPIO_3		BIT3	// 2.3 RX data
#define SDN			BIT4	// 2.4 chip shutdown, set high for 1us to reset radio, pulled low by 100k resistor
#define NIRQ		BIT5	// 2.5 preamble, high when detected (for debug only, use sync word for actual package detection)

#define	DATA_CLK_PIN	GPIO_2
#define DATA_PIN		GPIO_3

#define SYNC_WORD_DETECTED	(P2IN & GPIO_0)
#define RADIO_READY			(P2IN & GPIO_1)
#define RX_DATA_CLK			(P2IN & DATA_CLK_PIN)
#define RX_DATA				(P2IN & DATA_PIN)
#define PREAMBLE_DETECTED	(P2IN & NIRQ)
#define CCA_DETECTED		(P2IN & NIRQ)

#define CMD_NOP						0x00
#define CMD_PART_INFO				0x01
#define CMD_FUNC_INFO				0x10
#define CMD_POWER_UP				0x12
#define CMD_FIFO_INFO				0x15
#define CMD_GET_INT_STATUS			0x20
#define CMD_GET_PH_STATUS			0x21
#define CMD_GET_MODEM_STATUS		0x22
#define CMD_GET_CHIP_STATUS			0x23
#define CMD_START_RX				0x32
#define CMD_REQUEST_DEVICE_STATE	0x33
#define CMD_CHANGE_STATE			0x34
#define CMD_READ_CMD_BUFF			0x44
#define CMD_READ_RX_FIFO			0x77

const uint8_t radio_configuration[] = RADIO_CONFIGURATION_DATA_ARRAY;

union radio_buffer_u radio_buffer;

void send_command(uint8_t cmd, const uint8_t *send_buffer, uint8_t send_length, uint8_t response_length);
int receive_result(uint8_t length);

void radio_setup(void)
{
	// initialize SPI pins
	P1SEL &= ~SPI_NSEL;								// NSEL pin as I/O
	P1DIR |= SPI_NSEL;								// set NSEL pin to output
	SPI_OFF;										// turn off chip select

	spi_init();

	// initialize GPIO pins
	P2SEL &= ~(GPIO_0 | GPIO_1 | GPIO_2 | GPIO_3);	// all GPIO pins are I/O
	P2DIR &= ~(GPIO_0 | GPIO_1 | GPIO_2 | GPIO_3);	// all GPIO pins are input

	// initialize shutdown pin
	P2SEL &= ~SDN;									// shutdown pin is I/O
	P2DIR |= SDN;									// shutdown pin is output

	return;
}

void radio_configure(void)
{
	// reset radio: SDN=1, wait >1us, SDN=0
	P2OUT |= SDN;
	_delay_cycles(1000);
	P2OUT &= ~SDN;

	while (!RADIO_READY);						// wait for chip to wake up

	// transfer radio configuration
	const uint8_t *cfg = radio_configuration;
	while (*cfg)	{							// configuration array stops with 0
		char count = (*cfg++) - 1;				// 1st byte: number of bytes, incl. command
		char cmd = *cfg++;						// 2nd byte: command
		send_command(cmd, cfg, count, 0);		// send bytes to chip
		cfg += count;							// point at next line
		while (!RADIO_READY);					// wait for chip to complete operation
	}

	return;
}

void radio_start(void)
{
	// enable interrupt on positive edge of pin wired to DATA_CLK (GPIO2 as configured in radio_config.h)
	P2IES &= ~DATA_CLK_PIN;
	P2IE |= DATA_CLK_PIN;
	_BIS_SR(GIE);      			// enable interrupts

	// transition radio into receive state
	radio_start_rx(0, 0, 0, RADIO_STATE_NO_CHANGE, RADIO_STATE_NO_CHANGE, RADIO_STATE_NO_CHANGE);
}

// interrupt handler for receiving data via DATA/DATA_CLK
#pragma vector=PORT2_VECTOR
__interrupt void radio_irq_handler(void)
{
	static uint8_t rx_prev_bit = 0;				// previous bit for NRZI decoding
	static uint16_t rx_bitstream = 0;			// last 16 bits processed

	uint8_t rx_this_bit, rx_bit;

	if(P2IFG & DATA_CLK_PIN) {					// verify this interrupt is from DATA_CLK/GPIO_2 pin

		if(P2IN & DATA_PIN)	{					// read bit and decode NRZI
			rx_this_bit = 1;
		} else {
			rx_this_bit = 0;
		}
		rx_bit = !(rx_prev_bit^rx_this_bit); 	// NRZI decoding: change = 0-bit, no change = 1-bit, i.e. 00,11=>1, 01,10=>0, i.e. NOT(A XOR B)
		rx_prev_bit = rx_this_bit;

		rx_bitstream = (rx_bitstream << 1) + rx_bit;
	}
}

void radio_stop(void)
{
	// disable interrupt on pin wired to GPIO2
	P2IE &= ~DATA_CLK_PIN;

	// transition radio from RX to READY
	radio_change_state(RADIO_STATE_READY);
}

void radio_part_info(void)
{
	send_command(CMD_PART_INFO, 0, 0, sizeof(radio_buffer.part_info));
}

void radio_func_info(void)
{
	send_command(CMD_FUNC_INFO, 0, 0, sizeof(radio_buffer.func_info));
}

void radio_fifo_info(uint8_t reset_fifo)
{
	radio_buffer.data[0] = reset_fifo;
	send_command(CMD_FIFO_INFO, radio_buffer.data, 1, sizeof(radio_buffer.fifo_info));
}

void radio_get_int_status(uint8_t ph_clr_pending, uint8_t modem_clr_pending, uint8_t chip_clr_pending)
{
	radio_buffer.data[0] = ph_clr_pending;
	radio_buffer.data[1] = modem_clr_pending;
	radio_buffer.data[2] = chip_clr_pending;
	send_command(CMD_GET_INT_STATUS, radio_buffer.data, 3, sizeof(radio_buffer.int_status));
}

void radio_get_ph_status(uint8_t clr_pending)
{
	radio_buffer.data[0] = clr_pending;
	send_command(CMD_GET_PH_STATUS, radio_buffer.data, 1, sizeof(radio_buffer.ph_status));
}

void radio_get_chip_status(uint8_t clr_pending)
{
	radio_buffer.data[0] = clr_pending;
	send_command(CMD_GET_CHIP_STATUS, radio_buffer.data, 1, sizeof(radio_buffer.chip_status));
}

void radio_get_modem_status(uint8_t clr_pending)
{
	radio_buffer.data[0] = clr_pending;
	send_command(CMD_GET_MODEM_STATUS, radio_buffer.data, 1, sizeof(radio_buffer.modem_status));
}

void radio_start_rx(uint8_t channel, uint8_t start_condition, uint16_t rx_length, uint8_t rx_timeout_state,	uint8_t rx_valid_state,	uint8_t rx_invalid_state)
{
	radio_buffer.data[0] = channel;
	radio_buffer.data[1] = start_condition;
	radio_buffer.data[2] = rx_length >> 8;
	radio_buffer.data[3] = rx_length & 0xff;
	radio_buffer.data[4] = rx_timeout_state;
	radio_buffer.data[5] = rx_valid_state;
	radio_buffer.data[6] = rx_invalid_state;
	send_command(CMD_START_RX, radio_buffer.data, 7, 0);
}

uint16_t radio_receive_bitstream(void)
{
	while(!SYNC_WORD_DETECTED)			// wait for sync word, or radio to exit receive mode
	{
/* don't check radio state as this will take too much time!
		radio_request_device_state();
		if((radio_buffer.device_state.curr_state != RADIO_STATE_RX) && (radio_buffer.device_state.curr_state != RADIO_STATE_TUNE_RX))
		{
			return 0;					// radio no longer ready to receive
		}
*/
	};

	uint16_t c = 0;						// counting number of bits received
	uint16_t i = 0;						// index in receive buffer
	uint8_t d = 0;						// currently received data byte

	while(SYNC_WORD_DETECTED) {			// while modem indicates sync
		while(RX_DATA_CLK);				// wait for clock to be 0
		while(!RX_DATA_CLK);			// wait for positive clock edge

		d <<= 1;						// shift bit into data byte
		if(RX_DATA)	{
			d |= 0x01;
		}

		c++;							// counting bits

		if((c & 0x07) == 0)	{			// store full bytes in receive buffer
			radio_buffer.data[i] = d;
			i++;
			d = 0;
		}
	}

	if((c & 0x07) != 0) {				// store remaining bits in receive buffer
		radio_buffer.data[i] = d;
	}

	return c;
}

uint16_t radio_receive_bitstream_nrzi(uint8_t sync_word)
{
	while(!CCA_DETECTED);			// wait for RSSI threshold to be exceeded

	uint16_t shiftreg = 0;
	uint8_t prevbit = 0;
	uint8_t thisbit = 0;
	uint16_t c;						// counting number of bits received
	uint16_t i = 0;						// index in receive buffer

	do {
			while(RX_DATA_CLK && CCA_DETECTED);				// wait for clock to be 0
			while(!RX_DATA_CLK && CCA_DETECTED);			// wait for positive clock edge

			if(!CCA_DETECTED) {
				return 0;
			}

			shiftreg >>= 1;

			if(RX_DATA)	{
				thisbit = 1;
			} else {
				thisbit = 0;
			}

			if(thisbit == prevbit) {
				shiftreg |= 0x8000;
			}
			else
			{
				shiftreg &= 0x7fff;
			}

			prevbit = thisbit;

	} while (shiftreg != 0x5555);

	c = 24;

	do {
		while(RX_DATA_CLK && CCA_DETECTED);				// wait for clock to be 0
		while(!RX_DATA_CLK && CCA_DETECTED);			// wait for positive clock edge

		shiftreg >>= 1;

		if(RX_DATA)	{
			thisbit = 1;
		} else {
			thisbit = 0;
		}

		if(thisbit == prevbit) {
			shiftreg |= 0x8000;
		}
		else
		{
			shiftreg &= 0x7fff;
		}

		prevbit = thisbit;

		c--;
	} while ((((shiftreg >> 8) & 0xff) != sync_word) && (c != 0));

	if(c == 0) {
		return 0;
	}

	c = 0;

	do {
		while(RX_DATA_CLK && CCA_DETECTED);				// wait for clock to be 0
		while(!RX_DATA_CLK && CCA_DETECTED);			// wait for positive clock edge

		shiftreg >>= 1;						// shift bit into data byte

		if(RX_DATA)	{
			thisbit = 1;
		} else {
			thisbit = 0;
		}

		if(thisbit == prevbit) {
			shiftreg |= 0x8000;
		}
		else
		{
			shiftreg &= 0x7fff;
		}

		prevbit = thisbit;

		c++;							// counting bits

		if((c & 0x07) == 0)	{			// store full bytes in receive buffer
			radio_buffer.data[i] = shiftreg >> 8;
			i++;
		}

	} while ((shiftreg >> 8) != sync_word && i < 0x1b && (CCA_DETECTED));

	if ((shiftreg >> 8) != sync_word)
	{
		return 0;
	}

	if((c & 0x07) != 0) {				// store remaining bits in receive buffer
		radio_buffer.data[i] = shiftreg >> 8;
	}

	return c;
}

void radio_request_device_state(void)
{
	send_command(CMD_REQUEST_DEVICE_STATE, 0, 0, sizeof(radio_buffer.device_state));
}

void radio_change_state(uint8_t next_state)
{
	radio_buffer.data[0] = next_state;
	send_command(CMD_CHANGE_STATE, radio_buffer.data, 1, 0);
}

uint8_t radio_read_rx_fifo(void)
{
	uint8_t fifo_count = 0;

	radio_fifo_info(0);
	fifo_count = radio_buffer.fifo_info.rx_fifo_count;

	if (fifo_count)	{

		SPI_ON;
		spi_transfer(CMD_READ_RX_FIFO);						// send command to read FIFO

		uint8_t i = 0;
		while (i < fifo_count) {
			radio_buffer.data[i] = spi_transfer(0);			// receive response
			i++;
		}

		SPI_OFF;
	}

	return fifo_count;
}

void radio_debug(void)
{
	radio_get_int_status(0, 0, 0);
	radio_get_chip_status(0);
	radio_get_modem_status(0);
	radio_part_info();
	radio_func_info();
	radio_request_device_state();
}

// send command, including optional parameters if sendBuffer != 0
void send_command(uint8_t cmd, const uint8_t *send_buffer, uint8_t send_length, uint8_t response_length)
{
	SPI_ON;

	spi_transfer(cmd);						// transmit command

	if (send_length && send_buffer) {		// if there are parameters to send, do so
		uint8_t c = 0;
		while (c != send_length) {
			spi_transfer(send_buffer[c]);	// transmit byte
			c++;
		}
	}

	SPI_OFF;

	while (!RADIO_READY);					// always wait for completion

	if (response_length) {
		while (receive_result(response_length) == 0);
	}

	return;
}

// read result: write 44h, read CTS byte, if 0xff read result bytes, else loop (cycle NSEL)
int receive_result(uint8_t length)
{
	SPI_ON;

	spi_transfer(CMD_READ_CMD_BUFF);		// send 0x44 to receive data/CTS
	if(spi_transfer(0) != 0xff)	{			// if CTS is not 0xff
		SPI_OFF;
		return 0;							// data not ready yet
	}

	uint8_t i = 0;							// data ready, read result into buffer
	while (i < length) {
		radio_buffer.data[i] = spi_transfer(0);		// receive byte
		i++;
	}

	SPI_OFF;

	return 1;						// data received
}

