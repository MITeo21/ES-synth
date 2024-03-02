#include <Arduino.h>
#include <U8g2lib.h>
#include <STM32FreeRTOS.h>
#include <bitset>

//Constants
  const uint32_t interval = 100; //Display update interval
  const char keys[12] = {'c', 'C', 'd', 'D', 'e', 'f', 'F', 'g', 'G', 'a', 'A', 'b'};
  const uint32_t stepSizes [] = {51076057, 54113197, 57330935, 60740010, 64351799, 68178356, 72232452, 76527617, 81078186, 85899346, 91007187, 96418756};
  const uint32_t knobMaxes[4] = {8,3,5,4};
//System state variable arrays
struct {
  volatile uint32_t keyValues[12] = {0,0,0,0,0,0,0,0,0,0,0,0};
  volatile char keyStrings[12] = {'-','-','-','-','-','-','-','-','-','-','-','-'};
  volatile uint32_t knobValues[4] = {knobMaxes[0],knobMaxes[1],knobMaxes[2],knobMaxes[3]}; // K0 K1 K2 K3
  volatile bool knobPushes[4] = {0,0,0,0};
  SemaphoreHandle_t mutex;
} sysState;
//Pin definitions
  //Row select and enable
  const int RA0_PIN = D3;
  const int RA1_PIN = D6;
  const int RA2_PIN = D12;
  const int REN_PIN = A5;

  //Matrix input and output
  const int C0_PIN = A2;
  const int C1_PIN = D9;
  const int C2_PIN = A6;
  const int C3_PIN = D1;
  const int OUT_PIN = D11;

  //Audio analogue out
  const int OUTL_PIN = A4;
  const int OUTR_PIN = A3;

  //Joystick analogue in
  const int JOYY_PIN = A0;
  const int JOYX_PIN = A1;

  //Output multiplexer bits
  const int DEN_BIT = 3;
  const int DRST_BIT = 4;
  const int HKOW_BIT = 5;
  const int HKOE_BIT = 6;

//Display driver object
U8G2_SSD1305_128X32_ADAFRUIT_F_HW_I2C u8g2(U8G2_R0);

//Big Endian
std::bitset<4> readCols(){
  std::bitset<4> result;
  result[3] = digitalRead(C3_PIN);
  result[2] = digitalRead(C2_PIN);
  result[1] = digitalRead(C1_PIN);
  result[0] = digitalRead(C0_PIN);
  return result;
}

void setRow(uint8_t row){
  digitalWrite(REN_PIN, LOW);
  digitalWrite(RA2_PIN, row & 0x04 ? HIGH : LOW);
  digitalWrite(RA1_PIN, row & 0x02 ? HIGH : LOW);
  digitalWrite(RA0_PIN, row & 0x01 ? HIGH : LOW);
  digitalWrite(REN_PIN, HIGH);
}

std::bitset<4> readRow(uint8_t row){
  setRow(row);
  delayMicroseconds(3);
  return readCols();
}

std::bitset<32> readKeys() {
  std::bitset<32> keysDown;
  for (int i = 0; i < 3; i++) {
      std::bitset<4> rowVals = readRow(i);
      for (int j = 0; j < 4; j++) {
          keysDown[(i*4)+j] = rowVals[j];
      }
  }
  return keysDown;
}

// Output in zero-indexed array order {3A 3B 2A 2B 1A 1B 0A 0B 3S 2S 1S 0S}
std::bitset<16> readKnobs(){
  std::bitset<16> knobVals;  
  for (int i = 3; i < 7; i++){
    std::bitset<4> rowVals = readRow(i);
    if ((3<=i)&(i<5)){ //A&B vals
      for (int j = 0; j < 4; j++) {
        knobVals[(i-3)*4+j] = rowVals[j];
      }
    }
    else{ //S vals
      for (int j = 0; j < 2; j++){
        knobVals[8+((i-5)*2)+j] = rowVals[1-j];
      }
    }
  }
  return knobVals;
}

// TIMED TASKS
void updateKeysTask(void * pvParameters) {
  const TickType_t xFrequency = 20/portTICK_PERIOD_MS;
  TickType_t xLastWakeTime = xTaskGetTickCount();
  while (1){
    vTaskDelayUntil( &xLastWakeTime, xFrequency );
    std::bitset<32> keyBools;
    keyBools = readKeys();
    // Store key string locally
    char localKeyStrings[12];
    for (int i = 0; i < 12; i++) {
        if (keyBools[i] == 0) {
            localKeyStrings[i] = keys[i];
        }
        else{
          localKeyStrings[i] = '-';
        }
    }
    // Store step sizes locally
    uint32_t localKeyValues[12];
    for (int i = 0; i < 12; i++) {
      if (keyBools[i] == 0) {
        localKeyValues[i] = stepSizes[i];
      }
      else{
        localKeyValues[i] = 0;
      }
    }
    // Store knob values and push bools
    std::bitset<16> prevKnobBools;
    static std::bitset<16> knobBools = readKnobs();
    prevKnobBools = knobBools;
    knobBools = readKnobs();
    int localKnobDiffs[4] = {0,0,0,0};
    int lastLegalDiff[4] = {0,0,0,0};
    bool localKnobPushes[4];
    for (int i=0; i<4; i++){
      //Knob values in format {B,A}
      std::bitset<2> prevBool, currBool;
      prevBool[1] = prevKnobBools[(2*i)+1];
      prevBool[0] = prevKnobBools[2*i];
      currBool[1] = knobBools[(2*i)+1];
      currBool[0] = knobBools[2*i];
      if(((prevBool == 0b00)and(currBool == 0b01))or((prevBool == 0b11)and(currBool == 0b10))){
        localKnobDiffs[3-i] = (sysState.knobValues[3-i] < knobMaxes[3-i]) ? 1 : 0;
        lastLegalDiff[3-i] = (sysState.knobValues[3-i] < knobMaxes[3-i]) ? 1 : 0;
      }
      else if(((prevBool == 0b01)and(currBool == 0b00))or((prevBool == 0b10)and(currBool == 0b11))){
        localKnobDiffs[3-i] = (sysState.knobValues[3-i] > 0) ? -1 : 0;
        lastLegalDiff[3-i] = (sysState.knobValues[3-i] > 0) ? -1 : 0;
      }
      else if(((prevBool == 0b00)and(currBool == 0b11))or((prevBool == 0b11)and(currBool == 0b00))or((prevBool == 0b01)and(currBool == 0b10))or((prevBool == 0b10)and(currBool == 0b01))){
        //other legal transition
        localKnobDiffs[3-i] = 0;
      }
      else{
        //illegal transition to help w skipping
        localKnobDiffs[3-i] = 5*lastLegalDiff[3-i];
      }
      //Knob pushes
      localKnobPushes[3-i] = !knobBools[i+8];
    }
    //Store to sysState
    xSemaphoreTake(sysState.mutex, portMAX_DELAY);
    // Store key string globally
    for (int i=0; i<12; i++) {
      __atomic_store_n(&sysState.keyStrings[i], localKeyStrings[i], __ATOMIC_RELAXED);
    }
    // Store key values globally
    for (int i=0; i<12; i++) {
      __atomic_store_n(&sysState.keyValues[i], localKeyValues[i], __ATOMIC_RELAXED);
    }
    // Store knob values globally
    for (int i=0; i<4; i++){
      int tmp = sysState.knobValues[i]+localKnobDiffs[i];
      __atomic_store_n(&sysState.knobValues[i], tmp, __ATOMIC_RELAXED);
    }
    // Store knob pushes globally
    for (int i=0; i<4; i++){
      __atomic_store_n(&sysState.knobPushes[i], localKnobPushes[i], __ATOMIC_RELAXED);
    }
    xSemaphoreGive(sysState.mutex);
    //
  }
}

// Refreshes display
void updateDisplayTask(void * pvParameters){
  const TickType_t xFrequency = 100/portTICK_PERIOD_MS;
  TickType_t xLastWakeTime = xTaskGetTickCount();
  while (1){
    u8g2.clearBuffer();         // clear the internal memory
    u8g2.setFont(u8g2_font_ncenB08_tr); // choose a suitable font
    //Header
    u8g2.drawStr(2,10,"UNISYNTH Ltd.");
    //Display key values
    // u8g2.setCursor(2,30);
    // int localKeyValues[12];
    // for (int i = 0; i<12; i++){
    //   localKeyValues[i] = sysState.keyValues[i];
    // }
    // for (int i = 0; i<12; i++){
    //   if (localKeyValues[i] != 0){
    //     u8g2.print(localKeyValues[i]);
    //   }
    // }
    //Display key names
    char localKeyStrings[13];
    localKeyStrings[12] = '\0'; //Termination
    for (int i=0; i<12; i++){
      localKeyStrings[i] = sysState.keyStrings[i];
    }
    u8g2.drawStr(2,20,localKeyStrings);
    //Display knob values
    // int localKnobValues[4];
    // for (int i=0; i<4; i++){
    //   localKnobValues[i] = sysState.knobValues[i];
    // }
    for (int i=0; i<4; i++){
      u8g2.setCursor((2+(35*i)),30);
      u8g2.print(sysState.knobValues[i]);
    }
    //Display knob pushes
    // int localKnobPushes[4];
    // for (int i=0; i<4; i++){
    //   localKnobPushes[i] = sysState.knobPushes[i];
    // }
    // for (int i=0; i<4; i++){
    //   u8g2.setCursor((2+(35*i)),30);
    //   u8g2.print(localKnobPushes[i]);
    // }
    //
    u8g2.sendBuffer();          // transfer internal memory to the display
    digitalToggle(LED_BUILTIN); //Toggle LED for CW requirement
  }
}

// ISR to output sound
void sampleISR() {
  static uint32_t phaseAcc = 0;
  uint32_t phaseAccChange = 0;
  for(int i=0; i<12; i++){
    phaseAccChange += sysState.keyValues[i];
  }
  if (phaseAccChange==0){
    phaseAcc = 0;
  }
  else{
    phaseAcc += phaseAccChange;
  }
  uint32_t volKnobValue;
  __atomic_store_n(&volKnobValue, sysState.knobValues[0], __ATOMIC_RELAXED);
  int32_t Vout = ((phaseAcc >> 24) - 128) >> (8-volKnobValue);
  analogWrite(OUTR_PIN, Vout + 128);
}

//Function to set outputs using key matrix
void setOutMuxBit(const uint8_t bitIdx, const bool value) {
      digitalWrite(REN_PIN,LOW);
      digitalWrite(RA0_PIN, bitIdx & 0x01);
      digitalWrite(RA1_PIN, bitIdx & 0x02);
      digitalWrite(RA2_PIN, bitIdx & 0x04);
      digitalWrite(OUT_PIN,value);
      digitalWrite(REN_PIN,HIGH);
      delayMicroseconds(2);
      digitalWrite(REN_PIN,LOW);
}

void setup() {
  // put your setup code here, to run once:

  //Set pin directions
  pinMode(RA0_PIN, OUTPUT);
  pinMode(RA1_PIN, OUTPUT);
  pinMode(RA2_PIN, OUTPUT);
  pinMode(REN_PIN, OUTPUT);
  pinMode(OUT_PIN, OUTPUT);
  pinMode(OUTL_PIN, OUTPUT);
  pinMode(OUTR_PIN, OUTPUT);
  pinMode(LED_BUILTIN, OUTPUT);

  pinMode(C0_PIN, INPUT);
  pinMode(C1_PIN, INPUT);
  pinMode(C2_PIN, INPUT);
  pinMode(C3_PIN, INPUT);
  pinMode(JOYX_PIN, INPUT);
  pinMode(JOYY_PIN, INPUT);

  //Initialise display
  setOutMuxBit(DRST_BIT, LOW);  //Assert display logic reset
  delayMicroseconds(2);
  setOutMuxBit(DRST_BIT, HIGH);  //Release display logic reset
  u8g2.begin();
  setOutMuxBit(DEN_BIT, HIGH);  //Enable display power supply

  //Initialise UART
  Serial.begin(9600);
  Serial.println("Hello World");

  //Initialise freeRTOS
  TaskHandle_t updateKeysHandle = NULL;
  xTaskCreate(
  updateKeysTask,		/* Function that implements the task */
  "updateKeys",		/* Text name for the task */
  128,      		/* Stack size in words, not bytes */
  NULL,			/* Parameter passed into the task */
  2,			/* Task priority */
  &updateKeysHandle );	/* Pointer to store the task handle */

  TaskHandle_t updateDisplayHandle = NULL;
  xTaskCreate(
  updateDisplayTask,		/* Function that implements the task */
  "updateDisplay",		/* Text name for the task */
  256,      		/* Stack size in words, not bytes */
  NULL,			/* Parameter passed into the task */
  1,			/* Task priority */
  &updateDisplayHandle );	/* Pointer to store the task handle */

  //Initialise hardware timer
  TIM_TypeDef *Instance = TIM1;
  HardwareTimer *sampleTimer = new HardwareTimer(Instance);
  sampleTimer->setOverflow(22000, HERTZ_FORMAT);
  sampleTimer->attachInterrupt(sampleISR);
  sampleTimer->resume();

  //Set up mutexes
  sysState.mutex = xSemaphoreCreateMutex();
  sysState.mutex = xSemaphoreCreateMutex();
  sysState.mutex = xSemaphoreCreateMutex();
  sysState.mutex = xSemaphoreCreateMutex();

  vTaskStartScheduler();
}

void loop() {}