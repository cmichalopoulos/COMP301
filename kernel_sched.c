
#include <assert.h>
#include <sys/mman.h>

#include "kernel_cc.h"
#include "kernel_proc.h"
#include "kernel_sched.h"
#include "tinyos.h"

#ifndef NVALGRIND
#include <valgrind/valgrind.h>
#endif


/********************************************
	
	Core table and CCB-related declarations.

 *********************************************/

/* Core control blocks */
CCB cctx[MAX_CORES];

#define PRIORITY_QUEUES 10
#define YIELDS 50

/* 
	The current core's CCB. This must only be used in a 
	non-preemtpive context.
 */
#define CURCORE (cctx[cpu_core_id])

/* 
	The current thread. This is a pointer to the TCB of the thread 
	currently executing on this core.

	This must only be used in non-preemptive context.
*/
#define CURTHREAD (CURCORE.current_thread)


/*
	This can be used in the preemptive context to
	obtain the current thread.
 */
TCB* cur_thread()
{
  int preempt = preempt_off;
  TCB* cur = CURTHREAD;
  if(preempt) preempt_on;
  return cur;
}



/*
   The thread layout.
  --------------------

  On the x86 (Pentium) architecture, the stack grows upward. Therefore, we
  can allocate the TCB at the top of the memory block used as the stack.

  +-------------+
  |   TCB       |
  +-------------+
  |             |
  |    stack    |
  |             |
  |      ^      |
  |      |      |
  +-------------+
  | first frame |
  +-------------+

  Advantages: (a) unified memory area for stack and TCB (b) stack overrun will
  crash own thread, before it affects other threads (which may make debugging
  easier).

  Disadvantages: The stack cannot grow unless we move the whole TCB. Of course,
  we do not support stack growth anyway!
 */

/*
  A counter for active threads. By "active", we mean 'existing',
  with the exception of idle threads (they don't count).
 */
volatile unsigned int active_threads = 0;
Mutex active_threads_spinlock = MUTEX_INIT;

/* This is specific to Intel Pentium! */
#define SYSTEM_PAGE_SIZE (1 << 12)

/* The memory allocated for the TCB must be a multiple of SYSTEM_PAGE_SIZE */
#define THREAD_TCB_SIZE \
	(((sizeof(TCB) + SYSTEM_PAGE_SIZE - 1) / SYSTEM_PAGE_SIZE) * SYSTEM_PAGE_SIZE)

#define THREAD_SIZE (THREAD_TCB_SIZE + THREAD_STACK_SIZE)

//#define MMAPPED_THREAD_MEM
#ifdef MMAPPED_THREAD_MEM

/*
  Use mmap to allocate a thread. A more detailed implementation can allocate a
  "sentinel page", and change access to PROT_NONE, so that a stack overflow
  is detected as seg.fault.
 */
void free_thread(void* ptr, size_t size) { CHECK(munmap(ptr, size)); }

void* allocate_thread(size_t size)
{
	void* ptr = mmap(NULL, size, PROT_READ | PROT_WRITE | PROT_EXEC,
		MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);

	CHECK((ptr == MAP_FAILED) ? -1 : 0);

	return ptr;
}
#else
/*
  Use malloc to allocate a thread. This is probably faster than  mmap, but
  cannot be made easily to 'detect' stack overflow.
 */
void free_thread(void* ptr, size_t size) { free(ptr); }

void* allocate_thread(size_t size)
{
	void* ptr = aligned_alloc(SYSTEM_PAGE_SIZE, size);
	CHECK((ptr == NULL) ? -1 : 0);
	return ptr;
}
#endif




/*
  This is the function that is used to start normal threads.
*/

void gain(int preempt); /* forward */

static void thread_start()
{
	gain(1);
	cur_thread()->thread_func();

	/* We are not supposed to get here! */
	assert(0);
}

/*
  Initialize and return a new TCB
*/

TCB* spawn_thread(PCB* pcb, void (*func)())
{
	/* The allocated thread size must be a multiple of page size */
	TCB* tcb = (TCB*)allocate_thread(THREAD_SIZE);

	/* Set the owner */
	tcb->owner_pcb = pcb;

	/* Initialize the other attributes */
	tcb->type = NORMAL_THREAD;
	tcb->state = INIT;
	tcb->phase = CTX_CLEAN;
	tcb->thread_func = func;
	tcb->wakeup_time = NO_TIMEOUT;
	rlnode_init(&tcb->sched_node, tcb); /* Intrusive list node */

	tcb->its = QUANTUM;
	tcb->rts = QUANTUM;
	tcb->last_cause = SCHED_IDLE;
	tcb->curr_cause = SCHED_IDLE;

	/* Compute the stack segment address and size */
	void* sp = ((void*)tcb) + THREAD_TCB_SIZE;

	/* Init the context */
	cpu_initialize_context(&tcb->context, sp, THREAD_STACK_SIZE, thread_start);

#ifndef NVALGRIND
	tcb->valgrind_stack_id = VALGRIND_STACK_REGISTER(sp, sp + THREAD_STACK_SIZE);
#endif

	/* increase the count of active threads */
	Mutex_Lock(&active_threads_spinlock);
	active_threads++;
	Mutex_Unlock(&active_threads_spinlock);

	return tcb;
}

/*
  This is called with sched_spinlock locked !
 */
void release_TCB(TCB* tcb)
{
#ifndef NVALGRIND
	VALGRIND_STACK_DEREGISTER(tcb->valgrind_stack_id);
#endif

	free_thread(tcb, THREAD_SIZE);

	Mutex_Lock(&active_threads_spinlock);
	active_threads--;
	Mutex_Unlock(&active_threads_spinlock);
}

/*
 *
 * Scheduler
 *
 */

/*
 *  Note: the scheduler routines are all in the non-preemptive domain.
 */

/*
  The scheduler queue is implemented as a doubly linked list. The
  head and tail of this list are stored in  SCHED.

  Also, the scheduler contains a linked list of all the sleeping
  threads with a timeout.

  Both of these structures are protected by @c sched_spinlock.
*/

int numof_yield_calls = 0; // num of yield_calls

rlnode SCHED[PRIORITY_QUEUES]; /* The scheduler queue */
rlnode TIMEOUT_LIST; /* The list of threads with a timeout */
Mutex sched_spinlock = MUTEX_INIT; /* spinlock for scheduler queue */

/* Interrupt handler for ALARM */
void yield_handler() { yield(SCHED_QUANTUM); }

/* Interrupt handle for inter-core interrupts */
void ici_handler()
{ /* noop for now... */
}

/*
  Possibly add TCB to the scheduler timeout list.

  *** MUST BE CALLED WITH sched_spinlock HELD ***
*/
static void sched_register_timeout(TCB* tcb, TimerDuration timeout)
{
	if (timeout != NO_TIMEOUT) {
		/* set the wakeup time */
		TimerDuration curtime = bios_clock();
		tcb->wakeup_time = (timeout == NO_TIMEOUT) ? NO_TIMEOUT : curtime + timeout;

		/* add to the TIMEOUT_LIST in sorted order */
		rlnode* n = TIMEOUT_LIST.next;
		for (; n != &TIMEOUT_LIST; n = n->next)
			/* skip earlier entries */
			if (tcb->wakeup_time < n->tcb->wakeup_time)
				break;
		/* insert before n */
		rl_splice(n->prev, &tcb->sched_node);
	}
}

/*
  Add TCB to the end of the scheduler list.

  *** MUST BE CALLED WITH sched_spinlock HELD ***
*/
static void sched_queue_add(TCB* tcb)
{
	/* Insert at the end of the scheduling list */
	rlist_push_back(&SCHED[tcb->priority], &tcb->sched_node);

	/* Restart possibly halted cores */
	cpu_core_restart_one();
}

/*
	Adjust the state of a thread to make it READY.

	*** MUST BE CALLED WITH sched_spinlock HELD ***
 */
static void sched_make_ready(TCB* tcb)
{
	assert(tcb->state == STOPPED || tcb->state == INIT);

	/* Possibly remove from TIMEOUT_LIST */
	if (tcb->wakeup_time != NO_TIMEOUT) {
		/* tcb is in TIMEOUT_LIST, fix it */
		assert(tcb->sched_node.next != &(tcb->sched_node) && tcb->state == STOPPED);
		rlist_remove(&tcb->sched_node);
		tcb->wakeup_time = NO_TIMEOUT;
	}

	/* Mark as ready */
	tcb->state = READY;

	/* Possibly add to the scheduler queue */
	if (tcb->phase == CTX_CLEAN)
		sched_queue_add(tcb);
}

/*
  Scan the \c TIMEOUT_LIST for threads whose timeout has expired, and
  wake them up.

  *** MUST BE CALLED WITH sched_spinlock HELD ***
*/
static void sched_wakeup_expired_timeouts()
{
	/* Empty the timeout list up to the current time and wake up each thread */
	TimerDuration curtime = bios_clock();

	while (!is_rlist_empty(&TIMEOUT_LIST)) {
		TCB* tcb = TIMEOUT_LIST.next->tcb;
		if (tcb->wakeup_time > curtime)
			break;
		sched_make_ready(tcb);
	}
}
/*
  Remove the head of the scheduler list, if any, and
  return it. Return NULL if the list is empty.

  *** MUST BE CALLED WITH sched_spinlock HELD ***
*/


// Weird to implement. Starting from top queue and goind down, trying to fing a thread, to use as a first. 
// Integer helper is used to break the loop once the thread is found. 
// If not found, decrease priority and start again.....
static TCB* sched_queue_select(TCB* current)
{
	
	TCB* next_thread = NULL;
	int i = PRIORITY_QUEUES;
	int helper = 0;
	while(i>0 && helper == 0){
		rlnode* sel = rlist_pop_front(&SCHED[i]);
		if(sel->tcb != NULL){
			next_thread = sel->tcb; /* When the list is empty, this is NULL */
			helper = 1;	// to escape loop 
		}
		else i--;
	}

	if (next_thread == NULL)
		next_thread = (current->state == READY) ? current : &CURCORE.idle_thread;

	next_thread->its = QUANTUM;

	return next_thread;
}

/*
  Make the process ready.
 */
int wakeup(TCB* tcb)
{
	int ret = 0;

	/* Preemption off */
	int oldpre = preempt_off;

	/* To touch tcb->state, we must get the spinlock. */
	Mutex_Lock(&sched_spinlock);

	if (tcb->state == STOPPED || tcb->state == INIT) {
		sched_make_ready(tcb);
		ret = 1;
	}

	Mutex_Unlock(&sched_spinlock);

	/* Restore preemption state */
	if (oldpre)
		preempt_on;

	return ret;
}

/*
  Atomically put the current process to sleep, after unlocking mx.
 */
void sleep_releasing(Thread_state state, Mutex* mx, enum SCHED_CAUSE cause,
	TimerDuration timeout)
{
	assert(state == STOPPED || state == EXITED);


	int preempt = preempt_off;
	TCB* tcb = CURTHREAD;
	Mutex_Lock(&sched_spinlock);

	/* mark the thread as stopped or exited */
	tcb->state = state;

	/* register the timeout (if any) for the sleeping thread */
	if (state != EXITED)
		sched_register_timeout(tcb, timeout);

	/* Release mx */
	if (mx != NULL)
		Mutex_Unlock(mx);

	/* Release the schduler spinlock before calling yield() !!! */
	Mutex_Unlock(&sched_spinlock);

	/* call this to schedule someone else */
	yield(cause);

	/* Restore preemption state */
	if (preempt)
		preempt_on;
}

// Function boost threads, boosts threads that are down in priority, starting at priority = 0, after 10 yields. This happens by finding 
// threads and adding 1 to their priority.
void boost_threads(){
	for(int i=0; i<PRIORITY_QUEUES; i++){
		for(int j = 0; j<rlist_len(&SCHED[i]); j++){
			rlnode* thr = rlist_pop_front(&SCHED[i]);
			if(thr->tcb != NULL){
				thr->tcb->priority += 1;
				rlist_push_back(&SCHED[i+1], thr);
			}
		}
	}
}

/* This function is the entry point to the scheduler's context switching */


void yield(enum SCHED_CAUSE cause)
{

	numof_yield_calls = numof_yield_calls + 1;

	/* Reset the timer, so that we are not interrupted by ALARM */
	TimerDuration remaining = bios_cancel_timer();

	/* We must stop preemption but save it! */
	int preempt = preempt_off;



	TCB* current = CURTHREAD; /* Make a local copy of current process, for speed */

	Mutex_Lock(&sched_spinlock);

	/* Update CURTHREAD state */
	if (current->state == RUNNING)
		current->state = READY;

	/* Update CURTHREAD scheduler data */
	current->rts = remaining;
	current->last_cause = current->curr_cause;
	current->curr_cause = cause;

	/* Wake up threads whose sleep timeout has expired */
	sched_wakeup_expired_timeouts();

	// Time to do some boosting...
	if(numof_yield_calls > YIELDS){
		boost_threads();
		numof_yield_calls =0;
	}


	/* Get next */
	TCB* next = sched_queue_select(current);
	assert(next != NULL);

	/* Save the current TCB for the gain phase */
	CURCORE.previous_thread = current;

	Mutex_Unlock(&sched_spinlock);

	/* Switch contexts */
	if (current != next) {
		CURTHREAD = next;
		cpu_swap_context(&current->context, &next->context);
	}

	switch(cause){
	
	// When the cause is SCHED_QUANTUM, it means that the thread hasn't completed its task in the given quantum and must give up place and reduce priority by 1
	case (SCHED_QUANTUM):
		if(current->priority > 0)
			current->priority = current->priority - 1;
		else current->priority = 0;
	break;

	// When the cause is SCHED_IO, it means that we have a thread waiting for I/O, which means that it will take a little time, and so give it a higher priority
	case (SCHED_IO): 
		if(current->priority < PRIORITY_QUEUES)
			current->priority = current->priority + 1;
		//else current->priority = PRIORITY_QUEUES;
	break;

	// When i have SCHED_MUTEX, it means that my high priority thread wants a mutex that is currently used by a low priority thread, meaning that i have to decrease
	// the high-priority thread so that i give the low-priority thread a chance to finish and release the wanted mutex
	case(SCHED_MUTEX):
		//if(current->priority>0) //&& current->priority>0)
		if(current->curr_cause == SCHED_MUTEX && current->last_cause == SCHED_MUTEX)
			current->priority = current->priority - 1;
	break;

	default:
	for(current->priority = 0; current->priority<PRIORITY_QUEUES; current->priority++)
		current->priority = current->priority;
	break;
	}

	/* This is where we get after we are switched back on! A long time
	   may have passed. Start a new timeslice...
	  */
	gain(preempt);
}



/*
  This function must be called at the beginning of each new timeslice.
  This is done mostly from inside yield().
  However, for threads that are executed for the first time, this
  has to happen in thread_start.

  The 'preempt' argument determines whether preemption is turned on
  in the new timeslice. When returning to threads in the non-preemptive
  domain (e.g., waiting at some driver), we need to not turn preemption
  on!
*/

void gain(int preempt)
{
	Mutex_Lock(&sched_spinlock);

	TCB* current = CURTHREAD;

	/* Mark current state */
	current->state = RUNNING;
	current->phase = CTX_DIRTY;
	current->rts = current->its;

	/* Take care of the previous thread */
	TCB* prev = CURCORE.previous_thread;
	if (current != prev) {
		prev->phase = CTX_CLEAN;
		switch (prev->state) {
		case READY:
			if (prev->type != IDLE_THREAD)
				sched_queue_add(prev);
			break;
		case EXITED:
			release_TCB(prev);
			break;
		case STOPPED:
			break;
		default:
			assert(0); /* prev->state should not be INIT or RUNNING ! */
		}
	}

	Mutex_Unlock(&sched_spinlock);

	/* Reset preemption as needed */
	if (preempt)
		preempt_on;

	/* Set a 1-quantum alarm */
	bios_set_timer(current->rts);
}

static void idle_thread()
{
	/* When we first start the idle thread */
	yield(SCHED_IDLE);

	/* We come here whenever we cannot find a ready thread for our core */
	while (active_threads > 0) {
		cpu_core_halt();
		yield(SCHED_IDLE);
	}

	/* If the idle thread exits here, we are leaving the scheduler! */
	bios_cancel_timer();
	cpu_core_restart_all();
}

/*
  Initialize the scheduler queue
 */
void initialize_scheduler()
{
	for(int i=0; i<PRIORITY_QUEUES; i++){
		rlnode_init(&SCHED[i], NULL);
	}

	rlnode_init(&TIMEOUT_LIST, NULL);
}

void run_scheduler()
{
	CCB* curcore = &CURCORE;

	/* Initialize current CCB */
	curcore->id = cpu_core_id;

	curcore->current_thread = &curcore->idle_thread;

	curcore->idle_thread.owner_pcb = get_pcb(0);
	curcore->idle_thread.type = IDLE_THREAD;
	curcore->idle_thread.state = RUNNING;
	curcore->idle_thread.phase = CTX_DIRTY;
	curcore->idle_thread.wakeup_time = NO_TIMEOUT;
	rlnode_init(&curcore->idle_thread.sched_node, &curcore->idle_thread);

	curcore->idle_thread.its = QUANTUM;
	curcore->idle_thread.rts = QUANTUM;

	curcore->idle_thread.curr_cause = SCHED_IDLE;
	curcore->idle_thread.last_cause = SCHED_IDLE;

	/* Initialize interrupt handler */
	cpu_interrupt_handler(ALARM, yield_handler);
	cpu_interrupt_handler(ICI, ici_handler);

	/* Run idle thread */
	preempt_on;
	idle_thread();

	/* Finished scheduling */
	assert(CURTHREAD == &CURCORE.idle_thread);
	cpu_interrupt_handler(ALARM, NULL);
	cpu_interrupt_handler(ICI, NULL);
}
