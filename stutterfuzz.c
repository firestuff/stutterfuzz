#include <stdio.h>
#include <stdint.h>

#include "rand.h"

static uint64_t sqrt64(uint64_t n) {
	uint64_t g = UINT64_C(1) << 31;
  
	for (uint64_t c = g; c; g |= c) {
		if (g * g > n) {
			g ^= c;  
		}
		c >>= 1;
	}  
	return g;
}  

static uint64_t get_split(uint64_t len) {
	uint64_t rnd;
	rand_fill(&rnd, sizeof(rnd));
	rnd %= (len * len);
	return sqrt64(rnd) + 1;
}

int main(int __attribute__ ((unused)) argc, char __attribute__ ((unused)) *argv[]) {
	rand_init();

	for (uint64_t len = 1397; len;) {
		uint64_t consume = get_split(len);
		fprintf(stderr, "consume %ju bytes\n", (uintmax_t) consume);
		len -= consume;
	}

	rand_cleanup();
}
