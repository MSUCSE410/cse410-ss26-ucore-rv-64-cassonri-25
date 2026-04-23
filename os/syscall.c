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
// Purpose: Retrieves metadata about an open file. This is how the user-space program verifies that nlink increased after a linkat call.
int sys_fstat(int fd,uint64 stat){
	//TODO: your job is to complete the syscall
	struct file *f;
    struct proc *p = curr_proc();
    Stat st;
	// 1. Verify file descriptor is valid and belongs to the current process
    if(fd < 0 || fd >= NFILE || (f = p->files[fd]) == 0) return -1;
	// 2. Ensure the in-memory inode has the freshest data from disk
    ivalid(f->ip); // Use ivalid instead of ilock
	// 3. Populate the Stat structure with metadata from the inode
    st.dev = 0;
    st.ino = f->ip->inum;
    st.nlink = f->ip->nlink; //Report current link count
	st.size = f->ip->size;
	// 4. Set mode bits (Standard bits for ucore tests)
    st.mode = (f->ip->type == T_DIR) ? 0x040000 : 0x100000;
    //iunlock(f->ip);
	// 5. Copy the results from kernel space back to user space memory
    if(copyout(p->pagetable, stat, (char *)&st, sizeof(st)) < 0) return -1;
    return 0;
}

//Purpose: Creates a "Hard Link." It gives an existing file (inode) a new name in a directory, effectively increasing its nlink count.
int sys_linkat(int olddirfd, uint64 oldpath, int newdirfd, uint64 newpath, uint64 flags){
	//TODO: your job is to complete the syscall
	char old_buf[MAXPATH], new_buf[MAXPATH]; // Use unique names for local buffers
    struct inode *ip, *dp;
    char name[DIRSIZ];
    struct proc *p = curr_proc();

    // Use the 'oldpath' and 'newpath' parameters directly [cite: 20, 21]
	// 1. Copy paths from user-space into kernel buffers(using provided addresses)
    if(copyinstr(p->pagetable, old_buf, oldpath, MAXPATH) < 0 ||
       copyinstr(p->pagetable, new_buf, newpath, MAXPATH) < 0) {
        return -1;
    }
	// 2. Locate the existing file (the "old" file)
    if((ip = namei(old_buf)) == 0) return -1; 
	// 3. Basic Safety: UNIX does not typically allow hard links to directories
    ivalid(ip);
    if(ip->type == T_DIR) {
        iunlockput(ip);
        return -1;
    }
	// 4. Increment link count and sync to disk immediately
    // If the system crashes now, the file just has an "extra" link that will be cleaned later
    ip->nlink++; 
    iupdate(ip); 
    //iunlock(ip);
	// 5. Find the parent directory for the "new" link name
    if((dp = nameiparent(new_buf, name)) == 0) goto rollback;
	// 6. Add the new entry to the parent directory pointing to the old inode
    ivalid(dp);
    if(dirlink(dp, name, ip->inum) < 0) {
        iunlockput(dp);
        goto rollback;
    }
	// 7. Cleanup: Release the directory and the inode reference (namei gave us 1 ref)
    iunlockput(dp);
    iput(ip);
    return 0; 

rollback:
	// If directory entry creation failed, we MUST undo the nlink increment
    ivalid(ip);
    ip->nlink--;
    iupdate(ip);
    iunlockput(ip);
	if (dp) {
		iunlockput(dp);
	}
    return -1;
}
// Purpose: Removes a name (link) from a directory. If this was the last name (nlink == 0) and no processes are using the file, iput will trigger the actual deletion.
int sys_unlinkat(int dirfd, uint64 name, uint64 flags){
	//TODO: your job is to complete the syscall
struct inode *ip, *dp;
    char name_buf[DIRSIZ];
    char path_buf[MAXPATH];
    struct proc *p = curr_proc();
	// Stores the byte offset of the directory entry we are removing
    uint off; // This will store the entry's location [cite: 40]
	// 1. Copy the path to the file being unlinked
    if(copyinstr(p->pagetable, path_buf, name, MAXPATH) < 0) return -1;
	// 2. Find the parent directory containing the file
    if((dp = nameiparent(path_buf, name_buf)) == 0) return -1;
    ivalid(dp);

    // Find the inode AND its offset in the parent directory
	// 3. Find the file's inode and, critically, its 'off' (position) in the directory
    if((ip = dirlookup(dp, name_buf, &off)) == 0) { 
        iput(dp);
        return -1; // File does not exist [cite: 55]
    }
    ivalid(ip);

    // Standard UNIX: cannot unlink directories with unlinkat
	// 4. Safety: Standard unlink cannot remove directories (use rmdir for that)
    if(ip->type == T_DIR) {
        iunlockput(ip);
        iunlockput(dp);
        return -1;
    }

    // Zero out the directory entry at the correct offset
	// 5. Remove the name from the parent directory by zeroing the entry at 'off'
    struct dirent de;
    memset(&de, 0, sizeof(de));
    if(writei(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de)) 
        panic("unlink: writei");
    // 6. Update the directory's metadata and release it
    iupdate(dp);
    iunlockput(dp);

    // Decrement link count and update disk [cite: 51, 83]
	// 7. Decrement the link count of the file itself
    if(ip->nlink > 0) {
        ip->nlink--; 
    }
	// 8. Sync the new link count to disk
    iupdate(ip);
	// 9. Drop the reference. If ip->nlink is now 0, iput() in fs.c 
    // will see this and finally free the disk blocks (itrunc).
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