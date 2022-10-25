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
