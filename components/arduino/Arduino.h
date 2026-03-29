#ifndef ARDUINO_H
#define ARDUINO_H

#include <stdint.h>
#include <math.h>

// Arduino type aliases
typedef uint8_t byte;

// Pin modes
#define INPUT  0
#define OUTPUT 1
#define INPUT_PULLUP 2

// Logic levels
#define HIGH 1
#define LOW  0

// GPIO functions
void pinMode(uint8_t pin, uint8_t mode);
void digitalWrite(uint8_t pin, uint8_t val);
int digitalRead(uint8_t pin);

// Timing functions
unsigned long micros();
unsigned long millis();
void delay(unsigned long ms);

// Minimal Serial stub
class HardwareSerial {
public:
    void begin(unsigned long baud);
    void println(const char *msg);
    void println(int val);
};

extern HardwareSerial Serial;

#endif
