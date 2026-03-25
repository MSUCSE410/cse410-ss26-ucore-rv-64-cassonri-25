#include "syscall.h"
#include "defs.h"
#include "loader.h"
#include "syscall_ids.h"
#include "timer.h"
#include "trap.h"
#include "vm.h"
uint64 sys_mmap(uint64 start, uint64 len, int port, int flag, int fd);
uint64 sys_munmap(uint64 start, uint64 len);

uint64 sys_write(int fd, uint64 va, uint len)
{
	debugf("sys_write fd = %d va = %x, len = %d", fd, va, len);
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

uint64 sys_gettimeofday(TimeVal *val, int _tz) // TODO: implement sys_gettimeofday in pagetable. (VA to PA)
{
	struct proc *p = curr_proc();
	TimeVal *pa_val = (TimeVal *)useraddr(p->pagetable, (uint64)val);
	if (pa_val == 0) return -1;
	// YOUR CODE
	// val->sec = 0;
	// val->usec = 0;

	/* The code in `ch3` will leads to memory bugs*/

	uint64 cycle = get_cycle();
	pa_val->sec = cycle / CPU_FREQ;
	pa_val->usec = (cycle % CPU_FREQ) * 1000000 / CPU_FREQ;
	return 0;
}

// TODO: add support for mmap and munmap syscall.
// hint: read through docstrings in vm.c. Watching CH4 video may also help.
// Note the return value and PTE flags (especially U,X,W,R)
/*
* LAB1: you may need to define sys_task_info here
*/

int sys_task_info(TaskInfo *ti) {
	struct proc *p = curr_proc();
	TaskInfo *pa_ti = (TaskInfo *)useraddr(p->pagetable, (uint64)ti);
	if (pa_ti == 0) return -1;
	pa_ti->status = Running;
	for(int i = 0; i < 500; i++) {
		pa_ti->syscall_times[i] = p->syscall_times[i];
	}
	uint64 cycle = get_cycle();
	uint64 now = (cycle * 1000UL) / CPU_FREQ;
	pa_ti->time = (int)(now - p->start_time);

	return 0;
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
	/*
	* LAB1: you may need to update syscall counter for task info here
	*/

	if (id >= 0 && id < 500) {
		curr_proc()->syscall_times[id]++;
	}
	switch (id) {
	case SYS_write:
		ret = sys_write(args[0], args[1], args[2]);
		break;
	case SYS_exit:
		sys_exit(args[0]);
		// __builtin_unreachable();
	case SYS_sched_yield:
		ret = sys_sched_yield();
		break;
	case SYS_gettimeofday:
		ret = sys_gettimeofday((TimeVal *)args[0], args[1]);
		break;
	/*
	* LAB1: you may need to add SYS_taskinfo case here
	*/
	case SYS_getpid:
		ret=curr_proc()->pid;
		break;
	case SYS_task_info:
		ret = sys_task_info((TaskInfo *)args[0]);
		break;
		// ADD THESE TWO CASES BELOW
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

    // 1. Validation: All pages in the range MUST be currently mapped [cite: 57]
    for (uint64 va = start; va < end; va += PAGE_SIZE) {
        if (walkaddr(p->pagetable, va) == 0) return -1;
    }

    // 2. Unmap and Free
    // Your vm.c provides uvmunmap(pagetable, va, npages, do_free)
    uint64 npages = (end - start) / PAGE_SIZE;
    uvmunmap(p->pagetable, start, npages, 1); // '1' means free physical memory
    
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
            // Insufficient physical memory: rollback and return -1 [cite: 47]
            sys_munmap(start, va - start);
            return -1;
        }
		memset(pa, 0, PAGE_SIZE); // Clean the "anonymous" memory
        
        if (mappages(p->pagetable, va, PAGE_SIZE, (uint64)pa, pte_flags) != 0) {
            kfree(pa);
            sys_munmap(start, va - start);
            return -1;
        }
    }
    return 0;
}

