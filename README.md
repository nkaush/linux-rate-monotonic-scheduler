# Rate-Monotonic Real-Time Linux Kernel Scheduler Module

## Usage:

An application that wishes to use this real-time scheduling module must use the 
following callbacks to interact with the scheduler interface: 
* Application registration - Notify the module to schedule this application
    * Note that application registration may be silently denied if the application CPU requirements exceeds what the scheduler is able to provide while also scheduling other tasks. 
* Processing time yielding - Notify the module that the application has finished it's real-time computation
* Application deregistration - Notify the module to remove the application from scheduling in the future

## Example C Code

### Application Registration

```C++
char* buf = NULL;
FILE* f = fopen("/proc/mp2/status", "w");
size_t period = ...
size_t processing_time = ...

asprintf(&buf, "R,%d,%zu,%zu", getpid(), period, processing_time);
fwrite(buf, strlen(buf), 1, f);
fclose(f); free(buf);
```

### Verifying that the Application Was Registered
```C++
int is_registered = 0; // initially false...
char *lineptr = NULL, *colon = NULL;
FILE *f = fopen(MP2_FILE, "r");
size_t buf_size = 0;

while ( getline(&lineptr, &buf_size, f) != EOF ) {
    colon = strstr(lineptr, ":"); *colon = '\0';
    if ( atoi(lineptr) == getpid() ) {
        is_registered = 1; // this process is registered!
        break;
    }
}

free(lineptr); fclose(f);
// continue...
```

### Processing Time Yielding

```C++
char* buf = NULL;
FILE* f = fopen("/proc/mp2/status", "w");
asprintf(&buf, "Y,%d", getpid());
fwrite(buf, strlen(buf), 1, f);
fclose(f); free(buf);
```

### Application Deregistration

```C++
char* buf = NULL;
FILE* f = fopen("/proc/mp2/status", "w");
asprintf(&buf, "D,%d", getpid());
fwrite(buf, strlen(buf), 1, f);
fclose(f); free(buf);
```

## Implementation

This module uses an entry in the proc filesystem to allow applications in the userspace to interact with it. Userspace applications can register, deregister, and yield using the protocol specified above as the module uses a single write callback to handle all kinds of interactions. Userspace applications can verify that they are registered by checking whether its pid is contained in the list of pids copied on a read to the proc filesystem entry. The module uses a read callback to support this. It also keeps track of all processes registered at any given instant with an internal linked list. Internally, the module uses a SLAB allocator to allocate list nodes for slightly optimized performance so that most of the CPU time is given to run processes registered with the module. This module also uses fixed point arithmetic to determine whether processes may be registered under the Liu/Layland model of a total processing time to period ratio of less than 0.693. It is expensive to use floating point math in the kernel, so the module resorts to fixed point arithmetic to compute the ratio.