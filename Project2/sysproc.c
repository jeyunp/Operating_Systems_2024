#include "types.h"
#include "x86.h"
#include "defs.h"
#include "date.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"

int
sys_fork(void)
{
  return fork();
}

int
sys_exit(void)
{
  exit();
  return 0;  // not reached
}

int
sys_wait(void)
{
  return wait();
}

int
sys_kill(void)
{
  int pid;

  if(argint(0, &pid) < 0)
    return -1;
  return kill(pid);
}

int
sys_getpid(void)
{
  return myproc()->pid;
}

int
sys_sbrk(void)
{
  int addr;
  int n;

  if(argint(0, &n) < 0)
    return -1;
  addr = myproc()->sz;
  if(growproc(n) < 0)
    return -1;
  return addr;
}

int
sys_sleep(void)
{
  int n;
  uint ticks0;

  if(argint(0, &n) < 0)
    return -1;
  acquire(&tickslock);
  ticks0 = ticks;
  while(ticks - ticks0 < n){
    if(myproc()->killed){
      release(&tickslock);
      return -1;
    }
    sleep(&ticks, &tickslock);
  }
  release(&tickslock);
  return 0;
}

// return how many clock tick interrupts have occurred
// since start.
int
sys_uptime(void)
{
  uint xticks;

  acquire(&tickslock);
  xticks = ticks;
  release(&tickslock);
  return xticks;
}

//Added: for MLFQ and monopoly scheduling
//Set priority of a process
int
sys_setpriority(void)
{
  int pid;
  int priority;
  if(argint(0, &pid) < 0)     //if no pid input
    return -1;
  if(argint(1, &priority) < 0)    //if no priority input
    return -1;
  if(priority > 10 || priority <0)    //priority out of range
    return -2;
  
  priority = setpriority(pid, priority);
  return priority;
}

//Get queue level of current process
int 
sys_getlev(void)
{
  return getlev();
}

//MoQ mode -> MLFQ mode
int 
sys_unmonopolize(void)
{
  unmonopolize();
  return 0;
}

//MLFQ mode -> MoQ mode
int
sys_monopolize(void)
{
  monopolize();
  return 0;
}

//Set a proccess as a MoQ process
int 
sys_setmonopoly(void)
{
  int pid;
  //char password[11];
  int password_int;
  //int i, cc;
  //char c;

  if(argint(0, &pid) < 0)       //No pid input
    return -1;
  if(argint(1, &password_int) < 0)    //No password input
    return -2;

  //If you like to get your PW from console...
  /*cprintf("%s", optstr);


  for(i=0; i+1 < 11; ){
    struct file* f= myproc()->ofile[0];
    cc = fileread(f, &c, 1);
    if(cc < 1)
      break;
    password[i++] = c;
    if(c == '\n' || c == '\r')
      break;
  }
  password[i] = '\0';
  if(password[0] == 0) // EOF
    return -2;
  
  password_int=0;
  i=0;
  while('0' <= password[i] && password[i] <= '9'){
    password_int = password_int*10 + password[i] - '0';
    i++;
  }*/
  //Other error cases will be handeled in setmonopoly()
  return setmonopoly(pid, password_int);
}

//yield current process
int
sys_yield(void)
{
  yield();
  return 0;
}