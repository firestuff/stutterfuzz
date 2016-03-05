#pragma once

#include <stdbool.h>
#include <stddef.h>

void rand_init(void);
void rand_cleanup(void);
void rand_fill(void *, size_t);
bool rand_yes_no(uint64_t);
