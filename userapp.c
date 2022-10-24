#include "userapp.h"

#include <sys/types.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <err.h>

#define MP2_FILE "/proc/mp2/status"
#define NS_PER_SECOND 1000000000UL

void register_app(void) {
    char* buf = NULL;
    FILE* f = fopen(MP2_FILE, "w");

    asprintf(&buf, "R,%d,%zu,%zu", getpid());
    fwrite(buf, strlen(buf), 1, f);
    fclose(f);
    free(buf);
}

void send_message(char m) {
    char* buf = NULL;
    FILE* f = fopen(MP2_FILE, "w");

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
    size_t buf_size = 0;
    pid_t pid = -1;

    while ( getline(&lineptr, &buf_size, f) != EOF ) {
        colon = strstr(lineptr, ":");
        *colon = '\0';
        sscanf(lineptr, "%d", &pid);

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

int main(int argc, char *argv[]) {
    struct timespec t0, wakeup_time, process_time, now;

    register_app();
	if ( !is_app_registered() ) {
        errx(1, "application was not registered...");
    }

	clock_gettime(CLOCK_MONOTONIC, &t0);
	yield_app();

	while (exists job) {
        clock_gettime(CLOCK_MONOTONIC, &now);
        difftime(&t0, &now, &wakeup_time); // wakeup_time = clock_gettime() - t0;
		
        do_job();
		process_time = clock_gettime() - wakeup_time;
		printf(wakeup_time, process_time);
		yield_app();
	}

	deregister_app();
	return 0;
}

