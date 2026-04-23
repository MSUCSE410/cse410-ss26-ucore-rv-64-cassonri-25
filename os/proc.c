#include "proc.h"
#include "defs.h"
#include "loader.h"
#include "trap.h"
#include "vm.h"
#include "queue.h"

struct proc pool[NPROC];
__attribute__((aligned(16))) char kstack[NPROC][PAGE_SIZE];
__attribute__((aligned(4096))) char trapframe[NPROC][TRAP_PAGE_SIZE];

extern char boot_stack_top[];
struct proc *current_proc;
struct proc idle;
struct queue task_queue;

int threadid()
{
	return curr_proc()->pid;
}

struct proc *curr_proc()
{
	return current_proc;
}

// initialize the proc table at boot time.
void proc_init()
{
	struct proc *p;
	for (p = pool; p < &pool[NPROC]; p++) {
		p->state = UNUSED;
		p->kstack = (uint64)kstack[p - pool];
		p->trapframe = (struct trapframe *)trapframe[p - pool];
	}
	idle.kstack = (uint64)boot_stack_top;
	idle.pid = IDLE_PID;
	current_proc = &idle;
	init_queue(&task_queue);
}

int allocpid()
{
	static int PID = 1;
	return PID++;
}

struct proc *fetch_task()
{
	int index = pop_queue(&task_queue);
	if (index < 0) {
		debugf("No task to fetch\n");
		return NULL;
	}
	debugf("fetch task %d(pid=%d) to task queue\n", index, pool[index].pid);
	return pool + index;
}

void add_task(struct proc *p)
{
	push_queue(&task_queue, p - pool);
	debugf("add task %d(pid=%d) to task queue\n", p - pool, p->pid);
}

// Look in the process table for an UNUSED proc.
// If found, initialize state required to run in the kernel.
// If there are no free procs, or a memory allocation fails, return 0.
struct proc *allocproc()
{
	struct proc *p;
	for (p = pool; p < &pool[NPROC]; p++) {
		if (p->state == UNUSED) {
			goto found;
		}
	}
	return 0;
// initialize all the accounting variables anytime a process is born
found:
	// init proc
	p->pid = allocpid();
	p->state = USED;
	p->ustack = 0;
	p->max_page = 0;
	p->parent = NULL;
	p->exit_code = 0;
	p->pagetable = uvmcreate((uint64)p->trapframe);
	memset(&p->context, 0, sizeof(p->context));
	memset((void *)p->kstack, 0, KSTACK_SIZE);
	memset((void *)p->trapframe, 0, TRAP_PAGE_SIZE);
	p->context.ra = (uint64)usertrapret;
	p->context.sp = p->kstack + KSTACK_SIZE;
	uint64 min_s = 0;
	// This is the stride scheduling init
	// we set the default a priority of 16
	// pass is here to determine how much stride increases every time the proc will run
	// initialize the stride to 0 or the system minimum, which is min_s in this case
	// this is so the process will be eligible to immedietly run
	for(struct proc *tmp = pool; tmp < &pool[NPROC]; tmp++) {
    	if(tmp->state == RUNNABLE && tmp->stride > min_s) min_s = tmp->stride;
	}
	p->priority = 16;
	p->stride = min_s;
	p->pass = (uint64)(0x1000000000000000L / 16);
	return p;
}

// Scheduler never returns.  It loops, doing:
//  - choose a process to run.
//  - swtch to start running that process.
//  - eventually that process transfers control
//    via swtch back to the scheduler.
// The scheduler is the brain of the os
// does not use a queue, but scans the pool for most deserving process - iterates
// ignores anything that is not "RUNNABLE"
// picks one with lowest stride
void scheduler()
{
	struct proc *p;
	struct proc *best;
	uint64 min_stride;
	for (;;) {
		best = NULL;
		// this is the selection loop
		// we iterate through the entire process pool to find the runnable process with the smallest stride value
		// this "greedy choice will ensure fairness"
		min_stride = -1ULL;
        // 1. Scan the pool for the RUNNABLE process with the smallest stride
        for (p = pool; p < &pool[NPROC]; p++) {
            if (p->state == RUNNABLE) {
                if (best == NULL || p->stride < best->stride) {
                    best = p;
				}
				if (p->stride < min_stride) {
					min_stride = p->stride;
				}	
			}	
		}
		/*int has_proc = 0;
		for (p = pool; p < &pool[NPROC]; p++) {
			if (p->state == RUNNABLE) {
				has_proc = 1;
				tracef("swtich to proc %d", p - pool);
				p->state = RUNNING;
				current_proc = p;
				swtch(&idle.context, &p->context);
			}
		}
		if(has_proc == 0) {
			panic("all app are over!\n");
		}*/
		// p = fetch_task();
		if (best != NULL) {
			if (min_stride > 0) {
				for (struct proc *hp = pool; hp < &pool[NPROC]; hp++) {
                    if (hp->state != UNUSED && hp->stride >= min_stride) {
                        hp->stride -= min_stride;
					}	
				}	
			}
			best->state = RUNNING;
			current_proc = best;
			// this is the stride update
			// before we switch to the process, we increment the stride by its pass value
			// this will charge the process for its CPU time
			best->stride += best->pass;
			swtch(&idle.context, &best->context);
			current_proc = &idle;
			//panic("all app are over!\n");
		}
		//tracef("swtich to proc %d", best - pool);
		//p->state = RUNNING;
		//current_proc = best;
		//best->stride += best->pass;
		//swtch(&idle.context, &p->context);
	}
}

// Switch to scheduler.  Must hold only p->lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->noff, but that would
// break in the few places where a lock is held but
// there's no process.
void sched()
{
	struct proc *p = curr_proc();
	if (p->state == RUNNING)
		panic("sched running");
	swtch(&p->context, &idle.context);
}

// Give up the CPU for one scheduling round.
void yield()
{
	current_proc->state = RUNNABLE;
	//add_task(current_proc);
	sched();
}

// Free a process's page table, and free the
// physical memory it refers to.
void freepagetable(pagetable_t pagetable, uint64 max_page)
{
	uvmunmap(pagetable, TRAMPOLINE, 1, 0);
	uvmunmap(pagetable, TRAPFRAME, 1, 0);
	uvmfree(pagetable, max_page);
}

void freeproc(struct proc *p)
{
	if (p->pagetable)
		freepagetable(p->pagetable, p->max_page);
	p->pagetable = 0;
	p->state = UNUSED;
}

int fork()
{
	struct proc *np;
	struct proc *p = curr_proc();
	// Allocate process.
	if ((np = allocproc()) == 0) {
		panic("allocproc\n");
	}
	// Copy user memory from parent to child.
	if (uvmcopy(p->pagetable, np->pagetable, p->max_page) < 0) {
		panic("uvmcopy\n");
	}
	np->max_page = p->max_page;
	// copy saved user registers.
	*(np->trapframe) = *(p->trapframe);
	// Cause fork to return 0 in the child.
	np->trapframe->a0 = 0;
	np->parent = p;
	np->state = RUNNABLE;
	//add_task(np);
	return np->pid;
}

int exec(char *name)
{
	int id = get_id_by_name(name);
	if (id < 0)
		return -1;
	struct proc *p = curr_proc();
	uvmunmap(p->pagetable, 0, p->max_page, 1);
	p->max_page = 0;
	loader(id, p);
	return 0;
}

int wait(int pid, int *code)
{
	struct proc *np;
	int havekids;
	struct proc *p = curr_proc();

	for (;;) {
		// Scan through table looking for exited children.
		havekids = 0;
		for (np = pool; np < &pool[NPROC]; np++) {
			if (np->state != UNUSED && np->parent == p &&
			    (pid <= 0 || np->pid == pid)) {
				havekids = 1;
				// loop
				if (np->state == ZOMBIE) {
					// Found one.
					//np->state = UNUSED;
					// this is resource reclimation
					// only place where freeproc is called
					// with freeing of resources here, ensures that as soon as parent finishes
					// witing, the process slot(NPROC) is actually available for new apps
					pid = np->pid;
					*code = np->exit_code;
					freeproc(np);
					np->state = UNUSED;
					return pid;
				}
			}
		}
		if (!havekids) {
			return -1;
		}
		p->state = RUNNABLE;
		//p->stride += p->pass;
		//add_task(p);
		sched();
	}
}

// Exit the current process.
void exit(int code)
{
	struct proc *p = curr_proc();
	p->exit_code = code;
	debugf("proc %d exit with %d\n", p->pid, code);
	//freeproc(p);
	// this is the zombie transition
	// dont free memory over here, the parent might need it still to read exit code
	// we mark it as ZOMBIE so the scheduler will ignore it
	// 'wait' is still able to find it
	if (p->parent != NULL) {
		// Parent should `wait`
		p->state = ZOMBIE;
	} else {
		p->state = UNUSED;
	}	
	// Set the `parent` of all children to NULL
	struct proc *np;
	for (np = pool; np < &pool[NPROC]; np++) {
		if (np->parent == p) {
			np->parent = NULL;
			if(np->state == ZOMBIE) np->state = UNUSED;
		}
	}
	// orphan handling
	sched();
}
// makes a child and loads target program directly into it - helper function for sys_spawn
int spawn(char *name)
{
    int id = get_id_by_name(name); 
    if (id < 0) return -1; 
	// helper function calls allocproc to reserve a slot in the process pool 
    struct proc *np = allocproc(); 
    if (np == 0) return -1; 
	//call loader to load the program binary into memory
    loader(id, np); 
    np->parent = curr_proc(); 
    np->state = RUNNABLE;
    //add_task(np); 
    
    return np->pid; 
}