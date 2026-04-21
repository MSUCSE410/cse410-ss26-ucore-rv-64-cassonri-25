#include "syscall.h"
#include "console.h"
#include "defs.h"
#include "loader.h"
#include "syscall_ids.h"
#include "timer.h"
#include "trap.h"
#include "fs.h"
#include "file.h"
#include "types.h"
#include "proc.h"
#include "string.h"
extern struct proc pool[NPROC];
int spawn(char *name);
uint64 sys_mmap(uint64 start, uint64 len, int port, int flag, int fd);
uint64 sys_munmap(uint64 start, uint64 len);
struct inode* namei(char*);
struct inode* nameiparent(char*, char*);
struct inode* dirlookup(struct inode*, char*, uint*);
void ivalid(struct inode*);
void iunlock(struct inode*);
void iupdate(struct inode*);
void iput(struct inode*);
int dirlink(struct inode*, char*, uint);
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

uint64 sys_spawn(uint64 va)
{
	// TODO: your job is to complete the sys call
struct proc *p = curr_proc();
    char name[200];
    if (copyinstr(p->pagetable, name, va, 200) < 0) return -1;

    // Just call the helper; let the helper do the heavy lifting
    return spawn(name);
}

uint64 sys_set_priority(long long prio)
{
	// TODO: your job is to complete the sys call
	if (prio < 2) return -1;
	struct proc *p = curr_proc();
	p->priority = prio;
	p->pass = (uint64)(0x1000000000000000L / prio);	
	return prio;	
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

int sys_fstat(int fd,uint64 stat){
	//TODO: your job is to complete the syscall
	struct file *f;
    struct proc *p = curr_proc();
    Stat st;

    if(fd < 0 || fd >= NFILE || (f = p->files[fd]) == 0) return -1;

    ivalid(f->ip); // Use ivalid instead of ilock
    st.dev = 0;
    st.ino = f->ip->inum;
    st.nlink = f->ip->nlink;
	st.size = f->ip->size;
    st.mode = (f->ip->type == T_DIR) ? 0x040000 : 0x100000;
    //iunlock(f->ip);

    if(copyout(p->pagetable, stat, (char *)&st, sizeof(st)) < 0) return -1;
    return 0;
}

int sys_linkat(int olddirfd, uint64 oldpath, int newdirfd, uint64 newpath, uint64 flags){
	//TODO: your job is to complete the syscall
	char old_buf[MAXPATH], new_buf[MAXPATH]; // Use unique names for local buffers
    struct inode *ip, *dp;
    char name[DIRSIZ];
    struct proc *p = curr_proc();

    // Use the 'oldpath' and 'newpath' parameters directly [cite: 20, 21]
    if(copyinstr(p->pagetable, old_buf, oldpath, MAXPATH) < 0 ||
       copyinstr(p->pagetable, new_buf, newpath, MAXPATH) < 0) {
        return -1;
    }

    if((ip = namei(old_buf)) == 0) return -1; 

    ivalid(ip);
    if(ip->type == T_DIR) {
        iunlockput(ip);
        return -1;
    }

    ip->nlink++; 
    iupdate(ip); 
    //iunlock(ip);

    if((dp = nameiparent(new_buf, name)) == 0) goto rollback;

    ivalid(dp);
    if(dirlink(dp, name, ip->inum) < 0) {
        iunlockput(dp);
        goto rollback;
    }

    iunlockput(dp);
    iput(ip);
    return 0; 

rollback:
    ivalid(ip);
    ip->nlink--;
    iupdate(ip);
    iunlockput(ip);
	if (dp) {
		iunlockput(dp);
	}
    return -1;
}

int sys_unlinkat(int dirfd, uint64 name, uint64 flags){
	//TODO: your job is to complete the syscall
struct inode *ip, *dp;
    char name_buf[DIRSIZ];
    char path_buf[MAXPATH];
    struct proc *p = curr_proc();
    uint off; // This will store the entry's location [cite: 40]

    if(copyinstr(p->pagetable, path_buf, name, MAXPATH) < 0) return -1;

    if((dp = nameiparent(path_buf, name_buf)) == 0) return -1;
    ivalid(dp);

    // Find the inode AND its offset in the parent directory
    if((ip = dirlookup(dp, name_buf, &off)) == 0) { 
        iput(dp);
        return -1; // File does not exist [cite: 55]
    }
    ivalid(ip);

    // Standard UNIX: cannot unlink directories with unlinkat
    if(ip->type == T_DIR) {
        iunlockput(ip);
        iunlockput(dp);
        return -1;
    }

    // Zero out the directory entry at the correct offset
    struct dirent de;
    memset(&de, 0, sizeof(de));
    if(writei(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de)) 
        panic("unlink: writei");
    
    iupdate(dp);
    iunlockput(dp);

    // Decrement link count and update disk [cite: 51, 83]
    if(ip->nlink > 0) {
        ip->nlink--; 
    }
    iupdate(ip);
    iunlockput(ip); // This must trigger deletion in fs.c if nlink == 0 [cite: 85, 86]

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
	case SYS_fstat:
	    ret = sys_fstat(args[0],args[1]);
		break;
	case SYS_linkat:
	    ret = sys_linkat(args[0],args[1],args[2],args[3],args[4]);
		break;
	case SYS_unlinkat:
	    ret = sys_unlinkat(args[0],args[1],args[2]);
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