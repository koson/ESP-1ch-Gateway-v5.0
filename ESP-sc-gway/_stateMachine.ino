// 1-channel LoRa Gateway for ESP8266
// Copyright (c) 2016, 2017, 2018 Maarten Westenberg version for ESP8266
// Version 5.1.0
// Date: 2018-04-25
//
// 	based on work done by Thomas Telkamp for Raspberry PI 1ch gateway
//	and many others.
//
// All rights reserved. This program and the accompanying materials
// are made available under the terms of the MIT License
// which accompanies this distribution, and is available at
// https://opensource.org/licenses/mit-license.php
//
// Author: Maarten Westenberg (mw12554@hotmail.com)
//
// This file contains the state machine code enabling to receive
// and transmit packages/messages.
// ========================================================================================
//


// ----------------------------------------------------------------------------
// stateMachine handler of the state machine.
// We use ONE state machine for all kind of interrupts. This assures that we take
// the correct action upon receiving an interrupt.
//
// _event is the software interrupt: If set this function is executed from loop(),
// the function should itself take care of setting or resetting the variable.
//
// STATE MACHINE
// The program uses the following state machine (in _state), all states
// are done in interrupt routine, only the follow-up of S-RXDONE is done
// in the main loop() program. This is because otherwise the interrupt processing
// would take too long to finish
//
// So _state has one of the following state values:
//
// S-INIT=0, 	The commands in this state are executed only once
//	- Goto S_SCAN
//
// S-SCAN, 		CadScanner() part
//	- Upon CDDECT (int1) goto S_RX, 
//	- upon CDDONE (int0) goto S_CAD, walk through all SF until CDDETD
//	- Else stay in SCAN state
//
// S-CAD, 		
//	- Upon CDDECT (int1) goto S_RX, 
//	- Upon CDDONE (int0) goto S_SCAN, start with SF7 recognition again
//
// S-RX, 		Received CDDECT so message detected, RX cycle started. 
//	- Upon RXDONE (int0) package read. If read ok continue to read message
//	- upon RXTOUT (int1) error, goto S_SCAN
//
// S-TX			Transmitting a message
//	- Upon TXDONE goto S_SCAN
//
// S-TXDONE		Transmission complete by loop() now again in interrupt
//	- Set the Mask
//	- reset the Flags
//	- Goto either SCAN or RX
//
// This interrupt routine has been kept as simple and short as possible.
// If we receive an interrupt that does not below to a _state then print error.
// _event is a special variable which indicate that an interrupt event has happened
//	and we need to take action OR that we generate a soft interrupt for state machine.
// 
// NOTE: We may clear the interrupt but leave the flag for the moment. 
//	The eventHandler should take care of repairing flags between interrupts.
// ----------------------------------------------------------------------------

void stateMachine()
{
	// Determine what interrupt flags are set
	//
	uint8_t flags = readRegister(REG_IRQ_FLAGS);
	uint8_t mask  = readRegister(REG_IRQ_FLAGS_MASK);
	uint8_t intr  = flags & ( ~ mask );				// Only react on non masked interrupts
#if DUSB>=1
	if (intr != flags) {
		Serial.print(F("FLAG  ::"));
		SerialStat(intr);
	}
#endif
	uint8_t rssi;
	_event=0;									// Reset the interrupt detector

	// If Hopping is selected AND if there is NO event interrupt detected 
	// and the state machine is called anyway
	// then we know its a soft interrupt and we do nothing and return to main loop.
	//
	if ((_hop) && (intr == 0x00) )
	{
		// If we are not in scanning state, and there will be an interrupt coming,
		// In state S_RX it could be RXDONE in which case allow kernel time
		//
		if ((_state == S_SCAN) || (_state == S_CAD)) {

			_event=0;
		
			uint32_t eventWait = EVENT_WAIT;
			switch (_state) {
				case S_INIT:	eventWait = 0; break;
				case S_SCAN:	eventWait = EVENT_WAIT * 1; break;
				case S_CAD:		eventWait = EVENT_WAIT * 1; break;
				case S_RX:		eventWait = EVENT_WAIT * 8; break;
				case S_TX:		eventWait = EVENT_WAIT * 1; break;
				case S_TXDONE:	eventWait = EVENT_WAIT * 4; break;
				default:
					eventWait=0;
#if DUSB>=1
					Serial.print(F("DEF  :: "));
					SerialStat(intr);
#endif
			}
			//
			// So we init the wait time for RXDONE based on the current SF.
			// As for highter CF it takes longer to receive symbols
			//
			uint32_t doneWait = DONE_WAIT;			// Initial value
			switch (sf) {
				case SF7: 
					break;
				case SF8: 
					doneWait *= 2;		// Assume symbols in SF8 take twice the time of SF7
					break;
				case SF9: 
					doneWait *= 4;
					break;
				case SF10:
					doneWait *= 8;
					break;
				case SF11:
					doneWait *= 16;
					break;
				case SF12:
					doneWait *= 32;
					break;
				default:
#if DUSB>=1
				Serial.print(F("PRE:: DEF set"));
				Serial.println();
#endif
			}

			// If micros is starting over again after 51 minutes 
			// it's value is smaller than an earlier value of eventTime/doneTime
			//
			if (eventTime > micros())	eventTime=micros();
			if (doneTime > micros())	doneTime=micros();

			if ((micros() - doneTime) > doneWait ) 
			{
				_state = S_SCAN;
				hop();								// increment ifreq = (ifreq + 1) % NUM_HOPS ;
				cadScanner();						// Reset to initial SF, leave frequency "freqs[ifreq]"
				rxLoraModem();
#if DUSB>=1
				if (( debug >= 1 ) && ( pdebug & P_PRE )) {
					Serial.print(F("DONE::  "));
					SerialStat(intr);
				}
#endif
				eventTime=micros();					// reset the timer on timeout
				doneTime=micros();					// reset the timer on timeout
				return;
			}
			// If timeout occurs and still no _event, then increase SF
			// and when reaching SF12 hop and return
			// Start scanning again
			//
			if ((micros() - eventTime) > eventWait ) 
			{
				_state = S_SCAN;
				hop();								// increment ifreq = (ifreq + 1) % NUM_HOPS ;
				cadScanner();						// Reset to initial SF, leave frequency "freqs[ifreq]"
#if DUSB>=1
				if (( debug >= 2 ) && ( pdebug & P_PRE )) {
					Serial.print(F("HOP ::  "));
					SerialStat(intr);
				}
#endif
				eventTime=micros();					// reset the timer on timeout
				doneTime=micros();					// reset the timer on timeout
				return;
			}
			
		
			// If we are here, NO timeout has occurred 
			// So we need to return to the main State Machine
			// as there was NO interrupt

#if DUSB>=1
			if (( debug>=3 ) && ( pdebug & P_PRE )) {
				Serial.print(F("PRE:: eventTime="));
				Serial.print(eventTime);
				Serial.print(F(", micros="));
				Serial.print(micros());
				Serial.print(F(": "));
				SerialStat(intr);
			}
#endif
		
		} // if SCAN or CAD
		
		// else, S_RX of S_TX for example
		else {
			yield();
		} // else S_RX or S_TX, TXDONE
		
	}// intr==0 && _hop


	doneTime = micros();						// We need CDDONE or other intr to reset timeout

	  
	// This is the actual state machine of the gateway
	// and its next actions are depending on the state we are in.
	// For hop situations we do not get interrupts, so we have to
	// simulate and generate events ourselves.
	//
	switch (_state) 
	{
	  // --------------------------------------------------------------
	  // If the state is init, we are starting up.
	  // The initLoraModem() function is already called in setup();
	  //
	  case S_INIT:
#if DUSB>=1
		if (( debug>=1 ) && ( pdebug & P_PRE )) { 
			Serial.println(F("S_INIT")); 
		}
#endif
		// new state, needed to startup the radio (to S_SCAN)
		writeRegister(REG_IRQ_FLAGS, (uint8_t) 0xFF );		// Clear ALL interrupts
		writeRegister(REG_IRQ_FLAGS_MASK, (uint8_t) 0x00 );		// Clear ALL interrupts
		_event=0;
	  break;

	  
	  // --------------------------------------------------------------
	  // In S_SCAN we measure a high RSSI this means that there (probably) is a message
	  // coming in at that freq. But not necessarily on the current SF.
	  // If so find the right SF with CDDETD. 
	  //
	  case S_SCAN:
	    //
		// Intr==IRQ_LORA_CDDETD_MASK
		// We detected a message on this frequency and SF when scanning
		// We clear both CDDETD and swich to reading state to read the message
		//
		if (intr & IRQ_LORA_CDDETD_MASK) {

			_state = S_RX;								// Set state to receiving
			opmode(OPMODE_RX_SINGLE);					// set reg 0x01 to 0x06

			// Set RXDONE interrupt to dio0, RXTOUT to dio1
			writeRegister(REG_DIO_MAPPING_1, (
				MAP_DIO0_LORA_RXDONE | 
				MAP_DIO1_LORA_RXTOUT | 
				MAP_DIO2_LORA_NOP 	| 
				MAP_DIO3_LORA_CRC));
			
			// Since new state is S_RX, accept no interrupts except RXDONE or RXTOUT
			// HEADER and CRCERR
			writeRegister(REG_IRQ_FLAGS_MASK, (uint8_t) ~(
				IRQ_LORA_RXDONE_MASK | 
				IRQ_LORA_RXTOUT_MASK | 
				IRQ_LORA_HEADER_MASK | 
				IRQ_LORA_CRCERR_MASK));
				
			writeRegister(REG_IRQ_FLAGS, (uint8_t) 0xFF );		// reset all interrupt flags
			
			// Starting with version 5.0.1 the waittime is dependent on the SF
			// So for SF12 we wait longer (2^7 == 128 uSec) and for SF7 4 uSec.
			//delayMicroseconds( (0x01 << ((uint8_t)sf - 5 )) );
			delayMicroseconds( RSSI_WAIT );				// Wait some microseconds less
			
			rssi = readRegister(REG_RSSI);				// Read the RSSI
			_rssi = rssi;								// Read the RSSI in the state variable

			_event = 0;									// Make 0, as soon as we have an interrupt
// XXX IRQ writeregister was here May 11
			detTime = micros();
			
#if DUSB>=1
			if (( debug>=1 ) && (  pdebug & P_SCAN )) {
				Serial.print(F("SCAN:: "));
				SerialStat(intr);
			}
#endif
		}//if

		// CDDONE
		// We received a CDDONE int telling us that we received a message on this
		// frequency and possibly on one of its SF.
		// If so, we switch to CAD state where we will wait for CDDETD event.
		//
		else if (intr & IRQ_LORA_CDDONE_MASK) {

			opmode(OPMODE_CAD);
			rssi = readRegister(REG_RSSI);				// Read the RSSI

#if DUSB>=1
			if (( debug>=2 ) && ( pdebug & P_SCAN )) {
				Serial.print(F("SCAN:: f="));
				Serial.print(ifreq);
				Serial.print(F(", sf="));
				Serial.print(sf);
				Serial.print(F("CDDONE: "));
				SerialStat(intr);
			}
#endif
			// We choose the generic RSSI as a sorting mechanism for packages/messages
			// The pRSSI (package RSSI) is calculated upon successful reception of message
			// So we expect that this value makes little sense for the moment with CDDONE.
			// Set the rssi as low as the noise floor. Lower values are not recognized then.
			// Every cycle starts with ifreq==0 and sf=SF7 (or the set init SF)
			//
			//if ( rssi > RSSI_LIMIT )					// Is set to 35
			if ( rssi > (RSSI_LIMIT - (_hop * 7)))		// Is set to 35, or 29 for HOP
			{
#if DUSB>=1
				if (( debug>=2 ) && ( pdebug & P_SCAN )) {
					Serial.println(F("SCAN:: -> CAD: "));
					SerialStat(intr);
				}
#endif
				_state = S_CAD;							// promote to next level
				_event=0;
			}
			
			// If the RSSI is not big enough we skip the CDDONE
			// and go back to scanning
			else {
#if DUSB>=1
				if (( debug>=2 ) && ( pdebug & P_SCAN )) {
					Serial.print("SCAN:: rssi=");
					Serial.print(rssi);
					Serial.print(F(": "));
					SerialStat(intr);
				}
#endif
				_state = S_SCAN;
				//_event=1;								// loop() scan until CDDONE
			}

			// Clear the CADDONE flag
			writeRegister(REG_IRQ_FLAGS_MASK, (uint8_t) 0x00);
			writeRegister(REG_IRQ_FLAGS, (uint8_t) 0xFF);
			
		}//SCAN CDDONE 
		
		// So if we are here then we are in S_SCAN and the interrupt is not
		// CDDECT or CDDONE. it is probably soft interrupt _event==1
		// So if _hop we change the frequency and restart the
		// interrupt in order to check for CDONE on other frequencies
		// if _hop we start at the next frequency, hop () sets the sf to SF7.
		// If we are at the end of all frequencies, reset frequencies and sf
		// and go to S_SCAN state.
		//
		// Note:: We should make sure that all frequencies are scanned in a row
		// and when we switch to ifreq==0 we should stop for a while
		// to allow system processing.
		// We should make sure that we enable webserver etc every once in a while.
		// We do this by changing _event to 1 in loop() only for _hop and
		// use _event=0 for non hop.
		//
		else if (intr == 0x00) 
		{
			_event=0;							// XXX 26/12/2017 !!! NEED
		}
		
		// Unkown Interrupt, so we have an error
		//
		else {
#if DUSB>=1
			if (( debug>=0 ) && ( pdebug & P_SCAN )) {
				Serial.print(F("SCAN unknown:: "));
				SerialStat(intr);
			}
#endif
			_state=S_SCAN;
			//_event=1;								// XXX 06/03 loop until interrupt
			writeRegister(REG_IRQ_FLAGS_MASK, (uint8_t) 0x00);
			writeRegister(REG_IRQ_FLAGS, (uint8_t) 0xFF);
		}
		
	  break; // S_SCAN

	  
	  // --------------------------------------------------------------
	  // S_CAD: In CAD mode we scan every SF for high RSSI until we have a DETECT.
	  // Reason is the we received a CADDONE interrupt so we know a message is received
	  // on the frequency but may be on another SF.
	  //
	  // If message is of the right frequency and SF, IRQ_LORA_CDDETD_MSAK interrupt
	  // is raised, indicating that we can start beging reading the message from SPI.
	  //
	  // DIO0 interrupt IRQ_LORA_CDDONE_MASK in state S_CAD==2 means that we might have
	  // a lock on the Freq but not the right SF. So we increase the SF
	  //
	  case S_CAD:

		// Intr=IRQ_LORA_CDDETD_MASK
		// We have to set the sf based on a strong RSSI for this channel
		//
		if (intr & IRQ_LORA_CDDETD_MASK) {

			// Set RXDONE interrupt to dio0, RXTOUT to dio1
			writeRegister(REG_DIO_MAPPING_1, (
				MAP_DIO0_LORA_RXDONE | 
				MAP_DIO1_LORA_RXTOUT | 
				MAP_DIO2_LORA_NOP |
				MAP_DIO3_LORA_CRC ));
			
			// Accept no interrupts except RXDONE or RXTOUT
			_event=0;								// if CDECT, state=S_RX so we wait for intr
			writeRegister(REG_IRQ_FLAGS_MASK, (uint8_t) ~(
				IRQ_LORA_RXDONE_MASK | 
				IRQ_LORA_RXTOUT_MASK |
				IRQ_LORA_HEADER_MASK |
				IRQ_LORA_CRCERR_MASK ));
				
			// Reset all interrupts as soon as possible
			// But listen ONLY to RXDONE and RXTOUT interrupts 
			writeRegister(REG_IRQ_FLAGS, IRQ_LORA_CDDETD_MASK | IRQ_LORA_RXDONE_MASK);
			//writeRegister(REG_IRQ_FLAGS, (uint8_t) 0xFF );		// XXX 180326, reset all CAD Detect interrupt flags
			
			_state = S_RX;								// Set state to start receiving
			opmode(OPMODE_RX_SINGLE);					// set reg 0x01 to 0x06, initiate READ
			
			delayMicroseconds( RSSI_WAIT );				// Wait some microseconds less
			//delayMicroseconds( (0x01 << ((uint8_t)sf - 5 )) );
			rssi = readRegister(REG_RSSI);				// Read the RSSI
			_rssi = rssi;								// Read the RSSI in the state variable

			detTime = micros();
#if DUSB>=1
			if (( debug>=1 ) && ( pdebug & P_CAD )) {
				Serial.print(F("CAD:: "));
				SerialStat(intr);
			}
#endif


		}// CDDETD
		
		// Intr == CADDONE
		// So we scan this SF and if not high enough ... next
		//
		else if (intr & IRQ_LORA_CDDONE_MASK) {
			// If this is not SF12, increment the SF and try again
			// We expect on other SF get CDDETD
			//
			if (((uint8_t)sf) < SF12) {
			
				sf = (sf_t)((uint8_t)sf+1);				// Increment sf
				setRate(sf, 0x04);						// Set SF with CRC==on
				
				// reset interrupt flags for CAD Done
				_event=0;								// XXX 180324, when increasing SF loop, ws 0x00
				writeRegister(REG_IRQ_FLAGS_MASK, (uint8_t) 0x00);	// Reset the interrupt mask
				//writeRegister(REG_IRQ_FLAGS, IRQ_LORA_CDDONE_MASK | IRQ_LORA_CDDETD_MASK);
				writeRegister(REG_IRQ_FLAGS, (uint8_t) 0xFF );	// This will prevent the CDDETD from being read

				opmode(OPMODE_CAD);						// Scanning mode
				
				delayMicroseconds(RSSI_WAIT);
				rssi = readRegister(REG_RSSI);			// Read the RSSI

#if DUSB>=1
				if (( debug>=2 ) && ( pdebug & P_CAD )) {
					Serial.print(F("S_CAD:: CDONE, SF="));
					Serial.println(sf);
				}
#endif
			}

			// If we reach SF12, we should go back to SCAN state
			//
			else {

				// Reset Interrupts
				_event=1;								// reset soft intr, to state machine again
				writeRegister(REG_IRQ_FLAGS_MASK, (uint8_t) 0x00);	// Reset the interrupt mask
				writeRegister(REG_IRQ_FLAGS, (uint8_t) 0xFF );	// or IRQ_LORA_CDDONE_MASK
				
				_state = S_SCAN;						// As soon as we reach SF12 do something
				sf = SF7;
				cadScanner();							// Which will reset SF to SF7

#if DUSB>=1		
				if (( debug>=2 ) && ( pdebug & P_CAD )) {
					Serial.print(F("CAD->SCAN:: "));
					SerialStat(intr);
				}
#endif
			}
		} //CADDONE

		// if this interrupt is not CDECT or CDDONE then probably is 0x00
		// This means _event was set but there was no real interrupt (yet).
		// So we clear _event and wait for next (soft) interrupt.
		// We stay in the CAD state because CDDONE means something is 
		// coming on this frequency so we wait on CDECT.
		//
		else if (intr == 0x00) {
#if DUSB>=0
			if (( debug>=3 ) && ( pdebug & P_CAD )) {
				Serial.println("Err CAD:: intr is 0x00");
			}
#endif
			_event=1;											// Stay in CAD _state until real interrupt
		}
		
		// else we do not recognize the interrupt. We print an error
		// and restart scanning. If hop we even start at ifreq==1
		//
		else {
#if DUSB>=1
			if (( debug>=0) && ( pdebug & P_CAD )) { 
				Serial.print(F("Err CAD: Unknown::")); 
				SerialStat(intr);
			}
#endif
			_state = S_SCAN;
			sf = SF7;
			cadScanner();										// Scan and set SF7
			
			// Reset Interrupts
			_event=1;											// If unknown interrupt, restarts
			writeRegister(REG_IRQ_FLAGS_MASK, (uint8_t) 0x00);	// Reset the interrupt mask
			writeRegister(REG_IRQ_FLAGS, (uint8_t) 0xFF);		// Reset all interrupts

		}
	  break; //S_CAD
	  
	  // --------------------------------------------------------------
	  // If we receive an RXDONE interrupt on dio0 with state==S_RX
	  // 	So we should handle the received message
	  // Else if it is RXTOUT interrupt
	  //	So we handle this
	  // Else
	  //	Go back to SCAN
	  //
	  case S_RX:
		
		if (intr & IRQ_LORA_RXDONE_MASK) {
		
#if CRCCHECK==1
			// We have to check for CRC error which will be visible AFTER RXDONE is set.
			// CRC errors might indicate that the reception is not OK.
			// Could be CRC error or message too large.
			// CRC error checking requires DIO3
			//
			if (intr & IRQ_LORA_CRCERR_MASK) {
#if DUSB>=1
				if (( debug>=0 ) && ( pdebug & P_RX )) {
					Serial.print(F("Rx CRC err: "));
					SerialStat(intr);
				}
#endif
				if (_cad) {
					sf = SF7;
					_state = S_SCAN;
					cadScanner();
				}
				else {
					_state = S_RX;
					rxLoraModem();
				}

				// Reset interrupts
				_event=0;											// CRC error
				writeRegister(REG_IRQ_FLAGS_MASK, (uint8_t) 0x00);	// Reset the interrupt mask
				writeRegister(REG_IRQ_FLAGS, (uint8_t)(
					IRQ_LORA_RXDONE_MASK | 
					IRQ_LORA_RXTOUT_MASK | 
					IRQ_LORA_HEADER_MASK | 
					IRQ_LORA_CRCERR_MASK ));

				break;
			}// RX-CRC
#endif // CRCCHECK
			
			// If we are here, no CRC error occurred, start timer
#if DUSB>=1
			unsigned long ffTime = micros();	
#endif			
			// There should not be an error in the message
			LoraUp.payLoad[0]= 0x00;								// Empty the message

			// If receive S_RX error, 
			// - print Error message
			// - Set _state to SCAN
			// - Set _event=1 so that we loop until we have an interrupt
			// - Reset the interrupts
			// - break
			if((LoraUp.payLength = receivePkt(LoraUp.payLoad)) <= 0) {
#if DUSB>=1
				if (( debug>=1 ) && ( pdebug & P_RX )) {
					Serial.println(F("sMachine:: Error S-RX"));
				}
#endif
				_event=1;
				writeRegister(REG_IRQ_FLAGS_MASK, (uint8_t) 0x00);	// Reset the interrupt mask
				//writeRegister(REG_IRQ_FLAGS, (uint8_t)(
				//	IRQ_LORA_RXDONE_MASK | 
				//	IRQ_LORA_RXTOUT_MASK | 
				//	IRQ_LORA_HEADER_MASK | 
				//	IRQ_LORA_CRCERR_MASK ));
				writeRegister(REG_IRQ_FLAGS, (uint8_t) 0xFF);
				
				_state = S_SCAN;
				break;
			}
#if DUSB>=1
			if (( debug>=1 ) && ( pdebug & P_RX )) {
				Serial.print(F("RXDONE in dT="));
				Serial.print(ffTime - detTime);
				Serial.print(F(": "));
				SerialStat(intr);
			}
#endif
				
			// Do all register processing in this section
			uint8_t value = readRegister(REG_PKT_SNR_VALUE);	// 0x19; 
			if ( value & 0x80 ) {								// The SNR sign bit is 1
				
				value = ( ( ~value + 1 ) & 0xFF ) >> 2;			// Invert and divide by 4
				LoraUp.snr = -value;
			}
			else {
				// Divide by 4
				LoraUp.snr = ( value & 0xFF ) >> 2;
			}

			// Packet RSSI
			LoraUp.prssi = readRegister(REG_PKT_RSSI);			// read register 0x1A, packet rssi
    
			// Correction of RSSI value based on chip used.	
			if (sx1272) {										// Is it a sx1272 radio?
				LoraUp.rssicorr = 139;
			} else {											// Probably SX1276 or RFM95
				LoraUp.rssicorr = 157;
			}
				
			LoraUp.sf = readRegister(REG_MODEM_CONFIG2) >> 4;

			// If read was successful, read the package from the LoRa bus
			//
			if (receivePacket() <= 0) {							// read is not successful
#if DUSB>=1
				if (( debug>=0 ) && ( pdebug & P_RX )) {
					Serial.println(F("sMach:: Error receivePacket"));
				}
#endif
			}
			
			// Set the modem to receiving BEFORE going back to user space.
			// 
			if ((_cad) || (_hop)) {
				_state = S_SCAN;
				sf = SF7;
				cadScanner();
			}
			else {
				_state = S_RX;
				rxLoraModem();
			}
			
			writeRegister(REG_IRQ_FLAGS_MASK, (uint8_t) 0x00);
			writeRegister(REG_IRQ_FLAGS, (uint8_t) 0xFF);		// Reset the interrupt mask
			eventTime=micros();				//There was an event for receive
			_event=0;
		}// RXDONE
		
		// RX TIMEOUT: We did receive message receive timeout
		//
		else if (intr & IRQ_LORA_RXTOUT_MASK) {
			
			// Make sure we reset all interrupts//
			_event=0;												// Is set by interrupt handlers
			writeRegister(REG_IRQ_FLAGS_MASK, (uint8_t) 0x00 );
			writeRegister(REG_IRQ_FLAGS, (uint8_t) 0xFF);			// reset all interrupts
			
			// For the modem in cad state we reset to SF7
			// If a timeout occurs here we reset the cadscanner
			//
			
			if ((_cad) || (_hop)) {									// XXX 01/01/2018 
				// Set the state to CAD scanning

#if DUSB>=1
				if (( debug>=1 ) && ( pdebug & P_RX )) {
					Serial.print(F("RXTOUT:: f="));
					Serial.print(ifreq);
					Serial.print(F(", sf="));
					Serial.print(sf);
					Serial.print(F(", dT="));
					Serial.print(micros() - detTime);
					Serial.print(F(": "));
					SerialStat(intr);
				}
#endif

				sf = SF7;
				cadScanner();								// Start the scanner after RXTOUT
				_state = S_SCAN;				// New state is scan after RXTOUT

			}// RXTOUT
			
			// If not in cad mode we are in single channel single sf mode.
			//
			else {
				_state = S_RX;								// Receive when interrupted
				rxLoraModem();
			}
			eventTime=micros();				//There was an event for receive
		}
		
		else if (intr & IRQ_LORA_HEADER_MASK) {
			// This interrupt means we received an header successfully
			// which is normall an indication of RXDONE
			//writeRegister(REG_IRQ_FLAGS, IRQ_LORA_HEADER_MASK);
#if DUSB>=1
			if (( debug>=2 ) && ( pdebug & P_RX )) {
				Serial.print(F("RX HEADER:: "));
				SerialStat(intr);
			}
#endif
			//_event=1;
		}

		// If we did not receive a hard interrupt
		// The probably do not do anythings, because in the S_RX
		// state there e always comes a RXTOUT or RXDONE interrupt
		//
		else if (intr == 0x00) {
#if DUSB>=1
			if (( debug>=2) && ( pdebug & P_RX )) {
				Serial.print(F("S_RX noINTR:: "));
				SerialStat(intr);
			}
#endif
		}
		
		// The interrupt received is not RXDONE, RXTOUT or HEADER
		// therefore we wait. Make sure to clear the interrupt
		// as HEADER interrupt comes just before RXDONE
		else {							
#if DUSB>=1
			if (( debug>=0 ) && ( pdebug & P_RX )) {
				Serial.print(F("S_RX:: no RXDONE, RXTOUT, HEADER:: "));
				SerialStat(intr);
			}
#endif
			//writeRegister(REG_IRQ_FLAGS_MASK, (uint8_t) 0x00 );
			//writeRegister(REG_IRQ_FLAGS, (uint8_t) 0xFF);
		}// int not RXDONE or RXTOUT
		
	  break; // S_RX

	  
	  // --------------------------------------------------------------  
	  // Start te transmission of a message in state S-TX
	  // We use S-TXDONE as the state to read the message.
	  // This is not an interrupt state, we use this state to start transmission
	  // the interrupt TX-DONE tells us that the transmission was successful.
	  // It therefore is no use to set _event==1 as transmission might
	  // not be finished in the next loop iteration
	  //
	  case S_TX:
	  
		if (intr == 0x00) {
#if DUSB>=1
			Serial.println(F("TX:00"));
			writeRegister(REG_IRQ_FLAGS, (uint8_t) 0xFF);				// reset interrupt flags
			_event=0;
			return;
#endif
		}
		
		// Sset state to transmit
		_state = S_TXDONE;
		writeRegister(REG_IRQ_FLAGS_MASK, (uint8_t) 0x00);
		writeRegister(REG_IRQ_FLAGS, (uint8_t) 0xFF);					// reset interrupt flags
		
	  	// Initiate the transmission of the buffer (in Interrupt space)
		// We react on ALL interrupts if we are in TX state.
		txLoraModem(
			LoraDown.payLoad,
			LoraDown.payLength,
			LoraDown.tmst,
			LoraDown.sfTx,
			LoraDown.powe,
			LoraDown.fff,
			LoraDown.crc,
			LoraDown.iiq
		);
		
#if DUSB>=2
		if ( debug>=0 ) { 
			Serial.println(F("S_TX, ")); 
			SerialStat(intr);
		}
#endif

		_event=1;													// Or remove the break below
		
	  break; // S_TX

	  
	  // ---------------------------------------------------
	  // AFter the transmission is completed by the hardware, 
	  // the interrupt TXDONE is raised telling us that the tranmission
	  // was successful.
	  // If we receive an interrupt on dio0 _state==S_TX it is a TxDone interrupt
	  // Do nothing with the interrupt, it is just an indication.
	  // sendPacket switch back to scanner mode after transmission finished OK
	  //
	  case S_TXDONE:
		if (intr & IRQ_LORA_TXDONE_MASK) {
#if DUSB>=1
			if (( debug>=0 ) && ( pdebug & P_TX )) {
				Serial.println(F("TXDONE interrupt"));
			}
#endif
			// After transmission reset to receiver
			if ((_cad) || (_hop)) {									// XXX 26/02
				// Set the state to CAD scanning
				_state = S_SCAN;
				sf = SF7;
				cadScanner();										// Start the scanner after TX cycle
			}
			else {
				_state = S_RX;
				rxLoraModem();		
			}
			
			_event=0;
			writeRegister(REG_IRQ_FLAGS_MASK, (uint8_t) 0x00);
			writeRegister(REG_IRQ_FLAGS, (uint8_t) 0xFF);			// reset interrupt flags
#if DUSB>=1
			if (debug>=1) {
				Serial.println(F("TXDONE handled"));
				if (debug>=2) Serial.flush();
			}
#endif
		}
		
		// If a soft _event==0 interrupt and no transmission finished:
		else {
#if DUSB>=1
			if (( debug>=0 ) && ( pdebug & P_TX )) {
				Serial.print(F("TXDONE unknown int="));
				SerialStat(intr);
			}
#endif
			writeRegister(REG_IRQ_FLAGS_MASK, (uint8_t) 0x00);
			writeRegister(REG_IRQ_FLAGS, (uint8_t) 0xFF);		// reset interrupt flags
			_event=0;
			_state=S_SCAN;
		}
		
	  break; // S_TXDONE	  

	  
	  // --------------------------------------------------------------
	  // If _STATE is in undefined state
	  // If such a thing happens, we should re-init the interface and 
	  // make sure that we pick up next interrupt
	  default:
#if DUSB>=1
		if (debug >= 0) { 
			Serial.print("ERR state="); 
			Serial.println(_state);	
		}
#endif
		if ((_cad) || (_hop)) {
#if DUSB>=1
			if (debug>=0) {
				Serial.println(F("default:: Unknown _state "));
				SerialStat(intr);
			}
#endif
			_state = S_SCAN;
			sf = SF7;
			cadScanner();
			_event=1;									// XXX Restart the state machine
		}
		else											// Single channel AND single SF
		{
			_state = S_RX;
			rxLoraModem();
			_event=0;
		}
		writeRegister(REG_IRQ_FLAGS_MASK, (uint8_t) 0x00);
		writeRegister(REG_IRQ_FLAGS, (uint8_t) 0xFF);	// Reset all interrupts
		eventTime=micros();							// Reset event for unkonwn state
		
	  break;// default
	}// switch(_state)
	
	return;
}
