#include "MPU6050.h"
#include <I2Cdev.h>
#include <Wire.h>
#include <Arduino_FreeRTOS.h>
#include <semphr.h>
#include <stdlib.h>
#include <avr/power.h>

// Packet definitons
// Packet format: ID, x1, y1, z1, x2, y2, z2, x3, y3, z3, x4, y4, z4, voltage, current, power, totalPower, checksum
// Packet max length: 2 (ID) + 8 (char per sensor data) * 12 (12 sensor data) + 5 (char per power data) * 4 (4 power data)
// + 3 (checksum) + 19 (delimiter) = 140 bytes
const int MAX_DATA_POINTS = 12;         // 4 sensors of 3 data points each
const int MAX_POWER_POINTS = 4;         // 4 different power parameters
const int MAX_PACKET_SIZE = 150;
const int MAX_BUFFER = 30;

int ackID = 0;
int sendID = 0;
int slotID = 0;
char packet[MAX_PACKET_SIZE];

// Sensors and Power definitions
const int CURR_PIN = A0;
const int VOLT_PIN = A1;
const float RS = 0.1;                   // Shunt resistor value (in ohms)
const int RL = 10000;                   // RL of the INA169 (in ohms)
const int R1 = 22;                     // R1 of voltage divider circuit, between power source and VOLT_PIN, in kohms
const int R2 = 22;                     // R2 of voltage divider circuit, between VOLT_PIN and ground, in kohms

float voltage_divide = ((float) R1 + R2) / (float) R2;  // Measured voltage is R2/(R1+R2) times actual V
float current = 0.0;                    // Calculated current value
float voltage = 0.0;                    // Calculated voltage
float power = 0.0;                      // Calculated power (P = IV)
float totalPower = 0.0;                   // Calculated energy (E = Pt)
unsigned long timeLastTaken = 0;        // The last time readings were calculated (in number of ms elapsed since startup)
unsigned long tempTime = 0;             // To use as "current time" in two lines

const int RESET_PIN = 4;
  
// Sensor definitions
MPU6050 accelgyro; // class default I2C address is 0x68
MPU6050 accelgyro2(0x69);
int16_t accX1, accY1, accZ1;
int16_t gyX1, gyY1, gyZ1;
int16_t accX2, accY2, accZ2;
int16_t gyX2, gyY2, gyZ2;

float gForceX1, gForceY1, gForceZ1; // Accelerometer processed variables
float rotX1, rotY1, rotZ1;          // Gyroscope processed variables

float gForceX2, gForceY2, gForceZ2; // Accelerometer processed variables
float rotX2, rotY2, rotZ2;          // Gyroscope processed variables

float sensorData[MAX_DATA_POINTS]; // Acc1, Acc2, Acc3, gyro1
float powerData[MAX_POWER_POINTS]; // Voltage, current, power, totalPower

/////////////////////////////////
/////      HARDWARE        //////
/////////////////////////////////
void setup() {
  // Digital pins for reset
  digitalWrite(RESET_PIN, HIGH);
  pinMode(RESET_PIN, OUTPUT);

  // Digital pins for power
  pinMode(CURR_PIN, INPUT);
  pinMode(VOLT_PIN, INPUT);
    
   // Force unused DIGITAL pins to 0V for power saving
  for (int i = 0; i <= 53; i++) {
    if (i == 4 || i == 10 || i == 12 || i == 13 || i == 17 || i == 18 || i ==  19 || i == 20 || i == 21)
      continue;
    pinMode(i, OUTPUT);
    digitalWrite(i, LOW);
  }

  // Force unused ANALOG pins to 0V for power saving
  pinMode(A2, OUTPUT);
  pinMode(A3, OUTPUT);
  pinMode(A4, OUTPUT);
  pinMode(A5, OUTPUT);
  pinMode(A6, OUTPUT);
  pinMode(A7, OUTPUT);
  pinMode(A8, OUTPUT);
  pinMode(A9, OUTPUT);
  pinMode(A10, OUTPUT);
  pinMode(A11, OUTPUT);
  pinMode(A12, OUTPUT);
  pinMode(A13, OUTPUT);
  pinMode(A14, OUTPUT);
  pinMode(A15, OUTPUT);
  digitalWrite(A2, LOW);
  digitalWrite(A3, LOW);
  digitalWrite(A4, LOW);
  digitalWrite(A5, LOW);
  digitalWrite(A6, LOW);
  digitalWrite(A7, LOW);
  digitalWrite(A8, LOW);
  digitalWrite(A9, LOW);
  digitalWrite(A10, LOW);
  digitalWrite(A11, LOW);
  digitalWrite(A12, LOW);
  digitalWrite(A13, LOW);
  digitalWrite(A14, LOW);
  digitalWrite(A15, LOW);

  // Disable SPI, unused USART
  power_spi_disable();
  power_usart2_disable();
  
  // Disable unused system timers
  power_timer1_disable();
  power_timer2_disable();
  power_timer3_disable();
  power_timer4_disable();
  power_timer5_disable();

  Serial.begin(115200);
  Serial1.begin(115200); // Serial1: P19 RX, P18 TX
  Serial.println("Arduino Online!");
  Wire.begin();
  
  setupSensors();
  handshake();

  xTaskCreate(startWork, "working", 200, NULL, 1, NULL);
  vTaskStartScheduler();
}

void setupSensors() {
    accelgyro.initialize();
    accelgyro2.initialize();
}

// Get raw data, transform accelerometer readings to g, gyrometer readings to deg
void getSensorValues() {
    // Read sensors, integration with hardware
    accelgyro.getMotion6(&accX1, &accY1, &accZ1, &gyX1, &gyY1, &gyZ1);
    accelgyro2.getMotion6(&accX2, &accY2, &accZ2, &gyX2, &gyY2, &gyZ2);

    gForceX1 = accX1 / 16384.0 * 9.81;
    gForceY1 = accY1 / 16384.0 * 9.81; 
    gForceZ1 = accZ1 / 16384.0 * 9.81;
    rotX1 = gyX1 / 131.0;
    rotY1 = gyY1 / 131.0; 
    rotZ1 = gyZ1 / 131.0;

    gForceX2 = accX2 / 16384.0 * 9.81;
    gForceY2 = accY2 / 16384.0 * 9.81; 
    gForceZ2 = accZ2 / 16384.0 * 9.81;
    rotX2 = gyX2 / 131.0;
    rotY2 = gyY2 / 131.0; 
    rotZ2 = gyZ2 / 131.0;

    sensorData[0] = gForceX1;
    sensorData[1] = gForceY1;
    sensorData[2] = gForceZ1;
    sensorData[3] = rotX1;
    sensorData[4] = rotY1;
    sensorData[5] = rotZ1;
    sensorData[6] = gForceX2;
    sensorData[7] = gForceY2;
    sensorData[8] = gForceZ2;
    sensorData[9] = rotX2;
    sensorData[10] = rotY2;
    sensorData[11] = rotZ2;
}

void getPowerValues() {
  voltage = ((float) analogRead(VOLT_PIN) * 5.0) * voltage_divide / 1023.0;
  
  float Vout = (float) analogRead(CURR_PIN) * 5.0 / 1023.0;
  
  current = (Vout * 1000) / (RL * RS);
  
  power = voltage * current;
  
  tempTime = millis();
  totalPower += (tempTime - timeLastTaken) * power / 1000.0;
  timeLastTaken = tempTime;

  powerData[0] = voltage;
  powerData[1] = current;
  powerData[2] = power;
  powerData[3] = totalPower;
}

/////////////////////////////////
/////        COMMS         //////
/////////////////////////////////
void handshake() {
  while (1) { // Break only if rpi initiate handshake
    if (Serial1.available() && Serial1.read() == 'S') {
      Serial1.write('A');
      Serial.println("Ack Handshake"); 
      break;
    }else if (Serial.available() && Serial.read() == 'S') { // Test Ack Handshake
      Serial.println("Ack Handshake");
      break;
    }
  }

  while (1) { // Break only if Ack received
    if (Serial1.available() && Serial1.read() == 'A') {
      Serial.println("Handshake complete");
      delay(500);
      break;
    } else if (Serial.available() && Serial.read() == 'A') { // Test Ack Handshake
      Serial.println("Handshake complete");
      break;
    }
  }
}

static void startWork(void * pvParameters) {
  TickType_t xLastWakeTime = xTaskGetTickCount();
  const TickType_t xFrequency = 100;
  
  while (1) {
    TickType_t xCurrWakeTime = xTaskGetTickCount();

    getSensorValues();
    getPowerValues();
    formatMessage();
    pushMessage();
    getResponse();

    Serial.println();
    vTaskDelayUntil(&xCurrWakeTime, 30/portTICK_PERIOD_MS); // 30 ms interval, ~33 samples/s
  }
}

void formatMessage() {
  if (ackID == (slotID + 1) % MAX_BUFFER) {
    Serial.println("Buffer Full");
    return; // No more empty slots
  }

  strcpy(packet, "");
  char slotIDChar[1];
  itoa(slotID, slotIDChar, 10);
  strcat(packet, slotIDChar);  // Converts slotID int to str and cat to packet as the packetId

  for (int i = 0; i < MAX_DATA_POINTS + MAX_POWER_POINTS; i++) {
    strcat(packet, ","); // Delimiter
    char floatChar[8];
    if (i < MAX_DATA_POINTS) dtostrf(sensorData[i], 0, 2, floatChar); // Converts floats to str, inputs: val, min char, char after dp, dest
    else dtostrf(powerData[i-MAX_DATA_POINTS], 0, 2, floatChar);

    strcat(packet, floatChar);
  }

  char checksum = 0;
  int len = strlen(packet);
  for (int i = 0; i < len; i++) checksum ^= packet[i];

  char checksumChar[3];
  itoa((int) checksum, checksumChar, 10);

  strcat(packet, ",");
  strcat(packet, checksumChar);
  strcat(packet, "\n");

  slotID = (slotID + 1) % MAX_BUFFER;
  Serial.print("Message formatted, len: ");
  Serial.println(strlen(packet));
  Serial.println(packet);
}

void pushMessage() {
  if (sendID != slotID) {
    Serial.println("Pushing message");
    for (int i = 0; i < MAX_PACKET_SIZE; i++) Serial1.write(packet[i]);
    sendID = (sendID + 1) % MAX_BUFFER;
    Serial.println("Pushed message");
  } 
}

void getResponse() {
  char val = Serial1.read();
  Serial.print("Response: ");
  Serial.println(val);

  if (Serial1.available() && val == 'A') {
    Serial.println("Packet Ack");
    int packetID = Serial1.read();
    ackID = packetID;
  } else if (Serial1.available() && val == 'N') {
    Serial.println("Packet Nack");
    int packetID = Serial1.read();
    ackID = packetID;
    sendID = packetID; // Resend previous frame 
  } else if (val == 'R') { // Do not need to check Serial1.available() as RpiClient has closed it
    Serial.println("Resetting");
    digitalWrite(RESET_PIN, LOW); // Resets Arduino
  }
}

void loop() { }