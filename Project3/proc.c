#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "x86.h"
#include "proc.h"
#include "spinlock.h"

struct {
  struct spinlock lock;
  struct proc proc[NPROC];
  struct thread thread[NTHREAD];
} pttable;

static struct proc *initproc;

int nextpid = 1;
int nexttid = 1;
extern void forkret(void);
extern void trapret(void);

static void wakeup1(void *chan);

void
pinit(void)
{
  initlock(&pttable.lock, "pttable");
}

// Must be called with interrupts disabled
int
cpuid() {
  return mycpu()-cpus;
}

// Must be called with interrupts disabled to avoid the caller being
// rescheduled between reading lapicid and running through the loop.
struct cpu*
mycpu(void)
{
  int apicid, i;
  
  if(readeflags()&FL_IF)
    panic("mycpu called with interrupts enabled\n");
  
  apicid = lapicid();
  // APIC IDs are not guaranteed to be contiguous. Maybe we should have
  // a reverse map, or reserve a register to store &cpus[i].
  for (i = 0; i < ncpu; ++i) {
    if (cpus[i].apicid == apicid)
      return &cpus[i];
  }
  panic("unknown apicid\n");
}

// Disable interrupts so that we are not rescheduled
// while reading proc from the cpu structure
struct proc*
myproc(void) {
  struct cpu *c;
  struct proc *p;
  pushcli();
  c = mycpu();
  p = c->proc;
  popcli();
  return p;
}

struct thread*
mythread(void) {
  struct cpu *c;
  struct thread *t;
  pushcli();
  c = mycpu();
  t = c->thread;
  popcli();
  return t;
}

//PAGEBREAK: 32
// Look in the process table for an UNUSED proc.
// If found, change state to EMBRYO and initialize
// state required to run in the kernel.
// Otherwise return 0.
static struct proc*
allocproc(void)
{
  struct proc *p;
  int i;

  acquire(&pttable.lock);

  for(p = pttable.proc; p < &pttable.proc[NPROC]; p++)
    if(p->state == UNUSED)
      goto found;

  release(&pttable.lock);
  return 0;

found:
  p->state = EMBRYO;
  p->pid = nextpid++;

  release(&pttable.lock);

  if((p->master = allocthread()) == 0){
    return 0;
  }

  p->master->tid=0;
  p->master->ustack=0;
  p->master->process = myproc();
  for(i = 0; i<NTHREAD; i++){
    p->workers[i] = 0;
  }

  //cprintf("allocproc: %d\n", p->pid);
  return p;
}

//PAGEBREAK: 32
// Set up first user process.
void
userinit(void)
{
  struct proc *p;
  extern char _binary_initcode_start[], _binary_initcode_size[];

  p = allocproc();
  
  initproc = p;
  if((p->pgdir = setupkvm()) == 0)
    panic("userinit: out of memory?");
  inituvm(p->pgdir, _binary_initcode_start, (int)_binary_initcode_size);
  p->sz = PGSIZE;
  memset(p->master->tf, 0, sizeof(*p->master->tf));
  p->master->tf->cs = (SEG_UCODE << 3) | DPL_USER;
  p->master->tf->ds = (SEG_UDATA << 3) | DPL_USER;
  p->master->tf->es = p->master->tf->ds;
  p->master->tf->ss = p->master->tf->ds;
  p->master->tf->eflags = FL_IF;
  p->master->tf->esp = PGSIZE;
  p->master->tf->eip = 0;  // beginning of initcode.S

  p->master->ustack = 0;
  p->master->process = p;

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  // this assignment to p->state lets other cores
  // run this process. the acquire forces the above
  // writes to be visible, and the lock is also needed
  // because the assignment might not be atomic.
  acquire(&pttable.lock);

  p->master->state = RUNNABLE;
  p->state = RUNNABLE;

  release(&pttable.lock);
}

// Grow current process's memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  uint sz;
  struct proc *curproc = myproc();

  acquire(&pttable.lock);
  sz = curproc->sz;
  if(n > 0){
    if((sz = allocuvm(curproc->pgdir, sz, sz + n)) == 0)
      return -1;
  } else if(n < 0){
    if((sz = deallocuvm(curproc->pgdir, sz, sz + n)) == 0)
      return -1;
  }
  curproc->sz = sz;
  release(&pttable.lock);
  switchuvm(curproc);
  return 0;
}

// Create a new process copying p as the parent.
// Sets up stack to return as if from system call.
// Caller must set state of returned proc to RUNNABLE.
int
fork(void)
{
  int i, pid;
  struct proc *np;
  struct proc *curproc = myproc();
  //struct thread *t;
  //struct thread *nt;
  struct thread *curthread = mythread();

  // Allocate process.
  if((np = allocproc()) == 0){
    cprintf("failed to alloc proc");
    return -1;
  }

  if(curthread == curproc->master){
    if((np->pgdir = copyuvm(curproc->pgdir, curproc->mastertop)) == 0){
      kfree(np->master->kstack);
      np->master->state = UNUSED;
      np->master->kstack = 0;
      np->master->process = 0;
      np->master = 0;
      np->state = UNUSED;
      cprintf("failed to copy uvm");
      return -1;
    }


    np->sz = curproc->sz;
    np->parent = curproc;
    np->master->process = np;
    *np->master->tf = *curproc->master->tf;
    np->mastertop = curproc->mastertop;
    np->threadtop = curproc->threadtop;
  }else{
    if((np->pgdir = copyuvm(curproc->pgdir, curproc->mastertop - 2*PGSIZE)) == 0){
      kfree(np->master->kstack);
      np->master->state = UNUSED;
      np->master->kstack = 0;
      np->master->process = 0;
      np->master = 0;
      np->state = UNUSED;
      cprintf("failed to copy uvm");
      return -1;
    }
    np->mastertop = curproc->mastertop;
    allocuvm(np->pgdir, np->mastertop - 2*PGSIZE, np->mastertop);
    if(copyout(np->pgdir, np->mastertop - 2*PGSIZE,  (void*)curproc->ustacks[curthread->ustack].addr, 2*PGSIZE) < 0){
      kfree(np->master->kstack);
      np->master->state = UNUSED;
      np->master->kstack = 0;
      np->master->process = 0;
      np->master = 0;
      np->state = UNUSED;
      freevm(np->pgdir);
    }
    np->sz = curproc->sz;
    np->parent = curproc;
    np->master->process = np;
    *np->master->tf = *curthread->tf;
    np->mastertop = curproc->mastertop;
    np->master->retval = curthread->retval;
    np->master->tf->esp = np->master->tf->esp - (curproc->ustacks[curthread->ustack].addr+2*PGSIZE-np->mastertop);
    np->threadtop = curproc->threadtop;
  }

  for(i = 0; i<NTHREAD; i++){
    np->ustacks[i].addr = curproc->ustacks[i].addr;
    np->ustacks[i].check = 0;
  }
  np->master->tf->eax = 0;

  allocuvm(np->pgdir, np->threadtop, np->sz);
  if(copyout(np->pgdir, np->threadtop,  (void*)curproc->threadtop, np->sz-np->threadtop) < 0){
    kfree(np->master->kstack);
    np->master->state = UNUSED;
    np->master->kstack = 0;
    np->master->process = 0;
    np->master = 0;
    np->state = UNUSED;
    freevm(np->pgdir);
  }

  for(i = 0; i < NOFILE; i++)
    if(curproc->ofile[i])
      np->ofile[i] = filedup(curproc->ofile[i]);
  np->cwd = idup(curproc->cwd);

  safestrcpy(np->name, curproc->name, sizeof(curproc->name));

  pid = np->pid;

  acquire(&pttable.lock);
  np->state = RUNNABLE;
  np->master->state = RUNNABLE;
  release(&pttable.lock);

  return pid;

  //Thread뿐 아니라 프로세스 주소공간 전체 fork 시.
  /*
  //cprintf("fork called!, curproc pid = %d, curthread process pid=%d\n", curproc->pid, curthread->process->pid);
  // Copy process state from proc.
  if((np->pgdir = copyuvm(curproc->pgdir, curproc->mastertop)) == 0){
    kfree(np->master->kstack);
    np->master->state = UNUSED;
    np->master->kstack = 0;
    np->master->process = 0;
    np->master = 0;
    np->state = UNUSED;
    cprintf("failed to copy uvm");
    return -1;
  }


  np->sz = curproc->sz;
  np->parent = curproc;
  np->master->process = np;
  *np->master->tf = *curproc->master->tf;

  np->mastertop = curproc->mastertop;
  np->threadtop = curproc->threadtop;
  for(i = 0; i<NTHREAD; i++){
    np->ustacks[i] = curproc->ustacks[i];
  }

  for(i=0; i<NTHREAD; i++){
    t = curproc->workers[i];
    if(t==0)
      continue;
    //cprintf("%d, %d, %d\n", i, np->ustacks[t->ustack].addr, np->ustacks[t->ustack].check);
    allocuvm(np->pgdir, np->ustacks[t->ustack].addr, np->ustacks[t->ustack].addr + 2*PGSIZE);
    if(copyout(np->pgdir, np->ustacks[t->ustack].addr, (void*)curproc->ustacks[t->ustack].addr, 2*PGSIZE) < 0){
      kfree(np->master->kstack);
      np->master->state = UNUSED;
      np->master->kstack = 0;
      np->master->process = 0;
      np->master = 0;
      np->state = UNUSED;
      freevm(np->pgdir);
    }
    np->ustacks[t->ustack].check = 1;
    if((nt = allocthread()) == 0){
      cprintf("failed to alloc thread");
      return -1;
    }
    nt->process = np;
    *nt->tf = *t->tf;
    nt->ustack = t->ustack;
    np->workers[i] = nt;
    nt->retval = t->retval;
  }
  //cprintf("thread copy fin\n");

  // Clear %eax so that fork returns 0 in the child.
  //fork를 호출한 thread에 대응되는 np의 thread에 대해 수행해야 함.
  //np->master->tf->eax = 0;
  if(curthread == curproc->master){
    np->master->tf->eax = 0;
  }
  for(i=0; i<NTHREAD; i++){
    if(curproc->workers[i] == curthread){
      np->workers[i]->tf->eax = 0;
    }
  }

  for(i = 0; i < NOFILE; i++)
    if(curproc->ofile[i])
      np->ofile[i] = filedup(curproc->ofile[i]);
  np->cwd = idup(curproc->cwd);

  safestrcpy(np->name, curproc->name, sizeof(curproc->name));

  

  pid = np->pid;
  //cprintf("np parent:%d\n", np->parent->pid);
  acquire(&pttable.lock);

  //근데 이러면 깨우는것도 바꿔놔야 하는거 아닌가->일단 놔두자
  np->state = RUNNABLE;
  if(curproc->master == curthread){
    np->master->state = RUNNABLE;
  } else{
    np->master->state = curproc->master->state;
  }
  for(i = 0; i<NTHREAD; i++){
    if(curproc->workers[i] == curthread){
      np->workers[i]->state = RUNNABLE;
    }else{
      np->workers[i]->state = ZOMBIE;
    }
    
  }


  release(&pttable.lock);

  return pid;*/
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait() to find out it exited.
void
exit(void)
{
  struct proc *curproc = myproc();
  struct proc *p;
  struct thread *curthread = mythread();
  struct thread *t;
  int fd;

  if(curproc == initproc)
    panic("init exiting");

  for(t = pttable.thread; t < &pttable.thread[NTHREAD]; t++){       //새로만든부분
      if(t->process != curthread->process)
        continue;

      if(t->tid != curthread->tid)     //현재 thread가 아닌 경우
        t->state = ZOMBIE;
  }

  // Close all open files.
  for(fd = 0; fd < NOFILE; fd++){
    if(curproc->ofile[fd]){
      fileclose(curproc->ofile[fd]);
      curproc->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(curproc->cwd);
  end_op();
  curproc->cwd = 0;

  acquire(&pttable.lock);

  // Parent might be sleeping in wait(). 고쳐야하나...? -> curproc 넣고 자는 경우는 wait 뿐
  wakeup1(curproc->parent);

  // Pass abandoned children to init.
  for(p = pttable.proc; p < &pttable.proc[NPROC]; p++){
    if(p->parent == curproc){
      p->parent = initproc;
      if(p->state == ZOMBIE)
        wakeup1(initproc);
    }
  }
  // Jump into the scheduler, never to return.
  curproc->state = ZOMBIE;
  curthread->state = ZOMBIE;
  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
wait(void)
{
  struct proc *p;
  int havekids, pid;
  struct proc *curproc = myproc();
  struct thread *t;
  int i;
  
  acquire(&pttable.lock);
  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(p = pttable.proc; p < &pttable.proc[NPROC]; p++){
      if(p->parent != curproc){
        continue;
      }
        
      havekids = 1;
      if(p->state == ZOMBIE){
        // Found one.
        pid = p->pid;
        for(i=0; i<NTHREAD; i++){
          t = p->workers[i];
          if(t==0)
            continue;
          //deallocuvm(t->process->pgdir, t->process->ustacks[t->ustack].addr +2*PGSIZE, t->process->ustacks[t->ustack].addr);
          kfree(t->kstack);
        }
        kfree(p->master->kstack);
        p->master->kstack = 0;
        freevm(p->pgdir);
        p->master->process = 0;
        p->master->state = UNUSED;
        for(i = 0;i<NTHREAD; i++){
          p->workers[i]->state=UNUSED;
          p->workers[i] = 0;
        }
        p->master = 0;
        p->pid = 0;
        p->parent = 0;
        p->name[0] = 0;
        p->killed = 0;
        p->state = UNUSED;
        release(&pttable.lock);
        return pid;
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || curproc->killed){
      release(&pttable.lock);
      return -1;
    }

    // Wait for children to exit.  (See wakeup1 call in proc_exit.)
    sleep(curproc, &pttable.lock);  //DOC: wait-sleep
    //cprintf("wait awaken\n");
  }
}

//PAGEBREAK: 42
// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run
//  - swtch to start running that process
//  - eventually that process transfers control
//      via swtch back to the scheduler.
void
scheduler(void)
{
  struct thread *t;
  struct cpu *c = mycpu();
  c->thread = 0;
  c->proc = 0;
  
  for(;;){
    // Enable interrupts on this processor.
    sti();

    // Loop over process table looking for process to run.
    acquire(&pttable.lock);
    for(t = pttable.thread; t < &pttable.thread[NTHREAD]; t++){
      if(t->state != RUNNABLE)
        continue;

      // Switch to chosen process.  It is the process's job
      // to release ptable.lock and then reacquire it
      // before jumping back to us.
      c->thread = t;
      c->proc = t->process;
      switchuvm(t->process);
      t->state = RUNNING;


      swtch(&(c->scheduler), t->context);
      switchkvm();

      // Process is done running for now.
      // It should have changed its p->state before coming back.
      c->thread = 0;
    }
    release(&pttable.lock);

  }
}

// Enter scheduler.  Must hold only ptable.lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->ncli, but that would
// break in the few places where a lock is held but
// there's no process.
void
sched(void)
{
  int intena;
  struct thread *t = mythread();

  if(!holding(&pttable.lock))
    panic("sched pttable.lock");
  if(mycpu()->ncli != 1)
    panic("sched locks");
  if(t->state == RUNNING)
    panic("sched running");
  if(readeflags()&FL_IF)
    panic("sched interruptible");
  intena = mycpu()->intena;
  swtch(&t->context, mycpu()->scheduler);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void
yield(void)
{
  acquire(&pttable.lock);  //DOC: yieldlock
  mythread()->state = RUNNABLE;
  sched();
  release(&pttable.lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch here.  "Return" to user space.
void
forkret(void)
{
  static int first = 1;
  // Still holding ptable.lock from scheduler.
  release(&pttable.lock);
  //cprintf("forkret\n");
  if (first) {
    // Some initialization functions must be run in the context
    // of a regular process (e.g., they call sleep), and thus cannot
    // be run from main().
    first = 0;
    iinit(ROOTDEV);
    initlog(ROOTDEV);
  }

  // Return to "caller", actually trapret (see allocproc).
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
void
sleep(void *chan, struct spinlock *lk)
{
  struct thread *t = mythread();
  
  if(t == 0)
    panic("sleep");

  if(lk == 0)
    panic("sleep without lk");

  // Must acquire ptable.lock in order to
  // change p->state and then call sched.
  // Once we hold ptable.lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup runs with ptable.lock locked),
  // so it's okay to release lk.
  if(lk != &pttable.lock){  //DOC: sleeplock0
    acquire(&pttable.lock);  //DOC: sleeplock1
    release(lk);
  }
  // Go to sleep.
  t->chan = chan;
  t->state = SLEEPING;

  //if(t->tid!=0)
  //  cprintf("worker thread sleep!\n");
  sched();

  // Tidy up.
  t->chan = 0;

  // Reacquire original lock.
  if(lk != &pttable.lock){  //DOC: sleeplock2
    release(&pttable.lock);
    acquire(lk);
  }
}

//PAGEBREAK!
// Wake up all processes sleeping on chan.
// The ptable lock must be held.
static void
wakeup1(void *chan)
{
  struct thread *t;

  for(t = pttable.thread; t < &pttable.thread[NTHREAD]; t++)
    if(t->state == SLEEPING && t->chan == chan)
      t->state = RUNNABLE;
}

// Wake up all processes sleeping on chan.
void
wakeup(void *chan)
{
  acquire(&pttable.lock);
  wakeup1(chan);
  release(&pttable.lock);
}

// Kill the process with the given pid.
// Process won't exit until it returns
// to user space (see trap in trap.c).
int
kill(int pid)
{
  struct proc *p;
  struct thread *t;

  acquire(&pttable.lock);
  for(p = pttable.proc; p < &pttable.proc[NPROC]; p++){
    if(p->pid == pid){
      p->killed = 1;
      // Wake process/thread from sleep if necessary.
      for(t = pttable.thread; t < &pttable.thread[NTHREAD]; t++){
        if(t->process == p){
          if(t->state == SLEEPING)
          t->state = RUNNABLE;
        }
      }
      release(&pttable.lock);
      return 0;
    }
  }
  release(&pttable.lock);
  return -1;
}

//PAGEBREAK: 36
// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(void)
{
  static char *states[] = {
  [UNUSED]    "unused",
  [EMBRYO]    "embryo",
  [SLEEPING]  "sleep ",
  [RUNNABLE]  "runble",
  [RUNNING]   "run   ",
  [ZOMBIE]    "zombie"
  };
  int i;
  struct proc *p;
  char *state;
  uint pc[10];

  for(p = pttable.proc; p < &pttable.proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    cprintf("%d %s %s", p->pid, state, p->name);
    if(p->state == SLEEPING){
//      getcallerpcs((uint*)p->context->ebp+2, pc);
      for(i=0; i<10 && pc[i] != 0; i++)
        cprintf(" %p", pc[i]);
    }
    cprintf("\n");
  }
}

/*
1)ustack 만든다
2)exec에서처럼 page 2개 할당해 ustack 만들어줌
3)kstack 만든다:
*/

int
thread_create(thread_t *thread, void *(*start_routine)(void*), void *arg)
{
  struct thread *t;
  struct thread *curthread = mythread();
  struct proc *curproc = myproc();
  pte_t *pgdir;
  uint sp, ustack[3+1+1];
  int i;

  // Allocate thread.
  if((t = allocthread()) == 0){
    cprintf("Can't alloc thread!");
    return -1;
  }

  acquire(&pttable.lock);
  sp=0;
  for(i=0; i<NTHREAD; i++){
    if((curproc->ustacks[i]).check == 0){
      (curproc->ustacks[i]).check = 1;
      t->ustack = i;
      sp = (curproc->ustacks[i]).addr;
      break;
    }
  }
  release(&pttable.lock);

  pgdir = curproc->pgdir;
  sp = PGROUNDUP(sp);
  if((sp = allocuvm(pgdir, sp, sp + 2*PGSIZE)) == 0){
    cprintf("allocuvm failed!!!\n");
    goto bad;
  }
  clearpteu(pgdir, (char*)(sp - 2*PGSIZE));

  sp = sp-8;
  ustack[0] = 0xffffffff;  // fake return PC
  ustack[1] = (uint)arg;


  if(copyout(pgdir, sp, ustack, (2)*4) < 0){
    cprintf("copyout failed!->2");
    goto bad;
  }

  // Commit to the user image.
  //curproc->sz = curproc->sz + 2*PGSIZE;
  *t->tf = *curproc->master->tf;
  t->tf->eip = (uint)start_routine;  // main
  t->tf->esp = sp;
  t->process = curthread->process;
  
  //process의 소속 thread 배열의 빈 칸에 thread집어넣기 
  for(i=0; i<NTHREAD; i++){
    if(curproc->workers[i]==0){
      curproc->workers[i]=t;
      break;
    }
  }

  *thread = t->tid;

  //eax 초기화 x -> child는 return이 아니라 새로운 위치서 시작()
  acquire(&pttable.lock);
  t->state = RUNNABLE;
  release(&pttable.lock);
  
  return 0;

 bad:
  cprintf("bad!!!\n");
  return -1;
}

struct thread*
allocthread(void) 
{
  struct thread *t;
  char *sp;
  //struct proc *curproc = myproc();

  acquire(&pttable.lock);

  for(t = pttable.thread; t < &pttable.thread[NTHREAD]; t++)
    if(t->state == UNUSED)
      goto found;

  release(&pttable.lock);
  //error
  cprintf("\nNo threads left, nexttid = %d", nexttid);
  return 0;

found:
  t->state = EMBRYO;
  t->tid = nexttid++;

  release(&pttable.lock);

  // Allocate kernel stack.
  if((t->kstack = kalloc()) == 0){
    t->state = UNUSED;
    cprintf("can't alloc kstack");
    return 0;
  }
  sp = t->kstack + KSTACKSIZE;

  // Leave room for trap frame.
  sp -= sizeof *t->tf;
  t->tf = (struct trapframe*)sp;

  // Set up new context to start executing at forkret,
  // which returns to trapret.
  sp -= 4;
  *(uint*)sp = (uint)trapret;

  sp -= sizeof *t->context;
  t->context = (struct context*)sp;
  memset(t->context, 0, sizeof *t->context);
  t->context->eip = (uint)forkret;

  return t;
}


void thread_exit(void *retval)
{
  struct thread* curthread = mythread();
  curthread->retval = retval;
  curthread->state = ZOMBIE;
  wakeup(curthread);
  acquire(&pttable.lock);
  sched();
  panic("zombie exit");
}


int
thread_join(thread_t thread, void **retval)
{
  struct thread* t;
  int i;

  acquire(&pttable.lock);
  for(t = pttable.thread; t < &pttable.thread[NTHREAD]; t++){
    if(t->tid != thread)
      continue;
    break;
  }

  if(t == &pttable.thread[NTHREAD]){
    return -1;
  }

  if(t->state != ZOMBIE)
    sleep(t, &pttable.lock);

  *retval = t->retval;
  


  //worker array서 제거
  for(i=0; i<NTHREAD; i++){
    if(t->process->workers[i]->tid==thread){
      t->process->workers[i]->tid=0;
      t->process->workers[i]=0;
      break;
    }
  }
  kfree(t->kstack);
  t->kstack = 0;
  deallocuvm(t->process->pgdir, t->process->ustacks[t->ustack].addr +2*PGSIZE, t->process->ustacks[t->ustack].addr);
  t->process->ustacks[t->ustack].check=0;

  t->ustack = 0;
  t->tid = 0;
  t->process = 0;
  t->state = UNUSED;
  t->retval = 0;
  t->ustack = 0;
  release(&pttable.lock);
  return 0;
}