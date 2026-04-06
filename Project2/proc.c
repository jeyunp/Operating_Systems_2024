#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "x86.h"
#include "proc.h"
#include "spinlock.h"


//Add: process queue structure
struct pqueue{
  struct proc* proc[NPROC];
  int rear;
};

//Modify: ptable now includes 4 MLFQ queue, one monopoly queue, MoQ/MLFQ mode variable
struct {
  struct spinlock lock;
  struct proc proc[NPROC];
  struct pqueue QL0, QL1, QL2, QL3;
  struct pqueue moq;
  int mode;      //if 0->MLFQ, 1->MoQ
} ptable;



static struct proc *initproc;

int nextpid = 1;
extern void forkret(void);
extern void trapret(void);

static void wakeup1(void *chan);

void
pinit(void)
{
  initlock(&ptable.lock, "ptable");
  //Initialize scheduling queues
  ptable.QL0.rear = 0;
  ptable.QL1.rear = 0;
  ptable.QL2.rear = 0;
  ptable.QL3.rear = 0;
  ptable.moq.rear = 0;
  ptable.mode=0;
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

//PAGEBREAK: 32
// Look in the process table for an UNUSED proc.
// If found, change state to EMBRYO and initialize
// state required to run in the kernel.
// Otherwise return 0.
static struct proc*
allocproc(void)
{
  struct proc *p;
  char *sp;

  acquire(&ptable.lock);

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == UNUSED)
      goto found;

  release(&ptable.lock);
  return 0;

found:
  p->state = EMBRYO;
  p->pid = nextpid++;

  release(&ptable.lock);

  // Allocate kernel stack.
  if((p->kstack = kalloc()) == 0){
    p->state = UNUSED;
    return 0;
  }
  sp = p->kstack + KSTACKSIZE;

  // Leave room for trap frame.
  sp -= sizeof *p->tf;
  p->tf = (struct trapframe*)sp;

  // Set up new context to start executing at forkret,
  // which returns to trapret.
  sp -= 4;
  *(uint*)sp = (uint)trapret;

  sp -= sizeof *p->context;
  p->context = (struct context*)sp;
  memset(p->context, 0, sizeof *p->context);
  p->context->eip = (uint)forkret;

  //Add for scheduling: When the process created, they goes into L0 queue.
  p->qlevel = L0;
  p->priority = 0;
  p->timeslice = 2;
  acquire(&ptable.lock);
  ptable.QL0.proc[ptable.QL0.rear] = p;
  ptable.QL0.rear = ptable.QL0.rear+1;
  release(&ptable.lock);

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
  memset(p->tf, 0, sizeof(*p->tf));
  p->tf->cs = (SEG_UCODE << 3) | DPL_USER;
  p->tf->ds = (SEG_UDATA << 3) | DPL_USER;
  p->tf->es = p->tf->ds;
  p->tf->ss = p->tf->ds;
  p->tf->eflags = FL_IF;
  p->tf->esp = PGSIZE;
  p->tf->eip = 0;  // beginning of initcode.S

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  // this assignment to p->state lets other cores
  // run this process. the acquire forces the above
  // writes to be visible, and the lock is also needed
  // because the assignment might not be atomic.
  acquire(&ptable.lock);

  p->state = RUNNABLE;

  release(&ptable.lock);
}

// Grow current process's memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  uint sz;
  struct proc *curproc = myproc();

  sz = curproc->sz;
  if(n > 0){
    if((sz = allocuvm(curproc->pgdir, sz, sz + n)) == 0)
      return -1;
  } else if(n < 0){
    if((sz = deallocuvm(curproc->pgdir, sz, sz + n)) == 0)
      return -1;
  }
  curproc->sz = sz;
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


  // Allocate process.
  if((np = allocproc()) == 0){
    return -1;
  }

  // Copy process state from proc.
  if((np->pgdir = copyuvm(curproc->pgdir, curproc->sz)) == 0){
    kfree(np->kstack);
    np->kstack = 0;
    np->state = UNUSED;
    return -1;
  }
  np->sz = curproc->sz;
  np->parent = curproc;
  *np->tf = *curproc->tf;

  // Clear %eax so that fork returns 0 in the child.
  np->tf->eax = 0;

  for(i = 0; i < NOFILE; i++)
    if(curproc->ofile[i])
      np->ofile[i] = filedup(curproc->ofile[i]);
  np->cwd = idup(curproc->cwd);

  safestrcpy(np->name, curproc->name, sizeof(curproc->name));

  pid = np->pid;

  acquire(&ptable.lock);

  np->state = RUNNABLE;

  release(&ptable.lock);

  return pid;
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait() to find out it exited.
void
exit(void)
{
  struct proc *curproc = myproc();
  struct proc *p;
  int fd;

  if(curproc == initproc)
    panic("init exiting");

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

  acquire(&ptable.lock);

  // Parent might be sleeping in wait().
  wakeup1(curproc->parent);

  // Pass abandoned children to init.
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->parent == curproc){
      p->parent = initproc;
      if(p->state == ZOMBIE)
        wakeup1(initproc);
    }
  }

  // Jump into the scheduler, never to return.
  curproc->state = ZOMBIE;
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
  int i, j;

  acquire(&ptable.lock);
  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->parent != curproc)
        continue;
      havekids = 1;
      if(p->state == ZOMBIE){
        // Found one.
        pid = p->pid;
        kfree(p->kstack);
        p->kstack = 0;
        freevm(p->pgdir);
        p->pid = 0;
        p->parent = 0;
        p->name[0] = 0;
        p->killed = 0;
        p->state = UNUSED;

        //Added: while reaping, we should remove the ripped process from the queue.
        //Divided into five cases: qlevel L0, L1, L2, L3, others(MoQ). Each case, the code is nearly identical
        if(p->qlevel == L0){
          for(i = 0; i<ptable.QL0.rear; i++){       //Find ripped process in level queue 0.
            if(ptable.QL0.proc[i]->state == UNUSED){
              for(j=i; j<ptable.QL0.rear-1; j++){         //Remove it from queue
                ptable.QL0.proc[j] = ptable.QL0.proc[j+1];
              }
              ptable.QL0.rear--;
              break;
            }
          } 
        } else if(p->qlevel == L1){
          for(i = 0; i<ptable.QL1.rear; i++){
            if(ptable.QL1.proc[i]->state == UNUSED){
              for(j=i; j<ptable.QL1.rear-1; j++){
                ptable.QL1.proc[j] = ptable.QL1.proc[j+1];
              }
              ptable.QL1.rear--;
              break;
            }
          }
        } else if(p->qlevel == L2){
          for(i = 0; i<ptable.QL2.rear; i++){
            if(ptable.QL2.proc[i]->state == UNUSED){
              for(j=i; j<ptable.QL2.rear-1; j++){
                ptable.QL2.proc[j] = ptable.QL2.proc[j+1];
              }
              ptable.QL2.rear--;
              break;
            }
          } 
        }else if(p->qlevel == L3){
          for(i = 0; i<ptable.QL3.rear; i++){
            if(ptable.QL3.proc[i]->state == UNUSED){
              for(j=i; j<ptable.QL3.rear-1; j++){
                ptable.QL3.proc[j] = ptable.QL3.proc[j+1];
              }
              ptable.QL3.rear--;
              break;
            }
          } 
        }else{
          for(i = 0; i<ptable.moq.rear; i++){
            if(ptable.moq.proc[i]->state == UNUSED){
              for(j=i; j<ptable.moq.rear-1; j++){
                ptable.moq.proc[j] = ptable.moq.proc[j+1];
              }
              ptable.moq.rear--;
              break;
            }
          } 
        }
        release(&ptable.lock);
        return pid;
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || curproc->killed){
      release(&ptable.lock);
      return -1;
    }

    // Wait for children to exit.  (See wakeup1 call in proc_exit.)
    sleep(curproc, &ptable.lock);  //DOC: wait-sleep
  }
}

//Check if specific level queue has a runnable process
int checkQueue(struct pqueue* pq){
  int i;
  for(i=0; i<pq->rear; i++){
    if(pq->proc[i]->state == RUNNABLE){
      return 1;
    }
  }
  return 0;
}

//Schedule in specific level queue. This is almost identical to original scheduler code, except it serves schedule in queue pq
void levelSchedule(struct pqueue* pq){
  int i;
  struct cpu *c = mycpu();
  struct proc *p;
  c->proc = 0;
  for(i=0; i<pq->rear; i++){
    if(pq->proc[i]->state != RUNNABLE)      //The firstmost RUNNABLE queue will be scheduled: it naturally implements priority scheduling with yield() funcition.
      continue;

    p = pq->proc[i];

    // Switch to chosen process.  It is the process's job
    // to release ptable.lock and then reacquire it
    // before jumping back to us.
    c->proc = p;
    switchuvm(p);
    p->state = RUNNING;

    swtch(&(c->scheduler), p->context);
    switchkvm();

    // Process is done running for now.
    // It should have changed its p->state before coming back.
    c->proc = 0;
    break;
  }

}

//Check if MoQ has unfinished process. If there is no process in MoQ or all processes are in ZOMBIE state, call unmonopolize() 
void checkMoQ(struct pqueue* pq){
  int i, check;
  check = 0;
  for(i=0; i<ptable.moq.rear; i++){
    if(ptable.moq.proc[i]->state != ZOMBIE)
      check = 1;
  }
  if(ptable.moq.rear==1)
    check=0;
  if(check == 0){
    release(&ptable.lock);          //Need to release lock because unmonopolize() acuires lock in itself.
    unmonopolize();
    acquire(&ptable.lock);
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
  for(;;){
    // Enable interrupts on this processor.
    sti();
    acquire(&ptable.lock);
    for(;;){
      if(ptable.mode == 1){         //MoQ mode
        checkMoQ(&ptable.moq);
        levelSchedule(&ptable.moq);
        break;                //need to break because we need to release lock: otherwiser, wakeup call will be blocked
      } else {                              //MLFQ Mode: Upper level will be scheduled first becuase upper level queue is checked first
        if(checkQueue(&ptable.QL0)){
          levelSchedule(&ptable.QL0);
          continue;                       //While excuting a scheduled process, other process can be added to L0 queue, so it needs continue;
        }else if(checkQueue(&ptable.QL1)){
          levelSchedule(&ptable.QL1);
          continue;
        }else if(checkQueue(&ptable.QL2)){
          levelSchedule(&ptable.QL2);
          continue;
        }else if(checkQueue(&ptable.QL3)){
          levelSchedule(&ptable.QL3);
          continue;
        }else{
          break;
        }
      }
    }
    release(&ptable.lock);
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
  struct proc *p = myproc();

  if(!holding(&ptable.lock))
    panic("sched ptable.lock");
  if(mycpu()->ncli != 1)
    panic("sched locks");
  if(p->state == RUNNING)
    panic("sched running");
  if(readeflags()&FL_IF)
    panic("sched interruptible");
  intena = mycpu()->intena;
  swtch(&p->context, mycpu()->scheduler);
  mycpu()->intena = intena;
}

//Remove specific pointer to PCB of processor p from level queue
void
removePCB(struct pqueue* pq, struct proc *p)
{
  int i=0;
  int j=0;
  while(pq->proc[i]->pid != p->pid)
    i++;
  //Remove PCB from ready queue
  for(j=i; j<(pq->rear-1); j++)
    pq->proc[j] = pq->proc[j+1];
  pq->proc[pq->rear] = 0;
  pq->rear--;
}

// Give up the CPU for one scheduling round.
void
yield(void)
{
  struct proc* p = myproc();
  int i, j;

  if(p->qlevel == MOQ){             //MoQ: no need to decrease timeslice
    acquire(&ptable.lock);
    p->state = RUNNABLE;
    sched();
    release(&ptable.lock);
  }else{
    if(p->timeslice!=0) {           //MLFQ: decrease timeslice. If it is not 0, then it can maintain its postion in the queue
      p->timeslice--;
    }else{                        //Change the queue if timeslice is exhausted
      acquire(&ptable.lock);
      if(p->qlevel == L0){            //case when Queue = L0
        if(p->pid%2 == 0){            //if pid== odd number, move to L2
          p->qlevel = L2;
          p->timeslice = 6;

          removePCB(&(ptable.QL0), p);
          ptable.QL2.proc[ptable.QL2.rear] = p;
          ptable.QL2.rear++;
        } else {                    //if pid==even number, move to L1
          p->qlevel = L1;
          p->timeslice = 4;

          removePCB((&ptable.QL0), p);
          ptable.QL1.proc[ptable.QL1.rear] = p;
          ptable.QL1.rear++;
        }
      } else{          //case when queue =L1, L2, L3. Remove from current queue and insert it to priority queue L3
        
        p->timeslice = 8;

        if(p->qlevel == L1){
          removePCB(&(ptable.QL1), p);
        }
        if(p->qlevel == L2){
          removePCB(&(ptable.QL2), p);

        }
          
        if(p->qlevel == L3){          //In L3, we need to decrease its priority
          if(p->priority > 0)
            p->priority--;
          removePCB(&(ptable.QL3), p);
        }

        p->qlevel = L3;

        //Queue which as upper priority will be located on front side.
        for(i=0; i<ptable.QL3.rear; i++){
          if(ptable.QL3.proc[i]->priority < p->priority)
            break;
        }
        for(j = ptable.QL3.rear; j>i; j--){
          ptable.QL3.proc[j] = ptable.QL3.proc[j-1];
        }
        ptable.QL3.proc[i] = p;
        ptable.QL3.rear++;

      }
      p->state = RUNNABLE;
      sched();        //Schedlue
      release(&ptable.lock);
    }
  }
}

// A fork child's very first scheduling by scheduler()
// will swtch here.  "Return" to user space.
void
forkret(void)
{
  static int first = 1;
  // Still holding ptable.lock from scheduler.
  release(&ptable.lock);

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
  struct proc *p = myproc();
  
  if(p == 0)
    panic("sleep");

  if(lk == 0)
    panic("sleep without lk");

  // Must acquire ptable.lock in order to
  // change p->state and then call sched.
  // Once we hold ptable.lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup runs with ptable.lock locked),
  // so it's okay to release lk.
  if(lk != &ptable.lock){  //DOC: sleeplock0
    acquire(&ptable.lock);  //DOC: sleeplock1
    release(lk);
  }
  // Go to sleep.
  p->chan = chan;
  p->state = SLEEPING;

  sched();

  // Tidy up.
  p->chan = 0;

  // Reacquire original lock.
  if(lk != &ptable.lock){  //DOC: sleeplock2
    release(&ptable.lock);
    acquire(lk);
  }
}

//PAGEBREAK!
// Wake up all processes sleeping on chan.
// The ptable lock must be held.
static void
wakeup1(void *chan)
{
  struct proc *p;

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == SLEEPING && p->chan == chan)
      p->state = RUNNABLE;
}

// Wake up all processes sleeping on chan.
void
wakeup(void *chan)
{
  acquire(&ptable.lock);
  wakeup1(chan);
  release(&ptable.lock);
}

// Kill the process with the given pid.
// Process won't exit until it returns
// to user space (see trap in trap.c).
int
kill(int pid)
{
  struct proc *p;

  acquire(&ptable.lock);
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->pid == pid){
      p->killed = 1;
      // Wake process from sleep if necessary.
      if(p->state == SLEEPING)
        p->state = RUNNABLE;
      release(&ptable.lock);
      return 0;
    }
  }
  release(&ptable.lock);
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

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    cprintf("%d %s %s %d", p->pid, state, p->name, p->qlevel);
    if(p->state == SLEEPING){
      getcallerpcs((uint*)p->context->ebp+2, pc);
      for(i=0; i<10 && pc[i] != 0; i++)
        cprintf(" %p", pc[i]);
    }
    cprintf("\n");
  }
}

//For priority boosting
void
boost(uint *gbltick)
{
  int i=0;
  if(*gbltick%100 == 0){          //if priority boosting condition is satisfied, it moves all processes in L1, L2, L3 to L0
    acquire(&ptable.lock);
    for(i = 0; i<ptable.QL1.rear; i++){
      ptable.QL0.proc[ptable.QL0.rear] = ptable.QL1.proc[i];
      ptable.QL0.proc[ptable.QL0.rear]->timeslice = 2;
      ptable.QL0.proc[ptable.QL0.rear]->qlevel = 0;
      ptable.QL0.rear++;
    }
    for(i = 0; i<ptable.QL2.rear; i++){
      ptable.QL0.proc[ptable.QL0.rear] = ptable.QL2.proc[i];
      ptable.QL0.proc[ptable.QL0.rear]->timeslice = 2;
      ptable.QL0.proc[ptable.QL0.rear]->qlevel = 0;
      ptable.QL0.rear++;
    }
    for(i = 0; i<ptable.QL3.rear; i++){
      ptable.QL0.proc[ptable.QL0.rear] = ptable.QL3.proc[i];
      ptable.QL0.proc[ptable.QL0.rear]->timeslice = 2;
      ptable.QL0.proc[ptable.QL0.rear]->qlevel = 0;
      ptable.QL0.rear++;
    }

    ptable.QL1.rear=0;
    ptable.QL2.rear=0;
    ptable.QL3.rear=0;
    release(&ptable.lock);
  }
}

//return 1 if MoQ, 0 if mlfq
int
mode_queue(void)
{
    return ptable.mode;
}

//Add system call
//set priority of a process(<-specified with pid)
int
setpriority(int pid, int priority)
{
  struct proc* p;
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->pid == pid){
      p->priority = priority;
      return 0;
    }
  }
  return -1;
}

//return level of a current process
int
getlev(void)
{
  if(myproc()->qlevel != MOQ)
    return myproc()->qlevel;
  return 99;
}

//MoQ->MLFQ mode
void
unmonopolize(void)
{
  acquire(&ptable.lock);
  ptable.mode = 0;
  release(&ptable.lock);
  acquire(&tickslock);
  ticks = 0;
  release(&tickslock);
}

//MLFQ->MoQ mode
void
monopolize(void)
{
  acquire(&ptable.lock);
  ptable.mode = 1;
  myproc()->state = RUNNABLE;
  cprintf("monopolize\n");
  sched();
  release(&ptable.lock);
}

//Set process p with pid==pid to MoQ process.
int
setmonopoly(int pid, int password)
{
  struct proc* p;
  int i;
  if(password != student_number)
    return -2;
  if(pid == myproc()->pid)
      return -4;
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->pid == pid){
      for(i=0; i<ptable.moq.rear; i++){           //if the target process already an MoQ process, return -3
        if(pid == ptable.moq.proc[i]->pid)
          return -3;
      }

      if(p->qlevel == L0){
        removePCB(&(ptable.QL0), p);
      }else if(p->qlevel == L1){
        removePCB(&(ptable.QL1), p);
      }else if(p->qlevel == L2){
        removePCB(&(ptable.QL2), p);
      }else if(p->qlevel == L3){
        removePCB(&(ptable.QL3), p);
      }
      p->qlevel = MOQ;
      ptable.moq.proc[ptable.moq.rear] = p;
      ptable.moq.rear++;
      return ptable.moq.rear;
    }
  }
  return -1;
}