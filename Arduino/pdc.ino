#include <SPI.h>
#include <mcp2515.h>

#define SLAVE_ID 0x0F6
#define PIN_OUT 3              // Speed sensor pin
#define TRIG 8                 // Ultrasonic Trigger pin
#define ECHO 7                 // Ultrasonic Echo pin
#define CUSTOM_DELAY 100       // Measurement cycle [ms]
#define WHEEL_CIRCUMFERENCE_CM 20.083  
#define PULSES_PER_REV 20      

MCP2515 mcp2515(9);            // CS pin is 9
volatile unsigned int pulseCount = 0;
struct can_frame canMsg;

/* Union for distance data (4-byte float conversion) */
union DistanceUnion {
    float value;
    byte bytes[4];
} distanceData;

// Speed sensor Interrupt Service Routine (ISR)
void isrCount() {
    pulseCount++;
}

// Function to measure distance using ultrasonic sensor
float getDistance() {
    digitalWrite(TRIG, LOW);
    delayMicroseconds(2);
    digitalWrite(TRIG, HIGH);
    delayMicroseconds(10);
    digitalWrite(TRIG, LOW);
    
    // Timeout set to 50ms (measures up to approx. 8.5m)
    float cycletime = pulseIn(ECHO, HIGH, 50000); 
    
    if (cycletime == 0) return -1.0; // Return -1.0 if measurement fails
    
    // Convert time(us) to cm: (Time * Speed of Sound(340m/s)) / 10000 / 2 (Round trip)
    return ((340.0 * cycletime) / 10000.0) / 2.0;
}

void setup() {
    Serial.begin(115200);

    // Initialize CAN (1000KBPS)
    mcp2515.reset();
    mcp2515.setBitrate(CAN_1000KBPS, MCP_16MHZ);
    mcp2515.setNormalMode();

    // Configure speed sensor
    pinMode(PIN_OUT, INPUT);
    attachInterrupt(digitalPinToInterrupt(PIN_OUT), isrCount, RISING);

    // Configure ultrasonic sensor
    pinMode(TRIG, OUTPUT);
    pinMode(ECHO, INPUT);

    // Initialize CAN message structure
    canMsg.can_id = SLAVE_ID;
    canMsg.can_dlc = 8;
    memset(canMsg.data, 0x00, 8);
}

void loop() {
    // 1. Wait for the measurement cycle
    delay(CUSTOM_DELAY);

    // 2. Calculate speed (Safely copy pulse count from ISR)
    noInterrupts();
    unsigned int pulses = pulseCount;
    pulseCount = 0;
    interrupts();

    float intervalSec = CUSTOM_DELAY / 1000.0;
    float revs = pulses / (float)PULSES_PER_REV;
    float speed = revs * WHEEL_CIRCUMFERENCE_CM / intervalSec;

    // 3. Measure distance
    distanceData.value = getDistance();

    // 4. CAN Data Packing
    // [Speed Data - Indices 0, 1, 2]
    int int1_spd = (int)speed;
    int int2_spd = round((speed - int1_spd) * 100);

    canMsg.data[0] = int1_spd / 256;      // Integer part (High byte)
    canMsg.data[1] = int1_spd % 256;      // Integer part (Low byte)
    canMsg.data[2] = (byte)int2_spd;      // Two decimal places

    // [Distance Data - Indices 3, 4, 5, 6] (4-byte Float)
    for (int i = 0; i < 4; i++) {
        canMsg.data[3 + i] = distanceData.bytes[i];
    }

    canMsg.data[7] = 0x00; // Reserved/Empty byte

    // 5. Send CAN message and output to Serial
    if (mcp2515.sendMessage(&canMsg) == MCP2515::ERROR_OK) {
        Serial.print("Speed: "); Serial.print(speed);
        Serial.print(" cm/s, Distance: "); Serial.print(distanceData.value);
        Serial.println(" cm [Sent]");
    } else {
        Serial.println("CAN Send Error");
    }
}