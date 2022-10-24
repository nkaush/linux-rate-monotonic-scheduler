#include "userapp.h"

#define _GNU_SOURCE 1
#include <sys/types.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <err.h>

#define MP2_FILE "/proc/mp2/status"
#define NS_PER_SECOND 1000000000UL
#define DEFAULT_NUM_ITER 15
#define BASE_10 10

// This fibonacci number should take 320 to 330 ms to compute
#define FIBONACCI_NUMBER 39

void register_app(size_t period, size_t ptime) {
    char* buf = NULL;
    FILE* f = fopen(MP2_FILE, "w");

    if ( !f ) {
        err(1, "fopen %s", MP2_FILE);
    }

    asprintf(&buf, "R,%d,%zu,%zu", getpid(), period, ptime);
    fwrite(buf, strlen(buf), 1, f);
    fclose(f);
    free(buf);
}

void send_message(char m) {
    char* buf = NULL;
    FILE* f = fopen(MP2_FILE, "w");

    if ( !f ) {
        err(1, "fopen %s", MP2_FILE);
    }

    asprintf(&buf, "%c,%d", m, getpid());
    fwrite(buf, strlen(buf), 1, f);
    fclose(f);
    free(buf);
}

void deregister_app(void) {
    send_message('D');
}

void yield_app(void) {
    send_message('Y');
}

int is_app_registered(void) {    
    char *lineptr = NULL, *colon = NULL;
    FILE *f = fopen(MP2_FILE, "r");

    if ( !f ) {
        err(1, "fopen %s", MP2_FILE);
    }

    size_t buf_size = 0;
    pid_t pid = -1;

    while ( getline(&lineptr, &buf_size, f) != EOF ) {
        colon = strstr(lineptr, ":");
        *colon = '\0';
        pid = atoi(lineptr);

        if ( pid == getpid() ) {
            free(lineptr);
            fclose(f);
            return 1;
        }
    }

    free(lineptr);
    fclose(f);
    return 0;
}

void timespec_difftime(struct timespec *start, struct timespec *finish, struct timespec *diff) {
    size_t delta = (finish->tv_sec - start->tv_sec) * NS_PER_SECOND;
    delta += finish->tv_nsec - start->tv_nsec;

    diff->tv_sec = delta / NS_PER_SECOND;
    diff->tv_nsec = delta % NS_PER_SECOND;
}

void print_wakeup_and_process_times(struct timespec *wakeup, struct timespec *process) {
    printf("wakeup:  %lds %ldns\n", wakeup->tv_sec, wakeup->tv_nsec);
    printf("process: %lds %ldns\n", process->tv_sec, process->tv_nsec);
}

size_t fibonacci(size_t n) {
	if (n == 0) {
		return 0;
	} else if (n == 1) {
		return 1;
	} else {
		return fibonacci(n - 1) + fibonacci(n - 2);
	}
}

int main(int argc, char *argv[]) {
    struct timespec t0, wakeup_time, process_time, now, later;
    size_t num_iterations = DEFAULT_NUM_ITER, i = 0;
    size_t period = 0, ptime = 0;

    if ( argc >= 3 ) {
        period = strtoul(argv[1], NULL, BASE_10);
        ptime = strtoul(argv[2], NULL, BASE_10);

        if ( argc >= 4 ) {
            num_iterations = strtoul(argv[3], NULL, BASE_10);
        }
    } else {
        errx(1, "Usage: %s <period> <processing time> <num cycles (optional)>", argv[0]);
    }

    register_app(period, ptime);
	if ( !is_app_registered() ) {
        errx(1, "application was not registered...");
    }

	clock_gettime(CLOCK_REALTIME, &t0);
	yield_app();

	while (i++ < num_iterations) {
        clock_gettime(CLOCK_REALTIME, &now);
        timespec_difftime(&t0, &now, &wakeup_time); // wakeup_time = clock_gettime() - t0;
		
        fibonacci(FIBONACCI_NUMBER);

        clock_gettime(CLOCK_REALTIME, &later);
        timespec_difftime(&now, &later, &process_time); // process_time = clock_gettime() - wakeup_time;
		print_wakeup_and_process_times(&wakeup_time, &process_time);
		yield_app();
	}

	deregister_app();
	return 0;
}
