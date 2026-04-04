#include "types.h"
#include "defs.h"
#include "mmu.h"
#include "param.h"
#include "proc.h"

int getgpid(void) {
	return myproc()->parent->parent->pid;
}

int sys_getgpid(void) {
	return getgpid();
}
