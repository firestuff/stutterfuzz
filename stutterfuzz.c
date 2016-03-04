#include <stdio.h>
#include <stdint.h>

#include "rand.h"

static uint64_t get_split(uint64_t total_len, uint64_t remaining_len) {
	uint64_t rnd;
	rand_fill(&rnd, sizeof(rnd));
	rnd %= total_len;
	return rnd > remaining_len ? remaining_len : rnd;
}

int main(int __attribute__ ((unused)) argc, char __attribute__ ((unused)) *argv[]) {
	rand_init();

	uint64_t total_len = 1397;
	for (uint64_t remaining = total_len, consume = 0; remaining; remaining -= consume) {
		consume = get_split(total_len, remaining);
		fprintf(stderr, "consume %ju bytes\n", (uintmax_t) consume);
	}

	rand_cleanup();
}
