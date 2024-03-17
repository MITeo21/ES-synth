#ifndef MAIN_H
#define MAIN_H

#include <Arduino.h>
#include <U8g2lib.h>
#include <STM32FreeRTOS.h>
#include <ES_CAN.h>
#include <bitset>
#include <vector>
#include <cstring>

#include "state_machines.h"


//Constants
const uint32_t interval = 100; //Display update interval
const int sampleFreq = 22000; //Sample rate of device
const int maxRecordingLength = 100; //Number of events per recording
const int noteMultiplierNumerator = 30403886; //note shift helper
const int noteMultiplierDenominator = 28697447;
const char keys[12] = {
  'c', 'C', 'd', 'D', 'e', 'f', 'F', 'g', 'G', 'a', 'A', 'b'
};
const std::array<char, 12> base_keys = {  // TODO: currently unused
 '-', '-', '-', '-', '-', '-', '-', '-', '-', '-', '-', '-'
};
const uint32_t stepSizes [] = {
  51076057, 54113197, 57330935, 60740010, 64351799, 68178356, 
  72232452, 76527617, 81078186, 85899346, 91007187, 96418756
};
const char* toneNames [] = {"saw", "sqr", "tri", "sin"}; //TODO: REMOVE MET ONCE TESTED
const uint32_t knobMaxes[4] = {
  8, (sizeof(toneNames)/sizeof(toneNames[0]))-1, 5, 4
};

//System state variable arrays
struct {
  volatile char keyStrings[12] = {
    '-', '-', '-', '-', '-', '-', '-', '-', '-', '-', '-', '-'
  };
  
  // K0 K1 K2 K3
  volatile uint32_t knobValues[4] = {
    4, 0, knobMaxes[2], knobMaxes[3]
  }; 
  volatile bool knobPushes[4] = {0};  // VOL TONE SETTING ECHO
  
  volatile uint8_t octave = 4;
  volatile int32_t phase_accumulators[96] = {0};
  volatile bool keys_down[96] = {0};

  volatile short joystick_pos[2] = {0};
  // ^ TODO: is this being used?

  volatile uint8_t TX_Message[8] = {0};
  volatile uint8_t RX_Message[8] = {0};
  volatile bool isSender = false;

  volatile char playbackState = 'N'; // Neutral Recording Playback
  volatile uint32_t playbackAccumulator = 0;

  volatile uint8_t menuState = 0;  // 0 is met, 1 is playback, 2 is oct
  volatile bool isSelected = false;
  volatile uint8_t met = 120;
  volatile bool metMenuState = false;  //false is met slider, true is ON/OFF
  volatile bool metOnState = false;
  volatile uint8_t dotLocation[2] = {58, 4};
  SemaphoreHandle_t mutex;

  ButtonPress::State next_state(char button, const bool &isPressed){
    static ButtonPress joystickPush{'j'};  // 0th button
    switch (button){
      case 'j':
        return joystickPush.nextState(isPressed);
      case 'v':
        // implement volume button press
        return ButtonPress::OFF;
      default:
        // hopefully shouldn't get this
        return ButtonPress::OFF;
    }
  }
} sysState;

QueueHandle_t msgInQ;
QueueHandle_t msgOutQ;
SemaphoreHandle_t CAN_TX_Semaphore;

// an enum containing all the pin definitions, 
// since they're all just integers anyways
enum PinDefinitions{
  //Row select and enable
  RA0_PIN = D3,
  RA1_PIN = D6,
  RA2_PIN = D12,
  REN_PIN = A5,

  //Matrix input and output
  C0_PIN = A2,
  C1_PIN = D9,
  C2_PIN = A6,
  C3_PIN = D1,
  OUT_PIN = D11,

  //Audio analogue out
  OUTL_PIN = A4,
  OUTR_PIN = A3,

  //Joystick analogue in
  JOYY_PIN = A0,
  JOYX_PIN = A1,

  //Output multiplexer bits
  DEN_BIT = 3,
  DRST_BIT = 4,
  HKOW_BIT = 5,
  HKOE_BIT = 6,
};

//Display driver object
U8G2_SSD1305_128X32_ADAFRUIT_F_HW_I2C u8g2(U8G2_R0);

// - - - - - - - - - - - - - - READING INPUTS - - - - - - - - - - - - - - 

// reads state of columns: detects if a key in that column is depressed
std::bitset<4> readCols(void);

// sets the row to be read
void setRow(uint8_t row);

// higher order function to set a row and then read the key presses for that row
std::bitset<4> readRow(uint8_t row);

// an even higher order function to read all key presses
std::bitset<32> readKeys(void);

// reads the state of the knobs
std::bitset<12> readKnobs(void);

// calculates the direction of the joystick (goes from 0 to 1024)
char calcJoy(short x, short y, short p);

// navigates the cursor through the menu based on the joystick's direction
void navigate(char direction);

// - - - - - - - - - - - - - - TIMED TASKS - - - - - - - - - - - - - - 

// reads keys and updates all relevant system components
// including informing any connected devices
void updateKeysTask(void * pvParameters);

// refreshes display with updated information
void updateDisplayTask(void * pvParameters);

// - - - - - - - - - - - - - - - - - CAN TASKS - - - - - - - - - - - - - - - - -

// ISR to store incoming CAN RX messages
void CAN_RX_ISR (void);

// decodes queued incoming CAN messages and updates system state
void decodeMessageTask(void * pvParameters);

// ISR to give the semaphore once mailbox available
void CAN_TX_ISR (void);

// Sends queued messages to CAN bus
void CAN_TX_Task (void * pvParameters);

// - - - - - - - - - - - - - - - - - NOISE GEN - - - - - - - - - - - - - - - - -

// it plays the metronome...
// holds an internal metronome counter
int32_t playMetronome();

// for playing sampled tones, like sine waves
// uses internal array of phase counters
int32_t playSampled(uint32_t tone, uint8_t oct, uint8_t note, int idx);

// Increments the requested note's phase accumulator, based on:
// the octave, note index, and the tone type
int32_t playFunction(uint32_t tone, uint8_t oct, uint8_t note, int idx);

// small helper function to detect overflow and clip
int32_t inline jack_the_clipper(int32_t Vout, const uint32_t &vol);

// takes in the desired volume and tone, then sums the outputs from all notes
// via a call to playNote for each pressed note
int32_t playNotes(const uint32_t &tone, const uint32_t &vol);

// Interrupt Service Routine for sound output
void sampleISR(void);

// Function to set outputs using key matrix for display initialisation
void setOutMuxBit(const uint8_t bitIdx, const bool value);

// setup and loop don't need to go here

#endif