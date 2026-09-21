#pragma once
#include <cstddef>
#include <cstdint>
constexpr int HIGH = 1;
constexpr int LOW = 0;
unsigned long millis();
void delay(unsigned long ms);
int digitalRead(int pin);

#define PROGMEM
