/*
 * AIS packet handler for MSP430 + Si4362
 * Author: Adrian Studer
 * License: CC BY-NC-SA Creative Commons Attribution-NonCommercial-ShareAlike
 * 			http://creativecommons.org/licenses/by-nc-sa/4.0/
 * 			Please contact the author if you want to use this work in a commercial product
 */

#include <msp430.h>
#include <inttypes.h>

#include "radio.h"
#include "fifo.h"
#include "packet_handler.h"

// sync word for AIS
#define AIS_SYNC_WORD		0x7e

#define PH_TIMEOUT_PREAMBLE	6		// number of bits we wait for preamble to start before channel hop
#define PH_TIMEOUT_START	16		// number of bits we wait for start flag before state machine reset

// pins that packet handler uses to receive data
#define	PH_DATA_CLK_PIN		BIT2	// 2.2 RX data clock
#define PH_DATA_PIN			BIT3	// 2.3 RX data
#define PH_DATA_PORT		2	  	// data pins are on port 2 (only ports 1 and 2 supported)

// data port dependent defines
#if (PH_DATA_PORT == 1)
#define PH_DATA_IN P1IN
#define PH_DATA_OUT P1OUT
#define PH_DATA_SEL P1SEL
#define PH_DATA_DIR P1DIR
#define PH_DATA_IFG P1IFG
#define PH_DATA_IE P1IE
#define PH_DATA_IES P1IES
#define PH_DATA_PORT_VECTOR PORT1_VECTOR
#elif (PH_DATA_PORT == 2)
#define PH_DATA_IN P2IN
#define PH_DATA_OUT P2OUT
#define PH_DATA_SEL P2SEL
#define PH_DATA_DIR P2DIR
#define PH_DATA_IFG P2IFG
#define PH_DATA_IE P2IE
#define PH_DATA_IES P2IES
#define PH_DATA_PORT_VECTOR PORT2_VECTOR
#else
#error "Packet handler only supports port 1 and 2."
#endif

volatile uint8_t ph_state = PH_STATE_OFF;
volatile uint8_t ph_last_error = PH_ERROR_NONE;
volatile uint8_t ph_radio_channel = 0;
volatile uint8_t ph_message_type = 0;

// setup packet handler
void ph_setup(void)
{
	// configure data pins as inputs
	PH_DATA_SEL &= ~(PH_DATA_CLK_PIN | PH_DATA_PIN);
	PH_DATA_DIR &= ~(PH_DATA_CLK_PIN | PH_DATA_PIN);
	fifo_reset();
}

// start packet handler operation, including ISR
void ph_start(void)
{
	// enable interrupt on positive edge of pin wired to DATA_CLK (GPIO2 as configured in radio_config.h)
	PH_DATA_IES &= ~PH_DATA_CLK_PIN;
	PH_DATA_IE |= PH_DATA_CLK_PIN;
	_BIS_SR(GIE);      			// enable interrupts

	// reset packet handler state machine
	ph_last_error = PH_ERROR_NONE;
	ph_state = PH_STATE_RESET;
	ph_radio_channel = 0;

	// start radio
#ifndef TEST
	radio_start_rx(ph_radio_channel, 0, 0, RADIO_STATE_NO_CHANGE, RADIO_STATE_NO_CHANGE, RADIO_STATE_NO_CHANGE);
#endif
}

void ph_loop(void)
{
	// change radio channel when indicated by packet handler state machine
	if (ph_state == PH_STATE_HOP) {
		ph_radio_channel ^= 1;				// toggle radio channel between 0 and 1
		ph_state = PH_STATE_RESET;
#ifndef TEST
		radio_start_rx(ph_radio_channel, 0, 0, RADIO_STATE_NO_CHANGE, RADIO_STATE_NO_CHANGE, RADIO_STATE_NO_CHANGE);
#endif
	}
}

// interrupt handler for receiving raw modem data via DATA/DATA_CLK pins
#pragma vector=PH_DATA_PORT_VECTOR
__interrupt void ph_irq_handler(void)
{
	static uint16_t rx_bitstream;				// shift register with incoming data
	static uint16_t rx_bit_count;				// bit counter for various purposes
	static uint16_t rx_crc;						// word for AIS payload CRC calculation
	static uint8_t rx_one_count;				// counter of 1's to identify stuff bits
	static uint8_t rx_data_byte;				// byte to receive actual package data
	uint8_t rx_this_bit_NRZI;					// current bit for NRZI decoding
	static uint8_t rx_prev_bit_NRZI;			// previous bit for NRZI decoding
	uint8_t rx_bit;								// current decoded bit
	static uint8_t rx_prev_bit;					// previous decoded bit (for preamble detection)

	if (PH_DATA_IFG & PH_DATA_CLK_PIN) {		// verify this interrupt is from DATA_CLK/GPIO_2 pin

		// read data bit and decode NRZI
		if (PH_DATA_IN & PH_DATA_PIN)						// read encoded data bit from line
			rx_this_bit_NRZI = 1;
		else
			rx_this_bit_NRZI = 0;

		rx_bit = !(rx_prev_bit_NRZI ^ rx_this_bit_NRZI); 	// NRZI decoding: change = 0-bit, no change = 1-bit, i.e. 00,11=>1, 01,10=>0, i.e. NOT(A XOR B)
		rx_prev_bit_NRZI = rx_this_bit_NRZI;				// store encoded bit for next round of decoding

		// add decoded bit to bit-stream (receiving LSB first)
		rx_bitstream >>= 1;
		if (rx_bit)
			rx_bitstream |= 0x8000;

		// packet handler state machine
		switch (ph_state) {
		case PH_STATE_OFF:									// state: off, do nothing
			break;

		case PH_STATE_RESET:								// state: reset, prepare state machine for next packet
			rx_bitstream = 0;								// reset bit-stream
			rx_bit_count = 0;								// reset bit counter
			fifo_new_packet();								// reset fifo packet
			fifo_write_byte(ph_radio_channel);				// indicate channel for this packet
			// TODO: wakeup main thread from low-power mode to allow for error reporting
			ph_state = PH_STATE_WAIT_FOR_PREAMBLE;			// next state: wait for training sequence
			break;

		case PH_STATE_WAIT_FOR_PREAMBLE:					// state: waiting for at least 16 bit of training sequence (01010..)
			if (rx_bitstream == 0x5555) {					// if we found 16 alternating bits
				rx_bit_count = 0;							// reset bit counter
				ph_state = PH_STATE_WAIT_FOR_START;			// next state: wait for start flag
				break;
			}

			rx_bit_count++;									// increase bit counter
			if (rx_bit_count > PH_TIMEOUT_PREAMBLE)	{		// if we reached preamble timeout
				if (!(rx_bit ^ rx_prev_bit)) {				// and there's no preamble in progress, i.e. two identical bits in a row
					ph_state = PH_STATE_HOP;				// indicate packet handler loop to initiate hop
					// TODO: wakeup main thread from low-power mode to do hopping
					break;
				}
			}

			break;

		case PH_STATE_WAIT_FOR_START:						// state: wait for start flag 0x7e
			if ((rx_bitstream & 0xff00) == 0x7e00) {		// if we found the start flag
				rx_bit_count = 0;							// reset bit counter
				ph_state = PH_STATE_PREFETCH;				// next state: start receiving packet
				break;
			}

			rx_bit_count++;									// increase bit counter
			if (rx_bit_count > PH_TIMEOUT_START) {			// if start flag is not found within a certain number of bits
				ph_last_error = PH_ERROR_NOSTART;			// report missing start flag error
				ph_state = PH_STATE_RESET;					// reset state machine
				break;
			}

			break;

		case PH_STATE_PREFETCH:								// state: pre-fill receive buffer with 8 bits
			rx_bit_count++;									// increase bit counter
			if (rx_bit_count == 8) {						// after 8 bits arrived
				rx_bit_count = 0;							// reset bit counter
				rx_one_count = 0;							// reset counter for stuff bits
				rx_data_byte = 0;							// reset buffer for data byte
				rx_crc = 0xffff;							// init CRC calculation
				ph_state = PH_STATE_RECEIVE_PACKET;			// next state: receive and process packet
				ph_message_type = rx_bitstream >> 10;		// store AIS message type for debugging
				break;
			}

			break;											// do nothing for the first 8 bits to fill buffer

		case PH_STATE_RECEIVE_PACKET:						// state: receiving packet data
			rx_bit = rx_bitstream & 0x80;					// extract data bit for processing

			if (rx_one_count == 5) {						// if we expect a stuff-bit..
				if (rx_bit) {								// if stuff bit is not zero the packet is invalid
					ph_last_error = PH_ERROR_STUFFBIT;		// report invalid stuff-bit error
					ph_state = PH_STATE_RESET;				// restart state machine
				} else
					rx_one_count = 0;						// else ignore bit and reset stuff-bit counter
				break;
			}

			rx_data_byte = rx_data_byte >> 1 | rx_bit;		// shift bit into current data byte

			if (rx_bit) {									// if current bit is a 1
				rx_one_count++;								// count 1's to identify stuff bit
				rx_bit = 1;									// avoid shifting for CRC
			} else
				rx_one_count = 0;							// or reset stuff-bit counter

			if (rx_bit ^ (rx_crc & 0x0001))					// CCITT CRC calculation (according to Dr. Dobbs)
				rx_crc = (rx_crc >> 1) ^ 0x8408;
			else
				rx_crc >>= 1;

			if ((rx_bit_count & 0x07)==0x07) {				// every 8th bit.. (counter started at 0)
				fifo_write_byte(rx_data_byte);				// add buffered byte to FIFO
				rx_data_byte = 0;							// reset buffer
			}

			rx_bit_count++;									// count valid, de-stuffed data bits

			if ((rx_bitstream & 0xff00) == 0x7e00) {		// if we found the end flag 0x7e we're done
				if (rx_crc != 0xf0b8)						// if CRC verification failed
					ph_last_error = PH_ERROR_CRC;			// report CRC error
				else {
					fifo_commit_packet();					// else commit packet in FIFO
					// TODO: wakeup main thread from low-power mode
				}
				ph_state = PH_STATE_RESET;					// restart state machine
				break;
			}

			if (rx_bit_count > 1020) {						// if packet is too long, it's probably invalid
				ph_last_error = PH_ERROR_NOEND;				// report error
				ph_state = PH_STATE_RESET;					// reset state machine
				break;
			}

			break;

		case PH_STATE_HOP:									// state: waiting for radio channel hop
			// nothing to do, ph_loop() will push me out of this state
			break;

		}

		rx_prev_bit = rx_bit;								// store last bit for future use (e.g. preamble detection)
	}

	PH_DATA_IFG = 0;							// clear all pin interrupt flags
}

// stop receiving and processing packets
void ph_stop(void)
{
	// disable interrupt on pin wired to GPIO2
	PH_DATA_IE &= ~PH_DATA_CLK_PIN;

	// transition radio from RX to READY
	radio_change_state(RADIO_STATE_READY);
}

// get current state of packet handler state machine
uint8_t ph_get_state(void)
{
	return ph_state;
}

// get last error reported by packet handler
uint8_t ph_get_last_error(void)
{
	uint8_t error = ph_last_error;
	ph_last_error = PH_ERROR_NONE;			// clear error
	return error;
}

uint8_t ph_get_radio_channel(void)
{
	return ph_radio_channel;
}
uint8_t ph_get_message_type(void)
{
	return ph_message_type;
}

#ifdef TEST

// configure packet handler for self-test
void test_ph_setup(void)
{
	// configure data pins as outputs to test packet handler interrupt routine
	PH_DATA_SEL &= ~(PH_DATA_CLK_PIN | PH_DATA_PIN);
	PH_DATA_DIR |= PH_DATA_CLK_PIN | PH_DATA_PIN;
}

// encode and send one bit for packet handler self-test
void test_ph_send_bit_nrzi(uint8_t tx_bit)
{
	// send negative clock edge
	PH_DATA_OUT &= ~PH_DATA_CLK_PIN;

	// NRZI = toggle data pin for sending 0, keep state for sending 1
	if (tx_bit == 0)
		PH_DATA_OUT ^= PH_DATA_PIN;

	// wait a while to roughly simulate 9600 baud @16Mhz
	_delay_cycles(800);

	// send positive clock edge
	PH_DATA_OUT |= PH_DATA_CLK_PIN;

	// wait a while to roughly simulate 9600 baud @16Mhz
	_delay_cycles(800);
}

// send one NMEA encoded AIS payload to packet handler for self-test
void test_ph_send_packet(const char* message)
{
	// emulating AIS communication received from modem
	// wraps buffer into packet and sends it to packet handler

	uint8_t tx_bit = 0;
	uint8_t tx_byte;
	uint16_t tx_crc = 0xffff;
	uint8_t asc_byte;

	unsigned int asc_bit_count;
	unsigned int tx_bit_count;
	unsigned int one_count;

	unsigned int i;
	unsigned int j;


	// send preamble, up to 24 bits
	for (i = 20; i != 0; i--) {
		test_ph_send_bit_nrzi(tx_bit);
		tx_bit ^= 1;
	}

	// send sync word
	tx_byte = AIS_SYNC_WORD;
	for (i = 8; i != 0; i--) {
		if(tx_byte & 0x01) {
			test_ph_send_bit_nrzi(1);
		} else {
			test_ph_send_bit_nrzi(0);
		}
		tx_byte >>= 1;
	}

	// send packet
	one_count = 0;
	asc_bit_count = 0;
	asc_byte = 0;
	tx_bit_count = 0;
	tx_byte = 0;
	j = 0;
	while (message[j]) {
		// (re)fill transmission byte with next 8 bits
		while (tx_bit_count != 8) {
			if (asc_bit_count == 0) {
				// fetch next character and convert NMEA ASCII to 6-bit binary
				if (message[j]) {
					asc_byte = message[j] - 48;
					if (asc_byte > 40)
						asc_byte -= 8;
					asc_bit_count = 6;
					j++;
				} else {
					// no more bits - should not happen as test messages are 8 bit
					break;
				}
			}

			// shift new ascii bit into transmission byte
			tx_byte <<= 1;
			if (asc_byte & 0x20)
				tx_byte |= 0x01;
			tx_bit_count++;
			asc_byte <<= 1;
			asc_bit_count--;
		}

		// send bits, LSB first
		while (tx_bit_count != 0) {
			if (tx_byte & 0x01) {
				test_ph_send_bit_nrzi(1);
				one_count++;
			} else {
				test_ph_send_bit_nrzi(0);
				one_count = 0;
			}

			if ((tx_byte & 0x01) ^ (tx_crc & 0x0001)) 				// CCITT CRC calculation (according to Dr. Dobbs)
				tx_crc = (tx_crc >> 1) ^ 0x8408;
			else
				tx_crc >>= 1;

			tx_byte >>= 1;
			tx_bit_count--;

			// stuff with 0 after five consecutive 1's
			if (one_count == 5) {
				test_ph_send_bit_nrzi(0);
				one_count = 0;
			}
		}
	};

	// finish calculation of CRC and send CRC LSB first
	tx_crc = ~tx_crc;
//	tx_crc = (tx_crc << 8) | (tx_crc >> 8 & 0xff);			// no byte swap required as we send big-endian
	for (i = 16; i != 0; i--) {
		if (tx_crc & 0x01) {
			test_ph_send_bit_nrzi(1);
			one_count++;
		} else {
			test_ph_send_bit_nrzi(0);
			one_count = 0;
		}
		tx_crc >>= 1;

		// stuff with 0 after five consecutive 1's
		if (one_count == 5) {
			test_ph_send_bit_nrzi(0);
			one_count = 0;
		}
	}

	// send sync word
	tx_byte = AIS_SYNC_WORD;
	for (i = 8; i != 0; i--) {
		if (tx_byte & 0x01)
			test_ph_send_bit_nrzi(1);
		else
			test_ph_send_bit_nrzi(0);
		tx_byte >>= 1;
	}
}

#endif // TEST
