
#include "kernel_proc.h"
#include <assert.h>
#include "kernel_cc.h"
#include "kernel_streams.h"
#include "kernel_sysinfo.h"
#include "tinyos.h"


/* 
 The process table and related system calls:
 - Exec
 - Exit
 - WaitPid
 - GetPid
 - GetPPid

 */

/* The process table */
PCB PT[MAX_PROC];
unsigned int process_count;

PCB* get_pcb(Pid_t pid)
{
  return PT[pid].pstate==FREE ? NULL : &PT[pid];
}

Pid_t get_pid(PCB* pcb)
{
  return pcb==NULL ? NOPROC : pcb-PT;
}

/* Initialize a PCB */
static inline void initialize_PCB(PCB* pcb)
{
  pcb->pstate = FREE;
  pcb->argl = 0;
  pcb->args = NULL;

  // New thing thread_count needs to be initialized, probably here
  pcb->thread_count = 0;
  // Also initiallizing our list
  rlnode_init(&pcb->ptcb_list, pcb);

  for(int i=0;i<MAX_FILEID;i++)
    pcb->FIDT[i] = NULL;

  rlnode_init(& pcb->children_list, NULL);
  rlnode_init(& pcb->exited_list, NULL);
  rlnode_init(& pcb->children_node, pcb);
  rlnode_init(& pcb->exited_node, pcb);
  pcb->child_exit = COND_INIT;
}


static PCB* pcb_freelist;

void initialize_processes()
{
  /* initialize the PCBs */
  for(Pid_t p=0; p<MAX_PROC; p++) {
    initialize_PCB(&PT[p]);
  }

  /* use the parent field to build a free list */
  PCB* pcbiter;
  pcb_freelist = NULL;
  for(pcbiter = PT+MAX_PROC; pcbiter!=PT; ) {
    --pcbiter;
    pcbiter->parent = pcb_freelist;
    pcb_freelist = pcbiter;
  }

  process_count = 0;

  /* Execute a null "idle" process */
  if(Exec(NULL,0,NULL)!=0)
    FATAL("The scheduler process does not have pid==0");
}


/*
  Must be called with kernel_mutex held
*/
PCB* acquire_PCB()
{
  PCB* pcb = NULL;

  if(pcb_freelist != NULL) {
    pcb = pcb_freelist;
    pcb->pstate = ALIVE;
    pcb_freelist = pcb_freelist->parent;
    process_count++;
  }

  return pcb;
}

/*
  Must be called with kernel_mutex held
*/
void release_PCB(PCB* pcb)
{
  pcb->pstate = FREE;
  pcb->parent = pcb_freelist;
  pcb_freelist = pcb;
  process_count--;
}


/*
 *
 * Process creation
 *
 */

/*
	This function is provided as an argument to spawn,
	to execute the main thread of a process.
*/
void start_main_thread()
{
  int exitval;

  Task call =  CURPROC->main_task;
  int argl = CURPROC->argl;
  void* args = CURPROC->args;

  exitval = call(argl,args);
  Exit(exitval);
}


/*
	System call to create a new process.
 */
Pid_t sys_Exec(Task call, int argl, void* args)
{
  PCB *curproc, *newproc;
  
  /* The new process PCB */
  newproc = acquire_PCB();

  if(newproc == NULL) goto finish;  /* We have run out of PIDs! */

  if(get_pid(newproc)<=1) {
    /* Processes with pid<=1 (the scheduler and the init process) 
       are parentless and are treated specially. */
    newproc->parent = NULL;
  }
  else
  {
    /* Inherit parent */
    curproc = CURPROC;

    /* Add new process to the parent's child list */
    newproc->parent = curproc;
    rlist_push_front(& curproc->children_list, & newproc->children_node);

    /* Inherit file streams from parent */
    for(int i=0; i<MAX_FILEID; i++) {
       newproc->FIDT[i] = curproc->FIDT[i];
       if(newproc->FIDT[i])
          FCB_incref(newproc->FIDT[i]);
    }
  }


  /* Set the main thread's function */
  newproc->main_task = call;

  /* Copy the arguments to new storage, owned by the new process */
  newproc->argl = argl;
  if(args!=NULL) {
    newproc->args = malloc(argl);
    memcpy(newproc->args, args, argl);
  }
  else
    newproc->args=NULL;

  /* 
    Create and wake up the thread for the main function. This must be the last thing
    we do, because once we wakeup the new thread it may run! so we need to have finished
    the initialization of the PCB.
   */

  // Creating the main thread
  if(call != NULL) {

    // Create a thread...
    newproc->main_thread = spawn_thread(newproc, start_main_thread);
    rlnode_init(&newproc->ptcb_list, newproc);     // For original PCB list 
   
    // Create a PTCB
    // Must create ptcb and initialize everything except rlnode stuff
    PTCB* new_ptcb = (PTCB* )xmalloc(sizeof(PTCB));  // Mem. Allocating for new PTCB
    new_ptcb->tcb = newproc->main_thread;
    new_ptcb->task = call; 
    new_ptcb->argl = argl;
    new_ptcb->args = args;

    new_ptcb->exited = 0;
    new_ptcb->detached = 0;
    new_ptcb->exit_cv = COND_INIT;
    new_ptcb->refcount = 1;

    // Initializing list aka rlnode stuff
    rlnode_init(&(new_ptcb->ptcb_list_node), new_ptcb);      // For new PTCB list 
    rlist_push_back(&(newproc->ptcb_list), &(new_ptcb->ptcb_list_node));  // PCB shows PTCB
    
    
    //Connections...
    new_ptcb->tcb = newproc->main_thread;
    newproc->main_thread->ptcb = new_ptcb;
    newproc->thread_count += 1;
   
    wakeup(new_ptcb->tcb);
  }


finish:
  return get_pid(newproc);
}


/* System call */
Pid_t sys_GetPid()
{
  return get_pid(CURPROC);
}


Pid_t sys_GetPPid()
{
  return get_pid(CURPROC->parent);
}


static void cleanup_zombie(PCB* pcb, int* status)
{
  if(status != NULL)
    *status = pcb->exitval;

  rlist_remove(& pcb->children_node);
  rlist_remove(& pcb->exited_node);

  release_PCB(pcb);
}


static Pid_t wait_for_specific_child(Pid_t cpid, int* status)
{

  /* Legality checks */
  if((cpid<0) || (cpid>=MAX_PROC)) {
    cpid = NOPROC;
    goto finish;
  }

  PCB* parent = CURPROC;
  PCB* child = get_pcb(cpid);
  if( child == NULL || child->parent != parent)
  {
    cpid = NOPROC;
    goto finish;
  }

  /* Ok, child is a legal child of mine. Wait for it to exit. */
  while(child->pstate == ALIVE)
    kernel_wait(& parent->child_exit, SCHED_USER);
  
  cleanup_zombie(child, status);
  
finish:
  return cpid;
}


static Pid_t wait_for_any_child(int* status)
{
  Pid_t cpid;

  PCB* parent = CURPROC;

  /* Make sure I have children! */
  int no_children, has_exited;
  while(1) {
    no_children = is_rlist_empty(& parent->children_list);
    if( no_children ) break;

    has_exited = ! is_rlist_empty(& parent->exited_list);
    if( has_exited ) break;

    kernel_wait(& parent->child_exit, SCHED_USER);    
  }

  if(no_children)
    return NOPROC;

  PCB* child = parent->exited_list.next->pcb;
  assert(child->pstate == ZOMBIE);
  cpid = get_pid(child);
  cleanup_zombie(child, status);

  return cpid;
}


Pid_t sys_WaitChild(Pid_t cpid, int* status)
{
  /* Wait for specific child. */
  if(cpid != NOPROC) {
    return wait_for_specific_child(cpid, status);
  }
  /* Wait for any child */
  else {
    return wait_for_any_child(status);
  }

}


void sys_Exit(int exitval)
{

  PCB *curproc = CURPROC;  /* cache for efficiency */

  /* First, store the exit status */
  curproc->exitval = exitval;
  /* 
    Here, we must check that we are not the init task. 
    If we are, we must wait until all child processes exit. 
   */
  if(get_pid(curproc)==1) {

    while(sys_WaitChild(NOPROC,NULL)!=NOPROC);

  } else {

    /* Reparent any children of the exiting process to the 
       initial task */
    PCB* initpcb = get_pcb(1);
    while(!is_rlist_empty(& curproc->children_list)) {
      rlnode* child = rlist_pop_front(& curproc->children_list);
      child->pcb->parent = initpcb;
      rlist_push_front(& initpcb->children_list, child);
    }

    /* Add exited children to the initial task's exited list 
       and signal the initial task */
    if(!is_rlist_empty(& curproc->exited_list)) {
      rlist_append(& initpcb->exited_list, &curproc->exited_list);
      kernel_broadcast(& initpcb->child_exit);
    }

    /* Put me into my parent's exited list */
    rlist_push_front(& curproc->parent->exited_list, &curproc->exited_node);
    kernel_broadcast(& curproc->parent->child_exit);
  
  }

  assert(is_rlist_empty(& curproc->children_list));
  assert(is_rlist_empty(& curproc->exited_list));

  /* 
    Do all the other cleanup we want here, close files etc. 
   */

  /* Release the args data */
  if(curproc->args) {
    free(curproc->args);
    curproc->args = NULL;
  }

  /* Clean up FIDT */
  for(int i=0;i<MAX_FILEID;i++) {
    if(curproc->FIDT[i] != NULL) {
      FCB_decref(curproc->FIDT[i]);
      curproc->FIDT[i] = NULL;
    }
  }

  /* Disconnect my main_thread */
  curproc->main_thread = NULL;

  /* Now, mark the process as exited. */
  curproc->pstate = ZOMBIE;

  /* Bye-bye cruel world */
  kernel_sleep(EXITED, SCHED_USER);

  //sys_ThreadExit(exitval);

}

// System Info Stuff

int procinfo_read(void* procinfo_cb, char* buf, unsigned int size){
  procinfo* procinfocb = (procinfo*) procinfo_cb;
  procinf_cb* picb = (procinf_cb*)xmalloc(sizeof(procinf_cb));

  for(Pid_t i=0; i<MAX_PROC; i++){
    if(PT[i].pstate != FREE){
      // Get pid and ppid 
      procinfocb->pid = get_pid(&PT[i]);
      if(PT[i].parent == 0)                       // If process is parent, then ppid = 0
        procinfocb->ppid = 0;               
      else 
        procinfocb->ppid = get_pid(PT[i].parent); // else get the ppid

      // Check if process is alive 
      if(PT[i].pstate == ALIVE)
        procinfocb->alive = 1;
      else
        procinfocb->alive = PT[i].pstate;

      procinfocb->thread_count = PT[i].thread_count;
      procinfocb->main_task = PT[i].main_task;
      procinfocb->argl = PT[i].argl;
      //memcpy(procinfocb->args, PT[i].args, PT[i].argl);

      // Copy info from procinfo_cb, to the buffer that stores and shows info
      memcpy(buf, picb->curinfo, size);
      picb->pcb_cursor ++;
    }
  }
  return size;
}

int procinfo_close(void* procinfo_cb){
  procinf_cb* picb = (procinf_cb*) procinfo_cb;
  free(picb);
  return 0;
}

int procinfo_write(void* procinfo_cb, const char* buf, unsigned int size){
  return -1;
}

Fid_t sys_OpenInfo()
{
  Fid_t fid[1];
  FCB* fcb[1];
  int reserved = FCB_reserve(1, fid, fcb);                        // Reserve FCB from FT
  if(reserved == 0)
    return NOFILE;

  procinf_cb* picb = (procinf_cb*)xmalloc(sizeof(procinf_cb));
  if(picb == NULL)
    return NOFILE;

  picb->pcb_cursor = 1;                                           // Initialize everything from proc_info_control_block
  fcb[0]->streamobj = picb;                                       // created on kernel_sysinfo.h
  fcb[0]->streamfunc = &proc_info;

  return fid[0];
}
