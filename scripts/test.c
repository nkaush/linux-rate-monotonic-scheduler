#include <sys/types.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <err.h>

#define NS_PER_SECOND 1000000000UL
#define NS_PER_MS 1000000UL

size_t fibonacci(size_t n) {
	if (n == 0) {
		return 0;
	} else if (n == 1) {
		return 1;
	} else {
		return fibonacci(n - 1) + fibonacci(n - 2);
	}
}

void timespec_difftime(struct timespec *start, struct timespec *finish, struct timespec *diff) {
    size_t delta = (finish->tv_sec - start->tv_sec) * NS_PER_SECOND;
    delta += finish->tv_nsec - start->tv_nsec;

    diff->tv_sec = delta / NS_PER_SECOND;
    diff->tv_nsec = delta % NS_PER_SECOND;
}

size_t ns_to_ms(size_t ns) {
    return ns / NS_PER_MS;
}

int main(int argc, char **argv) {
    struct timespec start, end, diff;
    size_t n = 0;

    if ( argc != 2 ) {
        errx(1, "Usage: %s <count>", argv[0]);
    }

    n = strtoul(argv[1], NULL, 10);
    clock_gettime(CLOCK_MONOTONIC, &start);
    fibonacci(n);
    clock_gettime(CLOCK_MONOTONIC, &end);
    timespec_difftime(&start, &end, &diff);
    printf("diff: %lds %ldms\n", diff.tv_sec, ns_to_ms(diff.tv_nsec));
}