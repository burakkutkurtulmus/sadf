
#include <RF24Network.h> //Library for networking nRF24L01s, using version https://github.com/maniacbug/RF24Network
#include <RF24.h> //Library for nRF24L01, using version https://github.com/maniacbug/RF24
#include <SPI.h> //nRF24L01 uses SPI communication
#include <avr/sleep.h> //Sleep functions
#include <avr/wdt.h> //Watch dog timer functions
#include <EEPROM.h> //EEPROM functions
#include <OneWire.h> //communication library for the DS18S20 temp sensor
#include <DallasTemperature.h> //This library just further abstracts the above library for the DS18S20 temp sensor

RF24 radio(9,10); //create object to control and communicate with nRF24L01
RF24Network network(radio); //Create object to use nRF24L01 in mesh network
int thisNode; //address of this router or end device
const uint16_t rXNode = 00; //address of coordinator
float iREF; //Actual measured voltage value of the internal 1.1V reference
bool batShutdown = false; //tracks if battery is good or dead
bool timer = true; //Used while in router mode to set transmit interval
byte mMode; //used to track whether in end device mode or router mode
byte sInterval; //used to set interval between transmitting data
unsigned long tInterval; //Used for transmit time interval in router mode
unsigned long tStart; //Used for transmit time interval in router mode
int count; //used for tracking sleep interval
byte wDTInterval; //used for tracking sleep interval

//structure used to hold the payload that is sent to the coordinator. 
//Currently setup to send temp value as float, ADC value as int, and battery state as bool
//Can be adjusted to accomodate the exact data you want node to send
//Keep in mind the more data you send the more power is used and the more likely
//there is some kind of data error in the transmit receive process
struct payload_t
{
  float aDCTemp; //temperature from onboard sensor
  int aDCopen;  //open ADC port that returns measurement
  bool batState; //bool to communicate battery power level, true is good and false means battery needs to be replaced
};

void setup(void)
{
  wdt_disable(); //Datasheet recommends disabling WDT right away in case of low probabibliy event
  analogReference(INTERNAL); //set the ADC reference to internal 1.1V reference
  burn8Readings(); //make 8 readings but don't use them to ensure good reading after ADC reference change
  pinMode(7, OUTPUT); //set digital pin 7 as output to control status LED
  checkNodeAddress(); //Function used for adding and getting module settings from EEPROM
  setSleepInterval(sInterval);   //Used to set interval between transmits, based on settings data from EEPROM
  SPI.begin(); //Start SPI communication
  radio.begin(); //start nRF24L01 communication and control
  //setup network communication, first argument is channel which determines frequency band module communicates on. Second argument is address of this module
  network.begin(90, thisNode); 
}

void loop(void)
{ 
  if(!batShutdown) { //check to see if we are in battery shutdown, if so all the module will do is sleep
    network.update(); //check to see if there is any network traffic that needs to be passed on, technically an end device does not need this 
    
    if(timer) { //If in router mode this used to track when it is time to transmit again
      //create packet to transmit. First argument calculates temperature, second gets ADC reading from A2, third gives state of battery
      payload_t payload = { convertCtoF(getDS18S20Temp(A0)), averageADCReadings(A1,20), checkBatteryVolt()};
      RF24NetworkHeader header(rXNode); //Create transmit header. This goes in transmit packet to help route it where it needs to go, in this case it is the coordinator
      //send data onto network and make sure it gets there1
      if (network.write(header,&payload,sizeof(payload))) {
        digitalWrite(7,LOW); //transmit was successful so make sure status LED is off
      }
      else  { //transmit failed
        PIND |= (1<<PIND7); //this toggles the status LED at pin seven to show transmit failed
      }
    }
  }

 if(!mMode) { //if we are in end device mode get ready to go to sleep
    timer = true; //wer are not using the timer so make sure variable is set to true
     radio.powerDown(); //power down the nRF24L01 before we go to sleep
    for(int i=0; i<count; i++) { //loop sleep cycle to equal user entered transmit interval
        delayWDT(wDTInterval); //function for putting the Atmega328 to sleep
    }
  }
  else { //we are in router mode 
    timer = checkTimer(); //update timer and check to see if it is time to transmit
    batShutdown = false; //this should never be true when in router mode
  }
}

//This function sets the sleep interval timer
//Choices are 0 = 1 sec, 1 = 1 min (4 sec sleep interval 15), 2 = 10 min (8 sec sleep interval 75), 3 = 15 min (4 sec interval 225), 4 = 60 min (8 sec interval 450)
void setSleepInterval(byte interval) {

  //Following code sets the two variables for establishing the sleep interval on the ATmega328
  //wDTInterval sets the watch dog timer sleep interval at either 1 sec, 4 sec, 8 sec
  //count sets how many times we should loop the sleep cycle to equal the user defined transmit interval (1 sec, 1 min, 10 min, 15 min)
  if(interval == 0) { //set transmit interval to 1 sec
    count = 1;
    wDTInterval = 0b00000110;
    tInterval = 1000; //testing if time interval is going off
  }
  else if(interval == 1) { //set transmit interval to 1 min
    count = 15;
    wDTInterval = 0b00100000;
    tInterval = 60000;
  }
  else if(interval == 2) {
    count = 75;
    wDTInterval = 0b00100001;
    tInterval = 600000;
  }
  else {
    count = 225;
    wDTInterval = 0b00100000;
    tInterval = 900000;
  }
}

//This function serves as a power saving delay function. The argument is a Byte type variable that is used to set the delay time
//The function sets up sleep mode in power down state. The function then sets up the WDT timer in interrupt mode.
//It then puts the Arduino to sleep for the set time. Upon wake up the WDT and sleep mode are shut off
void delayWDT(byte timer) {
  sleep_enable(); //enable the sleep mode
  set_sleep_mode(SLEEP_MODE_PWR_DOWN); //set the type of sleep mode. Default is Idle. power down saves the most power
  ADCSRA &= ~(1<<ADEN); //Turn off ADC before going to sleep (set ADEN bit to 0). this saves even more power
  WDTCSR |= 0b00011000;    //Set the WDE bit and then clear it when set the prescaler, WDCE bit must be set if changing WDE bit   
  WDTCSR = 0b01000000 | timer; //This sets the WDT to the interval specified in the function argument
  wdt_reset(); //Reset the WDT 
  sleep_cpu(); //enter sleep mode. Next code that will be executed is the ISR when interrupt wakes Arduino from sleep
  sleep_disable(); //disable sleep mode
  ADCSRA |= (1<<ADEN); //Turn the ADC back on
}

//This function is used in router mode to track the transmit interval 
//It uses global variables set by the user or stored in EEPROM
//It returns false if it is not time to transmit and true when it is time to transmit
bool checkTimer() {
  unsigned long now = millis(); //get timer value
  if ( now - tStart >= tInterval  ) //check to see if it is time to transmit based on set interval
  {
    tStart = now; //reset start time of timer
    return true;
  }
  else return false;
}

//The function allows you to view and set the settigns of the wireless sensor module
//Settings include module network address, int. ADC ref value, battery volt cal factor, transmit interval, mode
//Settings are checked and set via serial monitor. when entering settings be sure not to use CR or line ending
//Settings need to be set when the first time the module is used. After that they can be viewed or set again by connecting 
//digital pin 8 to ground before power up or reset
void checkNodeAddress() {
  pinMode(8, INPUT_PULLUP); //set pin 8 as an input
  int val; //variable to store node address
  byte cAddr = 128; //this variable is used to see if this is the first time the module is being used
  byte tInterval; //set the transmit time interval
  byte mode; //set to router or end device mode
  float ref; //set measured internal reference value
  //The following strings are used by serial monitor more than once so to save memory they are made into variables
  String enter = F("Enter 'Y' for yes or any other character for no:"); //F() macro tells IDE to store string in flash memory and not SRAM
  String invalid = F("Invalid entry, default to ");
  String would = F("Would you like to update the ");

  //if this is either the first time the module is used or if pin 8 is connecting to ground enter settings mode
  if(EEPROM.get(0,cAddr)!= 128 || !digitalRead(8)) {
    digitalWrite(7,HIGH); //in settings mode so turn on status LED
    Serial.begin(57600); //start serial communication, need to turn off before using sleep and WDT
    //the following code reads the current settings from EEPROM and puts them in local variables
    Serial.println(F("Current settings in EEPROM"));
    Serial.print("Node address: ");
    Serial.println(EEPROM.get(1,val),OCT);
    Serial.print("Internal ref value: ");
    Serial.println(EEPROM.get(3,ref));
    Serial.print("Time interval: ");
    Serial.println(getInterval());
    Serial.print("Mode: ");
    Serial.println(getMode());
    //This following gives you the option to set each setting and if you change a setting it is stored in EEPROM
    Serial.print(would);
    Serial.print("Node Address? ");
    Serial.println(enter);
    if(getAnswer()) {
      Serial.println(F("Enter Node address to store in EEPROM"));
      while (!Serial.available()) { }
      val = Serial.parseInt();
      if(val >= 0) {
        EEPROM.put(1, val);
      }
      else { //if zero is entered it is invalid since coordinator is zero
        Serial.print(invalid);
        Serial.println("01");
        val = 01;
        EEPROM.put(1, val);
      }
    }
    Serial.print(would);
    Serial.print("internal ref voltage? ");
    Serial.println(enter);
    if(getAnswer()) {
      Serial.println(F("Enter internal ref voltage to store in EEPROM"));
      while (!Serial.available()) { }
      ref = Serial.parseFloat();
      if(ref >= 1.0 || ref <= 1.2) { //ADC reference value must be between 1.0 and 1.2
        EEPROM.put(3, ref);
      }
      else {
        Serial.print(invalid);
        Serial.println("1.1");
        ref = 1.1;
        EEPROM.put(3, ref);
      }
    }
    Serial.print(would);
    Serial.print("time interval? ");
    Serial.println(enter);
    if(getAnswer()) {
      Serial.println(F("Enter: 0 for 1 sec, 1 for 1 min, 2 for 10 min, 3 for 15 min"));
      while (!Serial.available()) { }
      tInterval = Serial.parseInt();
      if(tInterval >= 0 || tInterval < 4) { //check that entry was valid
        EEPROM.put(7, tInterval);
      }
      else {
        Serial.print(invalid);
        Serial.println("3");
        tInterval = 3;
        EEPROM.put(7, tInterval);
      }
    }
    Serial.print(would);
    Serial.print("mode setting? ");
    Serial.println(enter);
    if(getAnswer()) {
      Serial.println(F("Enter 0 for end device and 1 for router"));
      while (!Serial.available()) { }
      mode = Serial.parseInt();
      if(mode == 0 || mode == 1) { //check that entry was valid
        EEPROM.put(8, mode);
      }
      else {
        Serial.print(invalid);
        Serial.println("0");
        mode = 0;
        EEPROM.put(8, mode);
      }
    }
    
    getEEPROMValues(); //gets settings from EEPROM and stor in global variables
    //the following code prints out current settings from global variables
    Serial.print("Node address: ");
    Serial.println(thisNode,OCT);
    Serial.print("Internal ref voltage: ");
    Serial.println(iREF); 
    Serial.print("Time interval: ");
    Serial.println(sInterval);
    Serial.print("Mode setting: ");
    Serial.println(mMode);
    cAddr = 128; //write '128' to EEPROM to show that settings have been entered at least once
    EEPROM.put(0, cAddr);
    Serial.end(); //need to end serial communication before using the WDT and sleep functions
  }
  else {
     //not in settings mode so just get settings from EEPROM and store in global variables
    getEEPROMValues();
  }
}

//This function gets stored settings from EEPROM and stores in global variables
//The addresses are hard coded and are spread out based on size of stored variable
//each address is a byte in length
void getEEPROMValues() {
  EEPROM.get(1,thisNode);
  EEPROM.get(3,iREF);
  EEPROM.get(7,sInterval);
  EEPROM.get(8,mMode);
}

//Checks if user entered a 'Y' into the serial monitor
//if so returns true
bool getAnswer() {
   while (!Serial.available()) { }
   if(Serial.read() == 'Y') return true;
   else return false;
}


//gets transmit interval setting from EEPROM and prints it out to user
String getInterval() {
  byte i;
  String m = " min";
  EEPROM.get(7,i);
  if(i==0) {
    return "1 sec";
  }
  else if (i==1) {
    return ("1"+m);
  }
  else if (i==2) {
    return ("10"+m);
  }
  else {
     EEPROM.put(7,i);
    return ("15"+m);
  }
}

//gets mode setting from EEPROM and prints it out to user
String getMode() {
  byte m;
  EEPROM.get(8,m);
  if(m==1) {
    return "Router";
  }
  else {
    m = 0;
    EEPROM.put(8,m);
    return "End Device";
  }
}

//This function makes 8 ADC measurements but does nothing with them
//Since after a reference change the ADC can return bad readings. This function is used to get rid of the first 
//8 readings to ensure next reading is accurate
void burn8Readings() {
  for(int i=0; i<8; i++) {
    analogRead(A0);
    analogRead(A1);
  }
}

//This function convers the ADC level integer value into float voltage value.
//The inputs are the measured ADC value and the ADC reference voltage level
//The formula used was obtained from the data sheet: (ADC value / 1024) x ref voltage
float convertToVolt(float refVal, int aVAL) {
  return (((float)aVAL/1024)*refVal);
}

//This function gets temperature value from DS18S20 connected to specified Arduino pin
//This function uses two libraries and it is set for 9bit resolution temp reading
float getDS18S20Temp(int pin) {
  // Setup a oneWire instance to communicate with any OneWire devices (not just Maxim/Dallas temperature ICs)
  OneWire oneWire(pin);
  // Pass our oneWire reference to Dallas Temperature. 
  DallasTemperature sensors(&oneWire);
  sensors.begin(); //start communication with temp sensor
  sensors.setResolution(9);
  sensors.requestTemperatures(); // Send the command to get temperatures
  return sensors.getTempCByIndex(0);
}

float convertCtoF(float cValue) {
  return cValue*1.8 + 32;
}

//used to average multiple ADC values together
//This can help eliminate noise in measurements
int averageADCReadings(int aDCpin, int avgCount) {
 int aDCAvg = 0;
 for(int i=0;i<avgCount;i++) {
  aDCAvg = aDCAvg + analogRead(aDCpin);
 }

 return (aDCAvg/avgCount);
}

//This function uses the known internal reference value of the 328p (~1.1V) to calculate the VCC value which comes from a battery
//This was leveraged from a great tutorial found at https://code.google.com/p/tinkerit/wiki/SecretVoltmeter?pageId=110412607001051797704
float fReadVcc() {
  analogReference(EXTERNAL); //set the ADC reference to AVCC 
  burn8Readings(); //make 8 readings but don't use them to ensure good reading after ADC reference change 
  ADMUX = _BV(REFS0) | _BV(MUX3) | _BV(MUX2) | _BV(MUX1);
  unsigned long start = millis(); //get timer value
  while ( (start + 3) > millis()); //delay for 3 milliseconds
  ADCSRA |= _BV(ADSC); // Start ADC conversion
  while (bit_is_set(ADCSRA,ADSC)); //wait until conversion is complete
  int result = ADCL; //get first half of result
  result |= ADCH<<8; //get rest of the result
  float batVolt = (iREF / result)*1024; //Use the known iRef to calculate battery voltage
  analogReference(INTERNAL); //set the ADC reference back to internal
  burn8Readings(); //make 8 readings but don't use them to ensure good reading after ADC reference change 
  return batVolt;
}

//This function checks the battery voltage and let's the coordinator know when the battery is getting low
//If the battery gets too low this will trigger a constant sleep state
bool checkBatteryVolt() {
  float lowBat = 2.6; //voltage value for low battery
  float deadBat = 2.4; //voltage value for dead battery
  //Get ADC reading. Converter to a voltage. Multiple by battery cal factor to get true battery voltage
  //float batVal = convertToVolt(iREF,averageADCReadings(aPin,20))*bVoltDivide;
  float batVal = fReadVcc(); //get battery voltage by measuring internal ref with AVCC 

  if(batVal < deadBat) { 
    batShutdown = true; //battery is 'dead' the set battery shut down variable
    return false;
  }
  else if(batVal < lowBat) return false; //This will alert the coordinator that the battery power is low
  else return true; //battery is fine 
}


//This is the interrupt service routine for the WDT. It is called when the WDT times out. 
//This ISR must be in your Arduino sketch or else the WDT will not work correctly
ISR (WDT_vect) 
{
  wdt_disable();
   MCUSR = 0; //Clear WDT flag since it is disabled, this is optional
}  // end of WDT_vect
