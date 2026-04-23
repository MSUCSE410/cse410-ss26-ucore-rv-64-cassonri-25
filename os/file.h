#ifndef FILE_H
#define FILE_H

#include "fs.h"
#include "proc.h"
#include "types.h"

#define PIPESIZE (512)
#define FILEPOOLSIZE (NPROC * FD_BUFFER_SIZE)

// in-memory copy of an inode,it can be used to quickly locate file entities on disk
// This is the kernel's active cache of an inode. When a file is open, the kernel refers to this structure.
struct inode {
	uint dev; // Device number
	uint inum; // Inode number
	int ref; // Reference count - pointer count currently looking at that inode;kernel only counter
	int valid; // inode has been read from disk - true if so and filled structure with real disk data
	short type; // copy of disk inode - directory or file; what functions can be called on that inode
	// add nlink here so the kernel can increment/decrement it quickly without always hitting the disk.
	short nlink; // Number of links to inode in file system/Copy of disk link count
	uint size;
	uint addrs[NDIRECT + 1];
	// LAB4: You may need to add link count here
};

// Defines a file in memory that provides information about the current use of the file and the corresponding inode location
struct file {
	enum { FD_NONE = 0, FD_INODE, FD_STDIO } type;
	int ref; // reference count
	char readable;
	char writable;
	struct inode *ip; // FD_INODE
	uint off;
};

//A few specific fd
enum {
	STDIN = 0,
	STDOUT = 1,
	STDERR = 2,
};

extern struct file filepool[FILEPOOLSIZE];

void fileclose(struct file *);
struct file *filealloc();
int fileopen(char *, uint64);
uint64 inodewrite(struct file *, uint64, uint64);
uint64 inoderead(struct file *, uint64, uint64);
struct file *stdio_init(int);
int show_all_files();

#endif // FILE_H