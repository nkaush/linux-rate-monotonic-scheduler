#define LINUX

#include <linux/module.h>
#include <linux/kernel.h>

#include <linux/kthread.h>
#include <linux/proc_fs.h>
#include <uapi/linux/sched/types.h>
#include <linux/timer.h>
#include <linux/types.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/list.h>

#include "mp2_given.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Neil Kaushikkar");
MODULE_DESCRIPTION("CS-423 MP2");

#define DEBUG 1
#define FILENAME "status"
#define DIRECTORY "mp2"
#define PREFIX "[MP2] "
#define BASE10 10

#define RMS_THRESHOLD 693000UL
#define RMS_MULTIPLIER 1000000UL

// This enum represents the possible states an application registered with this
// module can be in throughout its real-time operating loop. 
enum mp2_task_state {
    READY,
    RUNNING,
    SLEEPING,
};

// This struct extends the Linux task_struct to store more information and data 
// about to run real-time scheduling implemented by this kernel module.
struct mp2_pcb {
    struct timer_list wakeup_timer;
    struct task_struct *linux_task;
    size_t period_ms;
    size_t runtime_ms;
    size_t deadline_jiff;
    pid_t pid;
    enum mp2_task_state state;
    struct list_head list;
};

// The task that is currently running
static struct mp2_pcb *current_task = NULL;
static DEFINE_SPINLOCK(current_task_lock);

// Spinlock synchronizing reads & writes to our list of processes to schedule
static DEFINE_SPINLOCK(rp_lock);

// This list keeps track of all the processes to schedule
static struct list_head task_list_head = LIST_HEAD_INIT(task_list_head);
static size_t task_list_size = 0;

// Keeps track of the current RMS status to speed up checking whether a task can
// be admitted to the module or not
static size_t current_rms_usage = 0;

// A slab allocator cache to speed up allocation of the extended task_structs
static struct kmem_cache *mp2_pcb_cache;

// A single thread workqueue for our dispatch thread
static struct task_struct *dispatcher;

// This proc entry represents the directory mp2 in the procfs 
static struct proc_dir_entry *proc_dir;

// This read callback is run whenever there is a read to the /proc/mp2/status 
// proc file system entry. This callback will write all active processes and 
// their respective processing times and periods to the user buffer, as allowed
// by the amount requested to read.
static ssize_t mp2_proc_read_callback(struct file *file, char __user *buffer, size_t count, loff_t *off);

// This write callback facilitates user applications' interactions with this 
// module. These interactions allow processes to register and deregister 
// themselves for scheduled by this module. Processes also interact with this 
// write callback to yield CPU time when finished with a real-time compute cycle.
static ssize_t mp2_proc_write_callback(struct file *file, const char __user *buffer, size_t count, loff_t *off);

// Compute the total RMS usage for a given period and runtime. This function is
// a helper to compute the total RMS usage. 
static size_t _compute_task_rms_usage(size_t period_ms, size_t runtime_ms);

// Check whether the module can admit a task with a given period and predicted 
// runtime, according to the Liu and Layland model.
static int can_admit_task(size_t period_ms, size_t runtime_ms);

// Admit a task to be scheduled by this module. Adds the given extended
// task_struct to the internal structure of tasks to schedule.
static void admit_task(struct mp2_pcb *pcb);

// Remove a task from being scheduled by this module.
static void deregister_task(pid_t pid);

// Destructor to assist in deallocating resources used by the extended task_struct.
void _teardown_pcb(struct mp2_pcb *pcb);

// This function handles all of the scheduling in its own kthread.
int dispatcher_work(void *data);

// This function wakes up the kernel thread to begin a scheduling cycle when 
// a timer triggers this function. 
void yield_timer_callback(struct timer_list *timer);

// Find a task to run that is in the READY state and has the highest priority
// (lowest period) of all tasks in the READY state. Returns NULL if no tasks
// are ready to be scheduled. 
static struct mp2_pcb * find_next_ready_task(void);

// Find the extended task structure in the module's list of structures by the 
// given pid. Returns NULL if no such structure is found. 
static struct mp2_pcb * find_mp2_pcb_by_pid(pid_t pid);

// This struct contains callbacks for operations on our procfs entry.
static const struct proc_ops mp2_file_ops = {
    .proc_read = mp2_proc_read_callback,
    .proc_write = mp2_proc_write_callback,
};

static ssize_t mp2_proc_read_callback(struct file *file, char __user *buffer, size_t count, loff_t *off) {
    struct mp2_pcb *entry;
    size_t to_copy = 0;
    ssize_t copied = 0;

    char *kernel_buf = (char *) kzalloc(count, GFP_KERNEL);
   
    // Go through each entry of the list and read + format the pid and cpu use
    spin_lock(&rp_lock);
    list_for_each_entry(entry, &task_list_head, list) {
        // if we have written more than can be copied to the user buffer, stop
        if (to_copy >= count) { break; }

        to_copy += 
            snprintf(kernel_buf + to_copy, count - to_copy, 
                "%d: %zu, %zu\n", entry->pid, entry->period_ms, entry->runtime_ms);
    }
    spin_unlock(&rp_lock);

    copied += simple_read_from_buffer(buffer, count, off, kernel_buf, to_copy);
    kfree(kernel_buf);

    return copied;
}

void _teardown_pcb(struct mp2_pcb *pcb) {
    list_del(&pcb->list);
    del_timer_sync(&pcb->wakeup_timer);
    --task_list_size;
    current_rms_usage -= _compute_task_rms_usage(pcb->period_ms, pcb->runtime_ms);
}

static size_t _compute_task_rms_usage(size_t period_ms, size_t runtime_ms) {
    return (runtime_ms * RMS_MULTIPLIER) / period_ms;
}

// @return 0 if we cannot admit the task, 1 if we can admit the task
static int can_admit_task(size_t period_ms, size_t runtime_ms) {
    int result;
    size_t extra_rms_usage = _compute_task_rms_usage(period_ms, runtime_ms);

    spin_lock(&rp_lock);
    result = current_rms_usage + extra_rms_usage <= RMS_THRESHOLD;
    spin_unlock(&rp_lock);

    return result;
}

static void admit_task(struct mp2_pcb *pcb) {
    spin_lock(&rp_lock);
    list_add(&pcb->list, task_list_head.next);
    ++task_list_size;
    current_rms_usage += _compute_task_rms_usage(pcb->period_ms, pcb->runtime_ms);
    spin_unlock(&rp_lock);
}

static struct mp2_pcb * find_mp2_pcb_by_pid(pid_t pid) {
    struct mp2_pcb *pcb;

    list_for_each_entry(pcb, &task_list_head, list) {
        if ( pcb->pid == pid ) {
            return pcb;
        }
    }
    return NULL;
}

static void deregister_task(pid_t pid) {
    struct mp2_pcb *pcb, *tmp;

    spin_lock(&rp_lock);
    list_for_each_entry_safe(pcb, tmp, &task_list_head, list) {
        if ( pcb->pid == pid ) {
            printk(PREFIX"removing pid %d from process list\n", pid);
            spin_lock(&current_task_lock);
            printk(PREFIX"Got lock\n");
            if ( current_task == pcb ) {
                printk(PREFIX"set current_task to NULL\n");
                current_task = NULL;
            }
            spin_unlock(&current_task_lock);
            printk(PREFIX"unlock!\n");
            
            _teardown_pcb(pcb);
            printk(PREFIX"teardown done!\n");
            kmem_cache_free(mp2_pcb_cache, pcb);
            printk(PREFIX"freed mem!\n");
            spin_unlock(&rp_lock);
            printk(PREFIX"unlock!\n");
            return;
        }
    }
    printk(PREFIX"Unable to deregister pid %d from process list\n", pid);
    spin_unlock(&rp_lock);
}

static ssize_t mp2_proc_write_callback(struct file *file, const char __user *buffer, size_t count, loff_t *off) {    
    unsigned long period, proc_time;
    ssize_t copied = 0;
    size_t kernel_buf_size = count + 1;
    char *kernel_buf = (char *) kzalloc(kernel_buf_size, GFP_KERNEL);
    char *kernel_strp, *pid_str, *period_str, *ptime_str;
    struct task_struct *pid_task = NULL;
    struct mp2_pcb *pcb = NULL;
    char command;
    pid_t pid;

    // Copy the userspace memory into our kernel buffer
    copied += simple_write_to_buffer(kernel_buf, kernel_buf_size, off, buffer, count);
    kernel_strp = kernel_buf + 2; // skip past the command and comma
    command = *kernel_buf;

    pid_str = strsep(&kernel_strp, ",");
    if ( kstrtoint(pid_str, BASE10, &pid ) != 0 ) { // failed to parse pid
        printk(PREFIX"Unable to parse pid [%s]\n", pid_str);
        kfree(kernel_buf);
        return copied;
    }
    
    if ( command == 'R' ) { // TRY TO REGISTER PROCESS
        period_str = strsep(&kernel_strp, ",");
        ptime_str = strsep(&kernel_strp, ",");

        pid_task = find_task_by_pid(pid);
        if ( !pid_task 
                || kstrtoul(period_str, BASE10, &period) 
                || kstrtoul(ptime_str, BASE10, &proc_time) ) { 
            // Unable to find task, or parse period/processing time, so exit
            printk(PREFIX"Unable to parse process info [%d][%s][%s]\n", pid, period_str, ptime_str);
            kfree(kernel_buf);
            return count;
        }

        if ( !can_admit_task(period, proc_time) ) {
            printk(PREFIX"Unable to admit task with pid=%d\n", pid);
            kfree(kernel_buf);
            return count;
        }

        printk(PREFIX"registering task with pid=%d, period=%zu, ptime=%zu\n", pid, period, proc_time);

        pcb = kmem_cache_alloc(mp2_pcb_cache, GFP_KERNEL);
        pcb->linux_task = pid_task;
        pcb->period_ms = period;
        pcb->runtime_ms = proc_time;
        pcb->pid = pid;
        pcb->state = RUNNING;
        pcb->deadline_jiff = jiffies + msecs_to_jiffies(pcb->period_ms);
        timer_setup(&pcb->wakeup_timer, yield_timer_callback, 0);

        admit_task(pcb);
        printk(PREFIX"Current RMS usage: %zu <= %zu", current_rms_usage, RMS_THRESHOLD);
    } else if ( command == 'D' ) { // TRY TO DE-REGISTER PROCESS
        printk(PREFIX"deregister pid %d\n", pid);
        deregister_task(pid);
        wake_up_process(dispatcher); // wakeup dispatch thread
    } else if ( command == 'Y' ) { // PROCESS YIELDED
        printk(PREFIX"pid %d yielded\n", pid);

        spin_lock(&rp_lock);
        pcb = find_mp2_pcb_by_pid(pid);
        if ( pcb != NULL ) { 
            pcb->state = SLEEPING;
            spin_lock(&current_task_lock);
            current_task = NULL;
            spin_unlock(&current_task_lock);

            if (jiffies >= pcb->deadline_jiff) { // we are past the deadline
                printk(PREFIX"Overshot deadline, keeping ready");
                while ( pcb->deadline_jiff < jiffies ) {
                    pcb->deadline_jiff += msecs_to_jiffies(pcb->period_ms);
                }
                
                pcb->state = READY;
                spin_unlock(&rp_lock);
            } else { // set timer to wakeup at next period start
                printk(PREFIX"registering timer to trigger at %zu (now: %zu)\n", pcb->deadline_jiff, jiffies);
                mod_timer(&pcb->wakeup_timer, pcb->deadline_jiff);
                pcb->deadline_jiff += msecs_to_jiffies(pcb->period_ms);
                spin_unlock(&rp_lock);
                
                set_current_state(TASK_UNINTERRUPTIBLE);
                schedule();
            } 

            wake_up_process(dispatcher); // wakeup dispatch thread
        } else {
            spin_unlock(&rp_lock);
        }
    }

    kfree(kernel_buf);
    return copied;
}

void yield_timer_callback(struct timer_list *timer) {
    struct mp2_pcb *pcb = from_timer(pcb, timer, wakeup_timer);
    pcb->state = READY;
    printk(PREFIX"timer for pid=%d triggered\n", pcb->pid);
    wake_up_process(dispatcher); // wakeup dispatch thread
}

static struct mp2_pcb * find_next_ready_task(void) {
    struct mp2_pcb *curr_pcb, *ready_task = NULL;

    list_for_each_entry(curr_pcb, &task_list_head, list) {
        // Find the next ready task to run
        if ( curr_pcb->state == READY ) {
            if ( ready_task == NULL ) { // the first ready task found 
                ready_task = curr_pcb;
            } else if ( curr_pcb->period_ms < ready_task->period_ms ) {
                // if the current task is ready and has a higher priority 
                // than the previous task found, then run this task...
                ready_task = curr_pcb;
            }
        }
    }

    if (ready_task) {
        printk(KERN_ALERT PREFIX "Found ready task pid=%d\n", ready_task->pid);
    } else {
        printk(KERN_ALERT PREFIX "No tasks ready to run\n");
    }

    return ready_task;
}

int dispatcher_work(void *data) {
    struct mp2_pcb *ready_task = NULL;
    struct sched_attr ready_attr, running_attr;
    int ret;
    (void) data;

    memset(&ready_attr, 0, sizeof(struct sched_attr));
    memset(&running_attr, 0, sizeof(struct sched_attr));

    while (!kthread_should_stop()) {
        printk(PREFIX"-------------------------\n");
        printk(PREFIX"Starting scheduling cycle\n");
        // we are pre-empting a currently running task
        spin_lock(&rp_lock);
        ready_task = find_next_ready_task();
        spin_unlock(&rp_lock);

        spin_lock(&current_task_lock);
        if ( current_task != NULL ) { 
            printk(PREFIX"Stopping task with pid=%d\n", current_task->pid);
            current_task->state = READY;

            // Make the Linux scheduler stop this task 
            running_attr.sched_policy = SCHED_NORMAL;
            running_attr.sched_priority = 0;
            ret = sched_setattr_nocheck(current_task->linux_task, &running_attr);
            if (ret) {
                printk(KERN_ALERT PREFIX "sched_setattr_nocheck returned %d", ret);
            }

            if ( !ready_task ) { // if there is no task ready to schedule other than this, schedule this...
                ready_task = current_task;
            }

            // sched_set_normal(current_task->linux_task, 19);
            current_task = NULL;
        }
        spin_unlock(&current_task_lock);
        
        // Find the next task to run
        spin_lock(&rp_lock);
        if ( ready_task != NULL ) {
            ready_task->state = RUNNING;
            printk(PREFIX"Starting to work on task with pid=%d\n", ready_task->pid);
            
            // Make the Linux scheduler run this task with highest priority
            wake_up_process(ready_task->linux_task);
            ready_attr.sched_policy = SCHED_FIFO;
            ready_attr.sched_priority = MAX_RT_PRIO - 1;
            ret = sched_setattr_nocheck(ready_task->linux_task, &ready_attr);
            if (ret) {
                printk(KERN_ALERT PREFIX "sched_setattr_nocheck returned %d", ret);
            }
            // sched_set_fifo(ready_task->linux_task);

            spin_lock(&current_task_lock);
            current_task = ready_task;
            spin_unlock(&current_task_lock);
        }
        spin_unlock(&rp_lock);
        
        printk(PREFIX"-------------------------\n");

        ready_task = NULL;
        set_current_state(TASK_INTERRUPTIBLE);
        schedule();
    }

    return 0;
}

// mp2_init - Called when module is loaded
int __init mp2_init(void) {
    #ifdef DEBUG
    printk(KERN_ALERT "MP2 MODULE LOADING\n");
    #endif

    // Setup proc fs entry
    proc_dir = proc_mkdir(DIRECTORY, NULL);
    proc_create(FILENAME, 0666, proc_dir, &mp2_file_ops);

    // Set up SLAB allocator cache
    mp2_pcb_cache = KMEM_CACHE(mp2_pcb, SLAB_PANIC);

    // Setup the dispatcher thread
    dispatcher = kthread_create(dispatcher_work, NULL, "mp2-dispatcher");

    printk(KERN_ALERT "MP2 MODULE LOADED\n");
    return 0;
}

// mp2_exit - Called when module is unloaded
void __exit mp2_exit(void) {
    struct mp2_pcb *entry, *tmp;

    #ifdef DEBUG
    printk(KERN_ALERT "MP2 MODULE UNLOADING\n");
    #endif
    // Remove the proc fs entry
    printk(PREFIX"Removing proc fs entry\n");
    remove_proc_entry(FILENAME, proc_dir);
    remove_proc_entry(DIRECTORY, NULL);

    // Stop our dispatch thread
    printk(PREFIX"Stopping kthread\n");
    kthread_stop(dispatcher);

    // Remove all the processes when removing our scheduler
    printk(PREFIX"Removing LL nodes\n");
    spin_lock(&rp_lock);
    list_for_each_entry_safe(entry, tmp, &task_list_head, list) {
        printk(PREFIX"removing process with pid %d\n", entry->pid);
        _teardown_pcb(entry);
        kmem_cache_free(mp2_pcb_cache, entry);
    };
    spin_unlock(&rp_lock);

    // Remove our SLAB allocator cache
    printk(PREFIX"Removing kmem cache\n");
    kmem_cache_destroy(mp2_pcb_cache);

    printk(KERN_ALERT "MP2 MODULE UNLOADED\n");
}

// Register init and exit funtions
module_init(mp2_init);
module_exit(mp2_exit);
