/**
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2014 Kyle J. Temkin <ktemkin@binghamton.edu>
 * Copyright (c) 2014 Binghamton University
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include <avr/interrupt.h>
#include <stdbool.h>
#include <stdlib.h>

#include "usb_serial/usb_serial.h"
#include "frequency.h"

#include "state.h"

#include "lights.h"
#include "ir_comm.h"
#include "pc_comm.h"

#include "main.h"

/**
 * The relative brightnesses for a bright and dim beacon LED,
 * in terms of percent duty cycle.
 */
static const uint8_t Bright = 100;
static const uint8_t Dim = 3;

/**
 * Stores the current state for the board.
 * See state.h for more information.
 */
volatile static BoardState beacon = {.mode = MODE_OFF, .owner = OwnerNone};

/**
 * Stores a "claim code", which is the code that needs to be transmitted to
 * claim the beacon. This should be populated with a random number once the
 * beacon is assigned an ID.
 */
volatile static uint8_t claim_code = 0;


/**
 * Stores the most recent attempt at a beacon claim which has not been
 * reported back to the PC, or -1 if no such claim exists.
 */ 
volatile static uint16_t last_claim_attempt = -1;

/**
 * Constant which specifies the maximum allowed amount of bit errors
 * before the beacon can be claimed. If this is set to 8, the beacon
 * will always be claimed when IR is received.
 */
static const uint8_t maximum_allowed_errors = 0;


/**
 * Main beacon control routines.
 */
int main() {

  //Set up the board's peripherals...
  set_up_hardware();

  //Start listening for data from the competing robots.
  //In this case, receipt of any signals will trigger the "handle IR receive" function.
  register_receive_handler(handle_IR_receive);

  //Register a function which will be called on a framing error.
  //This is used for diagnostic purposes.
  register_frame_error_handler(handle_IR_frame_error);

  //Registers the function that will determine the value to transmit during any IR
  //exchange.
  register_transmit_provider(value_to_transmit);

  //Handle communications with the host PC forever.
  //Note that all other functions are interrupt driven;
  //so PC communication is both the lowest priority and
  //the only task running in the "main loop".
  while(1) {
    handle_pc_comm();
  }

  //This code is unreachable, but avr-gcc throws a
  //warning if we don't return an integer.
  return 0;

}


/**
 * Sets up the board's peripherals and communications channels.
 */
void set_up_hardware() {

  //Ensure we're running at 16MHz.
  CPU_PRESCALE(CPU_16MHz);

  //Ensure that the unused pins aren't left floating,
  //which can cause them to switch inappropriately, drawing
  //unncessary amounts of power.
  pull_up_unused_pins();

  //Set up all of the peripherals...
  set_up_lights();
  set_up_ir_comm();

  //Set up the PC connection
  connect_to_pc();

  //And enable interrupts, starting the main device functions.
  sei();
}

/**
 * Configures the AVR's unused I/O pins to inputs with pull-up
 * resistors, to save power. This isn't an optimal configuration
 * (it would be better to use external pull-ups, or set these up
 *  as outputs), but it reduces the amount of soldering inherent
 *  to the first method, and reduces the risk of a short inherent
 *  to the second.
 */ 
void pull_up_unused_pins() {

  //Specify which of the values correspond to unused ports;
  //these will be configured as inputs with internal pull-ups.
  const uint8_t port_f_unused = 0b11111111;
  const uint8_t port_e_unused = 0b11111111;
  const uint8_t port_d_unused = 0b10110011;
  const uint8_t port_b_unused = 0b10001110;

 
  //Apply the port values themselves.
  DDRF  &= ~port_f_unused;
  PORTF |=  port_f_unused;

  DDRE  &= ~port_e_unused;
  PORTE &=  port_e_unused;

  DDRD  &= ~port_d_unused;
  PORTD &=  port_d_unused;

  DDRB  &= ~port_b_unused;
  PORTB &=  port_b_unused;
}


/**
 * Sets up communications with the host PC.
 *
 * If the beacon board isn't connected to a PC, this will
 * "hang" here forever. In the future, PC-less single-beacon
 * operation may be desirable, in which case this function
 * will have to be reworked.
 */
inline void connect_to_pc() {

  //Initialize USB communications...
  usb_init();

  //... and wait for the USB port to be configured.
  while(!usb_configured());

}


/**
 * Handles all requests (and commands) recieved from the PC.
 */
void handle_pc_comm() {

  BoardState new_state;

  //Wait until we have a request...
  while(!usb_serial_available());

  //Receive the new board state.
  new_state = receive_state_from_pc();

  //Perform an action based on the request given.
  switch(new_state.mode) 
  {

    //If the PC is requesting the most recent claim,
    //responsd with the most recent claim code; then
    //invalidate any pending claims.
    case REQUEST_LAST_CLAIM:
      send_most_recent_claim_attempt();
      break;

    //If the PC is requesting the current claim code, send it.
    case REQUEST_CLAIM_CODE:
      send_byte_to_pc(claim_code);
      break;

    //If the beacon is requesting an update,
    //transmit one.
    case REQUEST_UPDATE:
      send_state_to_pc(beacon);
      break;

    //If we weren't sent a request state, 
    //update the internal state, and transmit 
    //the state back to the PC as a primitive
    //acknowledgement.
    default:
      apply_state(new_state);
      send_state_to_pc(beacon);
      break;
  
  }

}

/**
 * Transmits the most recent claim attempt to the PC.
 * This invalidates any existing claim attempt.
 */
void send_most_recent_claim_attempt() {

  //Ensure the following block is run "atomically":
  //that is, ensure that no interrupts occur during
  //transmission of the claim attempt.
  ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
    send_word_to_pc(last_claim_attempt);
    last_claim_attempt = no_new_claim_code;
  }

}

/**
 * Applies the provided "beacon state" object to the
 * board, replacing the current state, and updating all peripherals.
 */
void apply_state(BoardState new_state) {

  //Apply the new state itself...
  beacon = new_state;

  //Seed the internal random number generator using
  //the current value of Timer 1 (which is run by the light
  //module). This allows us to use the PC communications erratic
  //timings as a source of pseudo-randomness.
  srand(TCNT1);

  //And apply the state's effects.
  enforce_state();
}


/**
 * Enforces the current beacon board state, which determines the
 * current state of the beacon LEDs and IR comm.
 */
void enforce_state() {

  turn_off_lights();

  // If the beacon has an invalid ID, turn off all peripherals
  // and wait to be assigned an ID.
  if(beacon_is_disabled()) {
    return;
  }

  // Set the color of the beacon's light according to which team
  // owns the beacon; or use white if no one owns the beacon.
  switch(beacon.owner) {

    case OwnerGreen:
      turn_on_light(Green);
      break;

    case OwnerRed:
      turn_on_light(Red);
      break;

    default:
      turn_on_light(White);
      break;

  }

  // If the beacon can be claimed, ensure the beacon's lights
  // is running at full brightness, and the IR channel is on.
  // Otherwise, dim the light so it's a less attractive target,
  // and disable IR transmission.
  if(beacon_can_be_claimed()) {
    set_light_brightness(Bright);
    start_transmitting_claim_code();
    ir_enable_receive();
  } else {
    set_light_brightness(Dim);
    ir_stop_transmitting();
    ir_disable_receive();
  }
}

/**
 * Starts the repeated transmission of a "claim code", a code which is
 * transmitted to the competing robots. If a robot is able to respond
 * with a modification of this code, it can claim the beacon.
 */
void start_transmitting_claim_code() {
  claim_code = rand();
  ir_start_continuously_transmitting(claim_code);
}

/**
 * Returns true iff this beacon can be claimed;
 * that is, if it isn't owned by the current team
 * (and the beacon isn't in a frozen mode.)
 */
bool beacon_can_be_claimed() {

  //Determine if the beacon is already claimed...
  uint8_t beacon_already_owned = ((uint8_t)beacon.owner == (uint8_t)beacon.affiliation);

  //.. and determine if the beacon is "frozen", and thus unable to be claimed.
  uint8_t beacon_is_frozen = (beacon.mode == MODE_FROZEN);

  //The beacon should be claimable if it's _not_ owned by the affiliated team,
  //and isn't in the frozen state.
  return !beacon_is_frozen && !beacon_already_owned;

}

/**
 * Returns true iff the given beacon is _disabled_,
 * and thus not being used in this round.
 */
bool beacon_is_disabled() {
  return beacon.mode == MODE_OFF || beacon.mode == MODE_ERROR;
}


/**
 * Returns the hamming distance (number of bit errors)
 * between two given bytes.
 */
uint8_t hamming_distance(uint8_t a, uint8_t b) {

  unsigned char distance = 0;

  //For each of the bits in an eight-bit number...
  for(uint16_t i = 1; i < 0x100; i = i << 1) {

      //If the given bit is different for both A and B,
      //increase the hamming distance.
      if((a & i) != (b & i)) {
        ++distance; 
      }
  }

  //... return the computed hamming distance.
  return distance;
}


/**
 * Returns true iff the provided "response code"
 * matches the transmitted "claim code". If this is
 *
 */
bool is_valid_response_code(uint8_t code) {

  //In this case, the cast is necessary, as avr-gcc
  //will promote claim_code to an integer _before_ its bitwise
  //not operation. We need it to be a uint8_t afterwards, so
  //the comparison is performed correctly!
  return hamming_distance(code, (uint8_t)~claim_code) <= maximum_allowed_errors;

}


/**
 * Functions which determines the value that should be transmitted
 * over IR. This is called roughly once per second by the IR module.
 */ 
uint8_t value_to_transmit() {

  //Determine a new, psuedo-random value to transmit.
  claim_code = rand();

  //And transmit that value.
  return claim_code;

}


/**
 * Function which handles the receipt of an IR value from
 * the competing robot. This function is called from within
 * an interrupt, and thus is assumed to be uninterruptable.
 */
void handle_IR_receive(uint8_t value) {

  //If the beacon is disabled, ignore all received IR data.
  if(beacon_is_disabled()) {
    return;
  }

  //Store the most recent claim attempt.
  last_claim_attempt = value;

  //If we've recieved a valid response code,
  //change this becaon's owner to match the claiming robot.
  if(is_valid_response_code(value)) {
    beacon.owner = beacon.affiliation;
  } 
  //Otherwise, disable the receiver until after the next
  //transmission is complete. This prevents contestants
  //from "spamming" the robot.
  else {
    ir_disable_receive_until_transmit_complete();
  }

  //Apply the beacon's state.
  apply_state(beacon);

}

/**
 * Function which handles reciept of an improperly framed
 * IR value. This is mostly used for diagnostic purposes.
 */ 
void handle_IR_frame_error(uint8_t value) {
  last_claim_attempt = misframed_claim_code;
}
