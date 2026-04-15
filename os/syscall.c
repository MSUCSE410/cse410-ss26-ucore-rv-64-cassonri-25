#include "syscall.h"
#include "console.h"
#include "defs.h"
#include "loader.h"
#include "syscall_ids.h"
#include "timer.h"
#include "trap.h"
int spawn(char *name);
uint64 sys_mmap(uint64 start, uint64 len, int port, int flag, int fd);
uint64 sys_munmap(uint64 start, uint64 len);

uint64 sys_write(int fd, uint64 va, uint len)
{
	debugf("sys_write fd = %d str = %x, len = %d", fd, va, len);
	if (fd != STDOUT)
		return -1;
	struct proc *p = curr_proc();
	char str[MAX_STR_LEN];
	int size = copyinstr(p->pagetable, str, va, MIN(len, MAX_STR_LEN));
	debugf("size = %d", size);
	for (int i = 0; i < size; ++i) {
		console_putchar(str[i]);
	}
	return size;
}

uint64 sys_read(int fd, uint64 va, uint64 len)
{
	debugf("sys_read fd = %d str = %x, len = %d", fd, va, len);
	if (fd != STDIN)
		return -1;
	struct proc *p = curr_proc();
	char str[MAX_STR_LEN];
	for (int i = 0; i < len; ++i) {
		int c = consgetc();
		str[i] = c;
	}
	copyout(p->pagetable, va, str, len);
	return len;
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
	debugf("fork!\n");
	return fork();
}

uint64 sys_exec(uint64 va)
{
	struct proc *p = curr_proc();
	char name[200];
	copyinstr(p->pagetable, name, va, 200);
	debugf("sys_exec %s\n", name);
	return exec(name);
}

uint64 sys_wait(int pid, uint64 va)
{
	struct proc *p = curr_proc();
	int *code = (int *)useraddr(p->pagetable, va);
	return wait(pid, code);
}
// creates a new process
// equivalent to fork + exec, but slightly more optimal due to it not copying a memory space just to immedietly delete it
// creates a new child process and executes the target program
// returns process ID of the child is succesful, otherwise -1
// atomic 
uint64 sys_spawn(uint64 va)
{
	// TODO: your job is to complete the sys call
	struct proc *p = curr_proc();
	char name[200];
	if (copyinstr(p->pagetable, name, va, 200) < 0) return -1;
	// this is the atomic load
	// calls the logic implemented in proc.c(allocproc) to allocate a slot
	// also loader.c to fill it with the program binary in one go
	return spawn(name);
}
// sets process priority
// allows a user-space program to influence the scheduler
// returns process id of the child if succesful
uint64 sys_set_priority(long long prio){
    // TODO: your job is to complete the sys call
	// the validation
	// if priorities less than 2 could cause pass values that are too large or lead to division by 0 errors
	if (prio < 2) return -1;
	struct proc *p = curr_proc();
	p->priority = prio;
	// this is the dynamic update:
	// we need to recalculate the pass value immediately so the process
	// will start to receive the new "fair share" of the CPU immedietly
	p->pass = (uint64)(0x1000000000000000L / prio);	
	return prio;	
}


extern char trap_page[];

void syscall()
{
	struct trapframe *trapframe = curr_proc()->trapframe;
	int id = trapframe->a7, ret;
	uint64 args[6] = { trapframe->a0, trapframe->a1, trapframe->a2,
			   trapframe->a3, trapframe->a4, trapframe->a5 };
	tracef("syscall %d args = [%x, %x, %x, %x, %x, %x]", id, args[0],
	       args[1], args[2], args[3], args[4], args[5]);
	switch (id) {
	case SYS_write:
		ret = sys_write(args[0], args[1], args[2]);
		break;
	case SYS_read:
		ret = sys_read(args[0], args[1], args[2]);
		break;
	case SYS_exit:
		sys_exit(args[0]);
		// __builtin_unreachable();
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
		ret = sys_exec(args[0]);
		break;
	case SYS_wait4:
		ret = sys_wait(args[0], args[1]);
		break;
	case SYS_spawn:
		ret = sys_spawn(args[0]);
		break;
	case SYS_setpriority:
		ret = sys_set_priority(args[0]);
		break;	
	case 215: // sys_munmap
        ret = sys_munmap(args[0], args[1]);
        break;
    case 222: // sys_mmap
        ret = sys_mmap(args[0], args[1], args[2], args[3], args[4]);
        break;
	default:
		ret = -1;
		errorf("unknown syscall %d", id);
	}
	trapframe->a0 = ret;
	tracef("syscall ret %d", ret);
}
uint64 sys_munmap(uint64 start, uint64 len) {
    if (len == 0) return 0;
    if (start % PAGE_SIZE != 0) return -1;

    struct proc *p = curr_proc();
    uint64 end = PGROUNDUP(start + len);

    for (uint64 va = start; va < end; va += PAGE_SIZE) {
        if (walkaddr(p->pagetable, va) == 0) return -1;
    }

    uint64 npages = (end - start) / PAGE_SIZE;
    uvmunmap(p->pagetable, start, npages, 1); 
    
    return 0;
}

uint64 sys_mmap(uint64 start, uint64 len, int port, int flag, int fd) {
    if (len == 0) return 0;
    if (len > 1024 * 1024 * 1024) return -1;
    if ((port & ~0x7) != 0 || (port & 0x7) == 0) return -1;
    if (start % PAGE_SIZE != 0) return -1;

    struct proc *p = curr_proc();
    uint64 end = PGROUNDUP(start + len);

    for (uint64 va = start; va < end; va += PAGE_SIZE) {
        if (walkaddr(p->pagetable, va) != 0) return -1;
    }

    int pte_flags = PTE_U | (port << 1); 
    for (uint64 va = start; va < end; va += PAGE_SIZE) {
        void *pa = kalloc();
        if (pa == 0) {
            sys_munmap(start, va - start);
            return -1;
        }
        memset(pa, 0, PAGE_SIZE);
        if (mappages(p->pagetable, va, PAGE_SIZE, (uint64)pa, pte_flags) != 0) {
            kfree(pa);
            sys_munmap(start, va - start);
            return -1;
        }
    }
    return 0;
}