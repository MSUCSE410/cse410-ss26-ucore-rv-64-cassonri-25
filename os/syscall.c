#include "console.h"
#include "defs.h"
#include "loader.h"
#include "sync.h"
#include "syscall.h"
#include "syscall_ids.h"
#include "timer.h"
#include "trap.h"
#include "proc.h"

uint64 console_write(uint64 va, uint64 len)
{
	struct proc *p = curr_proc();
	char str[MAX_STR_LEN];
	int size = copyinstr(p->pagetable, str, va, MIN(len, MAX_STR_LEN));
	tracef("write size = %d", size);
	for (int i = 0; i < size; ++i) {
		console_putchar(str[i]);
	}
	return len;
}

uint64 console_read(uint64 va, uint64 len)
{
	struct proc *p = curr_proc();
	char str[MAX_STR_LEN];
	tracef("read size = %d", len);
	for (int i = 0; i < len; ++i) {
		int c = consgetc();
		str[i] = c;
	}
	copyout(p->pagetable, va, str, len);
	return len;
}

uint64 sys_write(int fd, uint64 va, uint64 len)
{
	if (fd < 0 || fd > FD_BUFFER_SIZE)
		return -1;
	struct proc *p = curr_proc();
	struct file *f = p->files[fd];
	if (f == NULL) {
		errorf("invalid fd %d\n", fd);
		return -1;
	}
	switch (f->type) {
	case FD_STDIO:
		return console_write(va, len);
	case FD_PIPE:
		return pipewrite(f->pipe, va, len);
	case FD_INODE:
		return inodewrite(f, va, len);
	default:
		panic("unknown file type %d\n", f->type);
	}
}

uint64 sys_read(int fd, uint64 va, uint64 len)
{
	if (fd < 0 || fd > FD_BUFFER_SIZE)
		return -1;
	struct proc *p = curr_proc();
	struct file *f = p->files[fd];
	if (f == NULL) {
		errorf("invalid fd %d\n", fd);
		return -1;
	}
	switch (f->type) {
	case FD_STDIO:
		return console_read(va, len);
	case FD_PIPE:
		return piperead(f->pipe, va, len);
	case FD_INODE:
		return inoderead(f, va, len);
	default:
		panic("unknown file type %d\n", f->type);
	}
}

__attribute__((noreturn)) void sys_exit(int code)
{
	exit(code);
	__builtin_unreachable();
}

uint64 sys_sched_yield()
{
	yield();
	return 0;
}

uint64 sys_gettimeofday(uint64 val, int _tz)
{
	struct proc *p = curr_proc();
	uint64 cycle = get_cycle();
	TimeVal t;
	t.sec = cycle / CPU_FREQ;
	t.usec = (cycle % CPU_FREQ) * 1000000 / CPU_FREQ;
	copyout(p->pagetable, val, (char *)&t, sizeof(TimeVal));
	return 0;
}

uint64 sys_getpid()
{
	return curr_proc()->pid;
}

uint64 sys_getppid()
{
	struct proc *p = curr_proc();
	return p->parent == NULL ? IDLE_PID : p->parent->pid;
}

uint64 sys_clone()
{
	debugf("fork!");
	return fork();
}

static inline uint64 fetchaddr(pagetable_t pagetable, uint64 va)
{
	uint64 *addr = (uint64 *)useraddr(pagetable, va);
	return *addr;
}

uint64 sys_exec(uint64 path, uint64 uargv)
{
	struct proc *p = curr_proc();
	char name[MAX_STR_LEN];
	copyinstr(p->pagetable, name, path, MAX_STR_LEN);
	uint64 arg;
	static char strpool[MAX_ARG_NUM][MAX_STR_LEN];
	char *argv[MAX_ARG_NUM];
	int i;
	for (i = 0; uargv && (arg = fetchaddr(p->pagetable, uargv));
	     uargv += sizeof(char *), i++) {
		copyinstr(p->pagetable, (char *)strpool[i], arg, MAX_STR_LEN);
		argv[i] = (char *)strpool[i];
	}
	argv[i] = NULL;
	return exec(name, (char **)argv);
}

uint64 sys_wait(int pid, uint64 va)
{
	struct proc *p = curr_proc();
	int *code = (int *)useraddr(p->pagetable, va);
	return wait(pid, code);
}

uint64 sys_pipe(uint64 fdarray)
{
	struct proc *p = curr_proc();
	uint64 fd0, fd1;
	struct file *f0, *f1;
	if (f0 < 0 || f1 < 0) {
		return -1;
	}
	f0 = filealloc();
	f1 = filealloc();
	if (pipealloc(f0, f1) < 0)
		goto err0;
	fd0 = fdalloc(f0);
	fd1 = fdalloc(f1);
	if (fd0 < 0 || fd1 < 0)
		goto err0;
	if (copyout(p->pagetable, fdarray, (char *)&fd0, sizeof(fd0)) < 0 ||
	    copyout(p->pagetable, fdarray + sizeof(uint64), (char *)&fd1,
		    sizeof(fd1)) < 0) {
		goto err1;
	}
	return 0;

err1:
	p->files[fd0] = 0;
	p->files[fd1] = 0;
err0:
	fileclose(f0);
	fileclose(f1);
	return -1;
}

uint64 sys_openat(uint64 va, uint64 omode, uint64 _flags)
{
	struct proc *p = curr_proc();
	char path[200];
	copyinstr(p->pagetable, path, va, 200);
	return fileopen(path, omode);
}

uint64 sys_close(int fd)
{
	if (fd < 0 || fd > FD_BUFFER_SIZE)
		return -1;
	struct proc *p = curr_proc();
	struct file *f = p->files[fd];
	if (f == NULL) {
		errorf("invalid fd %d", fd);
		return -1;
	}
	fileclose(f);
	p->files[fd] = 0;
	return 0;
}

int sys_thread_create(uint64 entry, uint64 arg)
{
	struct proc *p = curr_proc();
	int tid = allocthread(p, entry, 1);
	if (tid < 0) {
		errorf("fail to create thread");
		return -1;
	}
	struct thread *t = &p->threads[tid];
	t->trapframe->a0 = arg;
	t->state = RUNNABLE;
	add_task(t);
	return tid;
}

int sys_gettid()
{
	return curr_thread()->tid;
}

int sys_waittid(int tid)
{
	if (tid < 0 || tid >= NTHREAD) {
		errorf("unexpected tid %d", tid);
		return -1;
	}
	struct thread *t = &curr_proc()->threads[tid];
	if (t->state == T_UNUSED || tid == curr_thread()->tid) {
		return -1;
	}
	if (t->state != EXITED) {
		return -2;
	}
	memset((void *)t->kstack, 7, KSTACK_SIZE);
	t->tid = -1;
	t->state = T_UNUSED;
	return t->exit_code;
}

/*
*	LAB5: (3) In the TA's reference implementation, here defines funtion
*					int deadlock_detect(const int available[LOCK_POOL_SIZE],
*						const int allocation[NTHREAD][LOCK_POOL_SIZE],
*						const int request[NTHREAD][LOCK_POOL_SIZE])
*				for both mutex and semaphore detect, you can also
*				use this idea or just ignore it.
*/
// This section implements the safety algorithm and the system calls for mutexes and semaphores.
// deadlock - when a group of threads are stuck in a permanent wait because every thread in group is waiting for resource held by another thread in the group [cite: 183, 187]
// runs a simulation where if allows current request for something it checks if it would cause a system deadlock
// if deadlock detected, action is not allowed
int deadlock_detect(const int available[LOCK_POOL_SIZE * 2],
                    const int allocation[NTHREAD][LOCK_POOL_SIZE * 2],
                    const int request[NTHREAD][LOCK_POOL_SIZE * 2]) {
    int work[LOCK_POOL_SIZE * 2]; // Temporary array for available resources
    int finish[NTHREAD] = {0};    // Tracks threads that could potentially complete
    int total_res = LOCK_POOL_SIZE * 2; 
	// Initialize work with currently available resources
    for(int i = 0; i < total_res; i++) work[i] = available[i];
	// Mark threads as "finished" if they have no active resource interaction
    for(int i = 0; i < NTHREAD; i++) {
        int has_active = 0;
        for(int j = 0; j < total_res; j++) {
            if(allocation[i][j] > 0 || request[i][j] > 0) {
                has_active = 1;
                break;
            }
        }
        // If a thread has no interaction with resources, it's "finished" 
        if(!has_active) finish[i] = 1;
    }
	// Main Safety Algorithm loop: simulate resource allocation
    while(1) {
        int found = 0;
        for(int i = 0; i < NTHREAD; i++) {
            if(!finish[i]) {
                int can_finish = 1;
				// Check if current thread's requests can be met by available work
                for(int j = 0; j < total_res; j++) {
                    if(request[i][j] > work[j]) {
                        can_finish = 0;
                        break;
                    }
                }
				// If thread can finish, release its current allocations back to work
                if(can_finish) {
                    for(int j = 0; j < total_res; j++)
                        work[j] += allocation[i][j];
                    finish[i] = 1;
                    found = 1;
                }
            }
        }
        if(!found) break; // Exit loop if no more threads can be finished
    }
	// If any active thread remains unfinished, a deadlock exists [cite: 183, 187]
    for(int i = 0; i < NTHREAD; i++) {
        if(!finish[i]) return 1; // Deadlock exists [cite: 183, 187]
    }
    return 0; // State is safe
}
// System call to create a mutex [cite: 87]
// mutex-manages access to critical section of the code;process must request mutex
//critical section - accesses a shared resource and must not be accesssed by more than one thread simultaneously
int sys_mutex_create(int blocking)
{
	struct proc *p = curr_proc();
    struct mutex *m = mutex_create(blocking);
    if (m == NULL) {
        errorf("fail to create mutex: out of resource");
        return -1;
    }
	// LAB5: (4-1) You may want to maintain some variables for detect here
	// Calculate ID and update availability for detection logic [cite: 101]
	int mutex_id = m - p->mutex_pool;
    p->available_res[mutex_id] = 1; // Mutexes have 1 unit available initially
    debugf("create mutex %d", mutex_id);
    return mutex_id;
}
// System call to acquire a mutex [cite: 111]
// records that a thread wants the lock-wants access to critical area
int sys_mutex_lock(int mutex_id)
// LAB5: (4-1) Record that this thread is now requesting the mutex
// LAB5: (4-1) You may want to maintain some variables for detect
//       or call your detect algorithm here
{
	struct proc *p = curr_proc();
    int tid = curr_thread()->tid;
    if (mutex_id < 0 || mutex_id >= p->next_mutex_id) return -1;
	//if thread requests lock, run deadlock detect
	// Proactively check for deadlock before blocking the thread [cite: 121]
    if (p->deadlock_detect_enabled) {
        p->request[tid][mutex_id] = 1; // Mark request
		//if deadlock found
        if (deadlock_detect(p->available_res, p->allocation, p->request)) {
			// Report and prevent the deadlock by failing the call [cite: 187]
            printf("ERROR %d-%ddetect deadlock on locking mutex %d!\n", p->pid, tid, mutex_id);
            p->request[tid][mutex_id] = 0;
			//when deadlock is found, returns this
            return -0xDEAD; // Special error value for user-level tests
        }
    }
	// Attempt to acquire the actual kernel mutex [cite: 125]
	// if theres no deadlock
    mutex_lock(&p->mutex_pool[mutex_id]);
	// Success: Update detection matrices to reflect ownership
    p->request[tid][mutex_id] = 0;
    p->allocation[tid][mutex_id] = 1;
    p->available_res[mutex_id] = 0;
    return 0;
}
// System call to release a mutex [cite: 133]
//takes away lock from that process
// resets the tracker,meaning mutex is available
int sys_mutex_unlock(int mutex_id)
{
	// LAB5: (4-1) You may want to maintain some variables for detect here
	struct proc *p = curr_proc();
    int tid = curr_thread()->tid;
    if (mutex_id < 0 || mutex_id >= p->next_mutex_id) return -1;
	// Update detection state: mutex is free and no longer allocated to this thread [cite: 144]
    p->allocation[tid][mutex_id] = 0;
    p->available_res[mutex_id] = 1;
    mutex_unlock(&p->mutex_pool[mutex_id]); // Release kernel mutex
    return 0;
}
// System call to create a semaphore [cite: 151]
// semaphore - maintains counter to control access to pool of shared resources
int sys_semaphore_create(int res_count)
{
	// LAB5: (4-2) You may want to maintain some variables for detect here
	struct proc *p = curr_proc();
    struct semaphore *s = semaphore_create(res_count); // Initialize kernel semaphore
    if (s == NULL) return -1;
    
    int sem_id = s - p->semaphore_pool;
    // Store the count in our tracking array
	// Map semaphore ID to detection array using offset [cite: 169]
    p->available_res[sem_id + LOCK_POOL_SIZE] = res_count; 
    return sem_id;
}
// System call to release semaphore resources [cite: 174]
//thread is done using resource and releases it back to the system, allowing other threads to acquire it
int sys_semaphore_up(int semaphore_id)
{
	// LAB5: (4-2) You may want to maintain some variables for detect here
	struct proc *p = curr_proc();
    int tid = curr_thread()->tid;
    int idx = semaphore_id + LOCK_POOL_SIZE; 
    if (semaphore_id < 0 || semaphore_id >= p->next_semaphore_id) return -1;
	// Update state: Release allocation and increment availability
    if (p->allocation[tid][idx] > 0) p->allocation[tid][idx]--;
	//one more resource becomes available when up is called
    p->available_res[idx]++;
    semaphore_up(&p->semaphore_pool[semaphore_id]); 
    return 0;
}
// System call to acquire semaphore resources [cite: 177]
// checks if taking a resource causes a deadlock;if safe, gives resource to thread that needs it
int sys_semaphore_down(int semaphore_id)
{
	// LAB5: (4-2) You may want to maintain some variables for detect
	//       or call your detect algorithm here
	struct proc *p = curr_proc();
    int tid = curr_thread()->tid;
	// Use offset to track semaphores
    int idx = semaphore_id + LOCK_POOL_SIZE; // Map to the second half of the resource array
    
    if (semaphore_id < 0 || semaphore_id >= p->next_semaphore_id) return -1;
	// Check for potential circular wait [cite: 175]
    if (p->deadlock_detect_enabled) {
        // Step 1: Tentatively mark the request
        p->request[tid][idx] = 1; // Log resource request

        // Step 2: Check if this request creates a deadlock
        if (deadlock_detect(p->available_res, p->allocation, p->request)) {
            // Step 3: Print EXACT error format: ERROR pid-tiddetect...
			// Report deadlock according to required format [cite: 183]
            printf("ERROR %d-%ddetect deadlock on down semaphore %d!\n", p->pid, tid, semaphore_id);
            p->request[tid][idx] = 0; // Clear request because we are failing the call
            return -0xDEAD; // Fail call to avoid indefinite block
        }
    }

    // Step 4: If no deadlock, proceed to block if necessary
	// Call kernel semaphore down (blocking)
    semaphore_down(&p->semaphore_pool[semaphore_id]);

    // Step 5: Once acquired, move from request to allocation
	// Update state once resource is successfully obtained
    p->request[tid][idx] = 0;
    p->allocation[tid][idx]++;
    p->available_res[idx]--;
    return 0;
}

int sys_condvar_create()
{
	struct condvar *c = condvar_create();
	if (c == NULL) {
		errorf("fail to create condvar: out of resource");
		return -1;
	}
	int cond_id = c - curr_proc()->condvar_pool;
	debugf("create condvar %d", cond_id);
	return cond_id;
}

int sys_condvar_signal(int cond_id)
{
	if (cond_id < 0 || cond_id >= curr_proc()->next_condvar_id) {
		errorf("Unexpected condvar id %d", cond_id);
		return -1;
	}
	cond_signal(&curr_proc()->condvar_pool[cond_id]);
	return 0;
}

int sys_condvar_wait(int cond_id, int mutex_id)
{
	if (cond_id < 0 || cond_id >= curr_proc()->next_condvar_id) {
		errorf("Unexpected condvar id %d", cond_id);
		return -1;
	}
	if (mutex_id < 0 || mutex_id >= curr_proc()->next_mutex_id) {
		errorf("Unexpected mutex id %d", mutex_id);
		return -1;
	}
	cond_wait(&curr_proc()->condvar_pool[cond_id],
		  &curr_proc()->mutex_pool[mutex_id]);
	return 0;
}

// LAB5: (2) you may need to define function enable_deadlock_detect here
// Toggle deadlock detection for the process [cite: 57]
uint64 sys_enable_deadlock_detect(int enabled) {
    curr_proc()->deadlock_detect_enabled = enabled; 
    return 0;
}

extern char trap_page[];

void syscall()
{
	struct trapframe *trapframe = curr_thread()->trapframe;
	int id = trapframe->a7, ret;
	uint64 args[6] = { trapframe->a0, trapframe->a1, trapframe->a2,
			   trapframe->a3, trapframe->a4, trapframe->a5 };
	if (id != SYS_write && id != SYS_read && id != SYS_sched_yield) {
		debugf("syscall %d args = [%x, %x, %x, %x, %x, %x]", id,
		       args[0], args[1], args[2], args[3], args[4], args[5]);
	}
	switch (id) {
	case SYS_write:
		ret = sys_write(args[0], args[1], args[2]);
		break;
	case SYS_read:
		ret = sys_read(args[0], args[1], args[2]);
		break;
	case SYS_openat:
		ret = sys_openat(args[0], args[1], args[2]);
		break;
	case SYS_close:
		ret = sys_close(args[0]);
		break;
	case SYS_exit:
		sys_exit(args[0]);
		// __builtin_unreachable();
	// case SYS_nanosleep:
	// 	ret = sys_nanosleep(args[0]);
	// 	break;
	case SYS_sched_yield:
		ret = sys_sched_yield();
		break;
	case SYS_gettimeofday:
		ret = sys_gettimeofday(args[0], args[1]);
		break;
	case SYS_getpid:
		ret = sys_getpid();
		break;
	case SYS_getppid:
		ret = sys_getppid();
		break;
	case SYS_clone: // SYS_fork
		ret = sys_clone();
		break;
	case SYS_execve:
		ret = sys_exec(args[0], args[1]);
		break;
	case SYS_wait4:
		ret = sys_wait(args[0], args[1]);
		break;
	case SYS_pipe2:
		ret = sys_pipe(args[0]);
	case SYS_thread_create:
		ret = sys_thread_create(args[0], args[1]);
		break;
	case SYS_gettid:
		ret = sys_gettid();
		break;
	case SYS_waittid:
		ret = sys_waittid(args[0]);
		break;
	case SYS_mutex_create:
		ret = sys_mutex_create(args[0]);
		break;
	case SYS_mutex_lock:
		ret = sys_mutex_lock(args[0]);
		break;
	case SYS_mutex_unlock:
		ret = sys_mutex_unlock(args[0]);
		break;
	case SYS_semaphore_create:
		ret = sys_semaphore_create(args[0]);
		break;
	case SYS_semaphore_up:
		ret = sys_semaphore_up(args[0]);
		break;
	case SYS_semaphore_down:
		ret = sys_semaphore_down(args[0]);
		break;
	case SYS_condvar_create:
		ret = sys_condvar_create();
		break;
	case SYS_condvar_signal:
		ret = sys_condvar_signal(args[0]);
		break;
	case SYS_condvar_wait:
		ret = sys_condvar_wait(args[0], args[1]);
		break;
	// LAB5: (2) you may need to add case SYS_enable_deadlock_detect here
	case SYS_enable_deadlock_detect: // Ensure this ID is in syscall_ids.h
		ret = sys_enable_deadlock_detect(args[0]);
		break;
	default:
		ret = -1;
		errorf("unknown syscall %d", id);
	}
	curr_thread()->trapframe->a0 = ret;
	if (id != SYS_write && id != SYS_read && id != SYS_sched_yield) {
		debugf("syscall %d ret %d", id, ret);
	}
}
