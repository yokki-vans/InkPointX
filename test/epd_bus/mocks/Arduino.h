#pragma once
#include <cstdint>
#include <cstring>
#define IRAM_ATTR
#define DRAM_ATTR
constexpr int HIGH = 1, LOW = 0, INPUT = 0, OUTPUT = 1, INPUT_PULLUP = 2;
constexpr int FALLING = 2, RISING = 3;
unsigned long millis();
void delay(unsigned long ms);
int digitalRead(int pin);
inline void pinMode(int, int) {}
inline void digitalWrite(int, int) {}
inline int digitalPinToInterrupt(int p) { return p; }
void attachInterrupt(int, void (*)(), int);
void detachInterrupt(int);
struct SerialMock {
  explicit operator bool() const { return false; }
  template <typename... T>
  void printf(const char*, T...) {}
};
inline SerialMock Serial;
