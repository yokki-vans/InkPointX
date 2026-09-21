#pragma once
using BaseType_t = int;
constexpr int pdFALSE = 0, pdTRUE = 1;
#define pdMS_TO_TICKS(ms) (ms)
#define portYIELD_FROM_ISR() ((void)0)
