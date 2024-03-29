#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
//#include "queue.h"

//queue implementation










#define WAIT_TIME 25 // max ticks a process waits for

struct cpu cpus[NCPU];

struct proc proc[NPROC];

struct proc *initproc;

int nextpid = 1;
struct spinlock pid_lock;

extern void forkret(void);
static void freeproc(struct proc *p);

extern char trampoline[]; // trampoline.S


// queues for mlfq
/*struct queue* q0;
struct queue* q1;
struct queue* q2;
struct queue* q3;
struct queue* q4;*/
struct proc* q_arr[5][NPROC];
int q_sizes[5]={0};
int max_run_times[5]={1,2,4,8,16}; // time slice per queue


void shift_front(int qnum)
{
  for(int i=1;i<q_sizes[qnum];i++)
  {
    q_arr[qnum][i-1] = q_arr[qnum][i];
  }
}

void shift_back(int qnum)
{
  for(int i=q_sizes[qnum];i>0;i--)
  {
    q_arr[qnum][i] = q_arr[qnum][i-1];
  }
}



// pseudo-random number generator seed, only use variables with this lock
struct spinlock psrg_lock; //is this lock local???
int psrg_seed=27;
int multiplier=6;
int increment=7;



// helps ensure that wakeups of wait()ing
// parents are not lost. helps obey the
// memory model when using p->parent.
// must be acquired before any p->lock.
struct spinlock wait_lock;

// Allocate a page for each process's kernel stack.
// Map it high in memory, followed by an invalid
// guard page.
void
proc_mapstacks(pagetable_t kpgtbl)
{
  struct proc *p;
  
  for(p = proc; p < &proc[NPROC]; p++) {
    char *pa = kalloc();
    if(pa == 0)
      panic("kalloc");
    uint64 va = KSTACK((int) (p - proc));
    kvmmap(kpgtbl, va, (uint64)pa, PGSIZE, PTE_R | PTE_W);
  }
}

// initialize the proc table and process queues
void
procinit(void)
{
  struct proc *p;
  
  initlock(&pid_lock, "nextpid");
  initlock(&wait_lock, "wait_lock");
  initlock(&psrg_lock,"psrg");


  for(p = proc; p < &proc[NPROC]; p++) {
      initlock(&p->lock, "proc");
      p->state = UNUSED;
      p->kstack = KSTACK((int) (p - proc));
  }
}


int modify_queues()
{
  // increment wait ticks in qs and age and upgrade process
  for(int i=1;i<5;i++)
  {
    if(q_sizes[i]!=0) //checking if the q has processes in it
    {
      int q_size=q_sizes[i];
      int iter=0;

      while(iter<q_size)
      {
        struct proc* wp = q_arr[i][iter]; //process for that node

        acquire(&wp->lock); 
        wp->q_wait_time++;
        if(wp->q_wait_time>=WAIT_TIME) //if it has exceeded wait time push it up 
        {
          wp->q_wait_time=0;
          wp->recent_run_time=0;
          shift_front(wp->q_num); //popping
          q_sizes[wp->q_num]--;
          iter--;

          wp->q_num--;
          q_arr[wp->q_num][q_sizes[wp->q_num]]=wp; //pushed in higher priority
          q_sizes[wp->q_num]++;
          
        }
        release(&wp->lock);
        iter++;
      }
    }
  }

  //  increment process runtime and check for preemption
  struct proc* running_p=myproc(); // currently running process
  if(running_p!=0)
  {
    running_p->recent_run_time++; // incremening run time
    if(running_p->recent_run_time==max_run_times[running_p->q_num]) // checking
    {
      if(running_p->q_num!=4) //except for last q
      {
        running_p->q_num++;
      }
      yield();
      return 0;
    }
  }



  // check for higher priority processes and run them ,push front currently running proc
  if(myproc()!=0)
  {
    for(int i=0;i<myproc()->q_num;i++)
    {
      if(q_sizes[i]!=0) //checking if the q has processes in it
      {
        if(myproc()->q_num > i) //if a higher priority process exists, preempt!!
        {
          //push front here and set inq to 1
          yield();
        }
      }
    }
  }
  return 0;

}


// Must be called with interrupts disabled,
// to prevent race with process being moved
// to a different CPU.
int
cpuid()
{
  int id = r_tp();
  return id;
}

// Return this CPU's cpu struct.
// Interrupts must be disabled.
struct cpu*
mycpu(void)
{
  int id = cpuid();
  struct cpu *c = &cpus[id];
  return c;
}

// Return the current struct proc *, or zero if none.
struct proc*
myproc(void)
{
  push_off();
  struct cpu *c = mycpu();
  struct proc *p = c->proc;
  pop_off();
  return p;
}


// sets the new static priority of a process with pid, returns -1 in case pid(RUNNABLE) doesnt exist
int set_priority(int new_priority,int pid)
{
  int old_sp=-1;
  struct proc* p;

  //finding the process with that pid(pre-emption???)
  for (p = proc; p < &proc[NPROC]; p++)
  {
    acquire(&p->lock);
    if (p->state == RUNNABLE)
    {
      if(p->pid==pid)
      {
        old_sp=p->sp;
        p->sp=new_priority;
        p->niceness=5;
        // reset run time and sleep time
        release(&p->lock);
        break;
      }
    }
    release(&p->lock);
  }
  return old_sp;
}


int
allocpid()
{
  int pid;
  
  acquire(&pid_lock);
  pid = nextpid;
  nextpid = nextpid + 1;
  release(&pid_lock);

  return pid;
}

// Look in the process table for an UNUSED proc.
// If found, initialize state required to run in the kernel,
// and return with p->lock held.
// If there are no free procs, or a memory allocation fails, return 0.
static struct proc*
allocproc(void)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if(p->state == UNUSED) {
      goto found;
    } else {
      release(&p->lock);
    }
  }
  return 0;

found:
  p->pid = allocpid();
  p->state = USED;

  // Allocate a trapframe page.
  if((p->trapframe = (struct trapframe *)kalloc()) == 0){
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // An empty user page table.
  p->pagetable = proc_pagetable(p);
  if(p->pagetable == 0){
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // Set up new context to start executing at forkret,
  // which returns to user space.
  memset(&p->context, 0, sizeof(p->context));
  p->context.ra = (uint64)forkret;
  p->context.sp = p->kstack + PGSIZE;

  p->trac_stat = 0;
  p->nticks = 0;
  p->ticklim = 0;
  p->alarm_lock = 0;

  //setting creation time(tick number)
  p->start_tick=ticks;
  p->tickets=1;

  //setting up scheduling variables
  p->sched_times=0;
  p->recent_run_ticks=0;
  p->niceness=5; //default niceness is 5
  p->sp=60;
  p->dp=60;

  // set q_wait_time=0 , recent_run_time=0 , and queue number=0
  p->q_wait_time=0;
  p->q_num=0;
  p->recent_run_time=0;
  p->inq=0;

  p->rtime = 0;
  p->etime = 0;
  p->ctime = ticks;

  return p;
}

// free a proc structure and the data hanging from it,
// including user pages.
// p->lock must be held.
static void
freeproc(struct proc *p)
{
  if(p->trapframe)
    kfree((void*)p->trapframe);
  p->trapframe = 0;
  if(p->pagetable)
    proc_freepagetable(p->pagetable, p->sz);
  p->pagetable = 0;
  p->sz = 0;
  p->pid = 0;
  p->trac_stat = 0;
  p->parent = 0;
  p->name[0] = 0;
  p->chan = 0;
  p->killed = 0;
  p->xstate = 0;
  p->nticks = 0;
  p->ticklim = 0;
  p->alarm_lock = 0;
  p->start_tick=0; 
  p->sched_times=0;
  p->recent_run_ticks=0;
  p->niceness=5;
  p->sp=60;
  p->dp=60;
  p->tickets=1;
  // free(p->fn);
  p->state = UNUSED;
}

// Create a user page table for a given process, with no user memory,
// but with trampoline and trapframe pages.
pagetable_t
proc_pagetable(struct proc *p)
{
  pagetable_t pagetable;

  // An empty page table.
  pagetable = uvmcreate();
  if(pagetable == 0)
    return 0;

  // map the trampoline code (for system call return)
  // at the highest user virtual address.
  // only the supervisor uses it, on the way
  // to/from user space, so not PTE_U.
  if(mappages(pagetable, TRAMPOLINE, PGSIZE,
              (uint64)trampoline, PTE_R | PTE_X) < 0){
    uvmfree(pagetable, 0);
    return 0;
  }

  // map the trapframe page just below the trampoline page, for
  // trampoline.S.
  if(mappages(pagetable, TRAPFRAME, PGSIZE,
              (uint64)(p->trapframe), PTE_R | PTE_W) < 0){
    uvmunmap(pagetable, TRAMPOLINE, 1, 0);
    uvmfree(pagetable, 0);
    return 0;
  }

  return pagetable;
}

// Free a process's page table, and free the
// physical memory it refers to.
void
proc_freepagetable(pagetable_t pagetable, uint64 sz)
{
  uvmunmap(pagetable, TRAMPOLINE, 1, 0);
  uvmunmap(pagetable, TRAPFRAME, 1, 0);
  uvmfree(pagetable, sz);
}

// a user program that calls exec("/init")
// assembled from ../user/initcode.S
// od -t xC ../user/initcode
uchar initcode[] = {
  0x17, 0x05, 0x00, 0x00, 0x13, 0x05, 0x45, 0x02,
  0x97, 0x05, 0x00, 0x00, 0x93, 0x85, 0x35, 0x02,
  0x93, 0x08, 0x70, 0x00, 0x73, 0x00, 0x00, 0x00,
  0x93, 0x08, 0x20, 0x00, 0x73, 0x00, 0x00, 0x00,
  0xef, 0xf0, 0x9f, 0xff, 0x2f, 0x69, 0x6e, 0x69,
  0x74, 0x00, 0x00, 0x24, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00
};

// Set up first user process.
void
userinit(void)
{
  struct proc *p;

  p = allocproc();
  initproc = p;
  
  // allocate one user page and copy initcode's instructions
  // and data into it.
  uvmfirst(p->pagetable, initcode, sizeof(initcode));
  p->sz = PGSIZE;

  // prepare for the very first "return" from kernel to user.
  p->trapframe->epc = 0;      // user program counter
  p->trapframe->sp = PGSIZE;  // user stack pointer

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  p->state = RUNNABLE;
  //printf("first user process\n");
  
  release(&p->lock);
}

// Grow or shrink user memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  uint64 sz;
  struct proc *p = myproc();

  sz = p->sz;
  if(n > 0){
    if((sz = uvmalloc(p->pagetable, sz, sz + n, PTE_W)) == 0) {
      return -1;
    }
  } else if(n < 0){
    sz = uvmdealloc(p->pagetable, sz, sz + n);
  }
  p->sz = sz;
  return 0;
}

// Create a new process, copying the parent.
// Sets up child kernel stack to return as if from fork() system call.
int
fork(void)
{
  int i, pid;
  struct proc *np;
  struct proc *p = myproc();

  // Allocate process.
  if((np = allocproc()) == 0){
    return -1;
  }

  // Copy user memory from parent to child.
  if(uvmcopy(p->pagetable, np->pagetable, p->sz) < 0){
    freeproc(np);
    release(&np->lock);
    return -1;
  }
  np->sz = p->sz;

  // copy saved user registers.
  *(np->trapframe) = *(p->trapframe);

  //copy strace
  np->trac_stat = p->trac_stat;

  // inherit same number of tickets as parent
  np->tickets=p->tickets;

  // Cause fork to return 0 in the child.
  np->trapframe->a0 = 0;

  // increment reference counts on open file descriptors.
  for(i = 0; i < NOFILE; i++)
    if(p->ofile[i])
      np->ofile[i] = filedup(p->ofile[i]);
  np->cwd = idup(p->cwd);

  safestrcpy(np->name, p->name, sizeof(p->name));

  pid = np->pid;

  release(&np->lock);

  acquire(&wait_lock);
  np->parent = p;
  release(&wait_lock);

  acquire(&np->lock);
  np->state = RUNNABLE;
  release(&np->lock);

  return pid;
}

// Pass p's abandoned children to init.
// Caller must hold wait_lock.
void
reparent(struct proc *p)
{
  struct proc *pp;

  for(pp = proc; pp < &proc[NPROC]; pp++){
    if(pp->parent == p){
      pp->parent = initproc;
      wakeup(initproc);
    }
  }
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait().
void
exit(int status)
{
  struct proc *p = myproc();

  if(p == initproc)
    panic("init exiting");

  // Close all open files.
  for(int fd = 0; fd < NOFILE; fd++){
    if(p->ofile[fd]){
      struct file *f = p->ofile[fd];
      fileclose(f);
      p->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(p->cwd);
  end_op();
  p->cwd = 0;

  acquire(&wait_lock);

  // Give any children to init.
  reparent(p);

  // Parent might be sleeping in wait().
  wakeup(p->parent);
  
  acquire(&p->lock);

  p->xstate = status;
  p->state = ZOMBIE;

  p->etime = ticks;

  release(&wait_lock);

  // Jump into the scheduler, never to return.
  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
wait(uint64 addr)
{
  struct proc *pp;
  int havekids, pid;
  struct proc *p = myproc();

  acquire(&wait_lock);

  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(pp = proc; pp < &proc[NPROC]; pp++){
      if(pp->parent == p){
        // make sure the child isn't still in exit() or swtch().
        acquire(&pp->lock);

        havekids = 1;
        if(pp->state == ZOMBIE){
          // Found one.
          pid = pp->pid;
          if(addr != 0 && copyout(p->pagetable, addr, (char *)&pp->xstate,
                                  sizeof(pp->xstate)) < 0) {
            release(&pp->lock);
            release(&wait_lock);
            return -1;
          }
          freeproc(pp);
          release(&pp->lock);
          release(&wait_lock);
          return pid;
        }
        release(&pp->lock);
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || killed(p)){
      release(&wait_lock);
      return -1;
    }
    
    // Wait for a child to exit.
    sleep(p, &wait_lock);  //DOC: wait-sleep
  }
}


// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
waitx(uint64 addr, uint* wtime, uint* rtime)
{
  struct proc *np;
  int havekids, pid;
  struct proc *p = myproc();

  acquire(&wait_lock);

  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(np = proc; np < &proc[NPROC]; np++){
      if(np->parent == p){
        // make sure the child isn't still in exit() or swtch().
        acquire(&np->lock);

        havekids = 1;
        if(np->state == ZOMBIE){
          // Found one.
          pid = np->pid;
          *rtime = np->rtime;
          *wtime = np->etime - np->ctime - np->rtime;
          if(addr != 0 && copyout(p->pagetable, addr, (char *)&np->xstate,
                                  sizeof(np->xstate)) < 0) {
            release(&np->lock);
            release(&wait_lock);
            return -1;
          }
          freeproc(np);
          release(&np->lock);
          release(&wait_lock);
          return pid;
        }
        release(&np->lock);
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || p->killed){
      release(&wait_lock);
      return -1;
    }

    // Wait for a child to exit.
    sleep(p, &wait_lock);  //DOC: wait-sleep
  }
}


void
update_time()
{
  struct proc* p;
  for (p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if (p->state == RUNNING) {
      p->rtime++;
    }
    release(&p->lock); 
  }
}


// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run.
//  - swtch to start running that process.
//  - eventually that process transfers control
//    via swtch back to the scheduler.
void
scheduler(void)
{
  struct proc *p;
  struct cpu *c = mycpu();
  
  c->proc = 0;
  for(;;){
    // Avoid deadlock by ensuring that devices can interrupt.
    intr_on();

    // round robin scheduling
    if(SCHED_POLICY==1)
    {
      for(p = proc; p < &proc[NPROC]; p++) {
        acquire(&p->lock);
        if(p->state == RUNNABLE) {
          

          // Switch to chosen process.  It is the process's job
          // to release its lock and then reacquire it
          // before jumping back to us.
          p->state = RUNNING;
          c->proc = p;
          swtch(&c->context, &p->context);

      
          // Process is done running for now.
          // It should have changed its p->state before coming back.
          c->proc = 0;
        }
        release(&p->lock);
      }
    }

    if(SCHED_POLICY==0)
    {
      // fcfs scheduling
      int minCreateTime=__INT_MAX__;
      struct proc* minP=0;  //process with minimum create time/ticks

      for(p=proc;p<&proc[NPROC];p++)
      {
        acquire(&p->lock);
        if((p->state==RUNNABLE) && (p->start_tick < minCreateTime))
        {
          minCreateTime=p->start_tick;
          minP=p;
        }
        release(&p->lock);
      }

      if(minP!=0) //checking if a process was chosen for scheduling, if no processes in proc table then no processs would be chosen
      {
        acquire(&minP->lock);
        // checking if the proc isnt running on another cpu,
        // while looping through the proc table another cpu may have acquired this proc
        if(minP->state==RUNNABLE) 
        {
          minP->state=RUNNING;
          c->proc=minP;
          swtch(&c->context, &minP->context);
          c->proc=0;
        }
        release(&minP->lock);
      }
    }

    if(SCHED_POLICY==2)
    {
      // PBS scheduling
      int max_priority=100; // higher the number, lower the priority
      struct proc* maxPriorP=0;
      for(p=proc;p<&proc[NPROC];p++)
      {
        acquire(&p->lock);
        if((p->state==RUNNABLE) && (p->dp<max_priority)) //checking for higher priority process
        {
          max_priority=p->dp;
          maxPriorP=p;
        }
        else if((p->state==RUNNABLE) && (p->dp==max_priority)) //if priority is equal
        {
          if(maxPriorP!=0 && maxPriorP!=p) //checking if maxpriorP is valid and is not equal to process itself
          {
            acquire(&maxPriorP->lock);

            // this is there since maxPriorP may change to p, and releasing maxPriorP may release p itself
            struct proc* old_MaxPriorP=maxPriorP; 
            if(p->sched_times < maxPriorP->sched_times) //taking on which was scheduled least times
            {
              maxPriorP=p;
            }
            else if(p->sched_times == maxPriorP->sched_times)
            {
              if(p->start_tick < maxPriorP->start_tick) //checking which one started first(schedules earlier one)
              {
                maxPriorP=p;
              }
            }
            release(&old_MaxPriorP->lock);
          }
          else
          {
            maxPriorP=p;
          }
        }
        release(&p->lock);
      }

      //process to run has been selected
      if(maxPriorP!=0)
      {
        acquire(&maxPriorP->lock);
        if(maxPriorP->state==RUNNABLE)
        {
          maxPriorP->recent_run_ticks=0; //resetting run time since last scheduling
          maxPriorP->sched_times++;
          maxPriorP->state=RUNNING;
          c->proc=maxPriorP;

          int brunTime=ticks;
          swtch(&c->context, &maxPriorP->context);

          maxPriorP->recent_run_ticks=(ticks-brunTime); //how much time it ran for
          maxPriorP->niceness=0; // setting niceness for now
          int k=(((maxPriorP->sp - maxPriorP->niceness +5)<100) ? (maxPriorP->sp - maxPriorP->niceness +5) : 100);
          maxPriorP->dp=((0 > k) ? 0 : k); // setting dynamic priority for now

          c->proc=0;
        }
        release(&maxPriorP->lock);
      }
    }

    if(SCHED_POLICY==3)
    {
      int total_tickets=0; //what about ticket numbers changing after total_tickets was calculated

      //finding total number of tickets , assuming every runnable process has at least 1 ticket
      for(p = proc; p < &proc[NPROC]; p++) 
      {
        acquire(&p->lock);
        if(p->state == RUNNABLE) 
        {
          total_tickets+=(p->tickets);  //lock tickets ???
        }
        release(&p->lock);
      }

      
      if(total_tickets!=0)  //checking if a runnable process exists
      {
        // choosing a random ticket
        int rand_ticket_num;
        acquire(&psrg_lock);

        // checking for validity of psrg parameters
        if (psrg_seed >= total_tickets)
        {
          psrg_seed = (psrg_seed) % total_tickets;
        }
        if (increment >= total_tickets)
        {
          increment = (increment) % total_tickets;
        }
        if (multiplier > total_tickets)
        {
          multiplier = (multiplier) % total_tickets;
        }
        else if (multiplier == total_tickets) // multiplier needs to be greater than zero
        {
          multiplier = 1;
        }
        psrg_seed = (multiplier * psrg_seed + increment) % total_tickets;
        rand_ticket_num = psrg_seed;
        release(&psrg_lock);



        int running_ticket_num = 0; // cumulative ticket sum

        // finding which proc ticket it is and running that proc
        for (p = proc; p < &proc[NPROC]; p++)
        {
          acquire(&p->lock);
          if (p->state == RUNNABLE)
          {
            running_ticket_num += (p->tickets);
            if (rand_ticket_num <= running_ticket_num)
            {
              p->state = RUNNING; // reduce ticket number here?
              c->proc = p;
              swtch(&c->context, &p->context);

              c->proc = 0;
              release(&p->lock);
              break;
            }
          }
          release(&p->lock);
        }
      }

    }

    if(SCHED_POLICY==4)
    {
      // adding runnable processes not inq to inq
      for (p = proc; p < &proc[NPROC]; p++)
      {
        acquire(&p->lock);
        if (p->state == RUNNABLE)
        {
          if(p->inq==0)
          {
            p->q_wait_time=0;
            p->recent_run_time=0;
            q_arr[p->q_num][q_sizes[p->q_num]]=p;
            q_sizes[p->q_num]++;
            p->inq=1;
          }
        }
        release(&p->lock);
      }


      for(int i=0;i<5;i++)
      {
        if(q_sizes[i]!=0)
        {
          //printf("helllloo %d\n",q_arr[i]->size);
          // struct q_node* run_node=q_pop(q_arr[i]); //lock??
          //printf("mmmmmmsmmmm\n");
          p=q_arr[i][0];
          shift_front(i);
          q_sizes[i]--;
          //printf("sdsdsdsdsd\n");
          acquire(&p->lock);
          //printf("kkslllskkksll\n");
          p->inq=0;
          p->state = RUNNING;
          c->proc = p;
          swtch(&c->context, &p->context);

        
          // Process is done running for now.
          // It should have changed its p->state before coming back.
          c->proc = 0;
          
          //printf("hello\n");
          release(&p->lock);
          //printf("lmklmk");
          break;
        }
      }
      //go through qs 0,1,2,3,4 sequentially looking for processes
      // run processes with highest priority
    }
  }
}



// Switch to scheduler.  Must hold only p->lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->noff, but that would
// break in the few places where a lock is held but
// there's no process.
void
sched(void)
{
  
  int intena;
  struct proc *p = myproc();

  if(!holding(&p->lock))
    panic("sched p->lock");
  if(mycpu()->noff != 1)
    panic("sched locks");
  if(p->state == RUNNING)
    panic("sched running");
  if(intr_get())
    panic("sched interruptible");

  intena = mycpu()->intena;
  swtch(&p->context, &mycpu()->context);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void
yield(void)
{
  struct proc *p = myproc();
  acquire(&p->lock);
  p->state = RUNNABLE;
  sched();
  release(&p->lock);
}


// A fork child's very first scheduling by scheduler()
// will swtch to forkret.
void
forkret(void)
{
  static int first = 1;

  // Still holding p->lock from scheduler.
  release(&myproc()->lock);

  if (first) {
    // File system initialization must be run in the context of a
    // regular process (e.g., because it calls sleep), and thus cannot
    // be run from main().
    first = 0;
    fsinit(ROOTDEV);
  }

  usertrapret();
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
void
sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();
  
  // Must acquire p->lock in order to
  // change p->state and then call sched.
  // Once we hold p->lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup locks p->lock),
  // so it's okay to release lk.

  acquire(&p->lock);  //DOC: sleeplock1
  release(lk);

  // Go to sleep.
  p->chan = chan;
  p->state = SLEEPING;

  int bSleepTime=ticks;

  sched();

  int sleepT=ticks-bSleepTime; //time it was sleeping for
  if((p->recent_run_ticks + sleepT)!=0)
    p->niceness=(int)(((sleepT)/(p->recent_run_ticks + sleepT))*10); //updating niceness

  int k=(((p->sp - p->niceness +5)<100) ? (p->sp - p->niceness +5) : 100);
  p->dp=((0 > k) ? 0 : k); //updating dp
  

  // Tidy up.
  p->chan = 0;

  // Reacquire original lock.
  release(&p->lock);
  acquire(lk);
}

// Wake up all processes sleeping on chan.
// Must be called without any p->lock.
void
wakeup(void *chan)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++) {
    if(p != myproc()){
      acquire(&p->lock);
      if(p->state == SLEEPING && p->chan == chan) {
        p->state = RUNNABLE;
      }
      release(&p->lock);
    }
  }
}

// Kill the process with the given pid.
// The victim won't exit until it tries to return
// to user space (see usertrap() in trap.c).
int
kill(int pid)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++){
    acquire(&p->lock);
    if(p->pid == pid){
      p->killed = 1;
      if(p->state == SLEEPING){
        // Wake process from sleep().
        p->state = RUNNABLE;
      }
      release(&p->lock);
      return 0;
    }
    release(&p->lock);
  }
  return -1;
}

void
setkilled(struct proc *p)
{
  acquire(&p->lock);
  p->killed = 1;
  release(&p->lock);
}

int
killed(struct proc *p)
{
  int k;
  
  acquire(&p->lock);
  k = p->killed;
  release(&p->lock);
  return k;
}

// Copy to either a user address, or kernel address,
// depending on usr_dst.
// Returns 0 on success, -1 on error.
int
either_copyout(int user_dst, uint64 dst, void *src, uint64 len)
{
  struct proc *p = myproc();
  if(user_dst){
    return copyout(p->pagetable, dst, src, len);
  } else {
    memmove((char *)dst, src, len);
    return 0;
  }
}

// Copy from either a user address, or kernel address,
// depending on usr_src.
// Returns 0 on success, -1 on error.
int
either_copyin(void *dst, int user_src, uint64 src, uint64 len)
{
  struct proc *p = myproc();
  if(user_src){
    return copyin(p->pagetable, dst, src, len);
  } else {
    memmove(dst, (char*)src, len);
    return 0;
  }
}

// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(void)
{
  static char *states[] = {
  [UNUSED]    "unused",
  [USED]      "used",
  [SLEEPING]  "sleep ",
  [RUNNABLE]  "runble",
  [RUNNING]   "run   ",
  [ZOMBIE]    "zombie"
  };
  struct proc *p;
  char *state;

  printf("\n");
  for(p = proc; p < &proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    printf("%d %s %s", p->pid, state, p->name);
    printf("\n");
  }
}
