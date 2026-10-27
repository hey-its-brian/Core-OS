/* Simulator-only hooks. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Queue a keyboard event as the HAL would report it (HID usage + modifiers). */
void sim_push_key(uint8_t code, uint8_t mods, bool pressed);
