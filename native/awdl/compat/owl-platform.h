// SPDX-License-Identifier: MIT
#pragma once
#include <stdint.h>
#include <time.h>
#define __attribute__(x)
#define localtime_r(t,out) localtime_s(out,t)
#define CLOCK_MONOTONIC 1
int windrop_clock_gettime(int clock, struct timespec* out);
#define clock_gettime windrop_clock_gettime
