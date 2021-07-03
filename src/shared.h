#pragma once

#define Assert(cond) do { if (!(cond)) __debugbreak(); } while (0)
#define AssertHR(hr) Assert(SUCCEEDED(hr))

#define ArrayCount(Array) (sizeof(Array) / sizeof((Array)[0]))
#define IsPowerOfTwo(Value) (((Value) & ((Value) - 1)) == 0)
#define SafeRatio1(A, B) ((B) ? ((A)/(B)) : (A))