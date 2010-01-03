/* See COPYRIGHT for copyright information. */

#ifdef __SHARC__
#pragma nosharc
#endif

#include <ros/common.h>
#include <arch/types.h>
#include <arch/arch.h>
#include <arch/mmu.h>
#include <arch/console.h>
#include <ros/timer.h>
#include <ros/error.h>

#include <string.h>
#include <assert.h>
#include <process.h>
#include <schedule.h>
#include <pmap.h>
#include <mm.h>
#include <trap.h>
#include <syscall.h>
#include <kmalloc.h>
#include <stdio.h>
#include <resource.h>
#include <colored_caches.h>
#include <arch/bitmask.h>
#include <kfs.h> // eventually replace this with vfs.h

#ifdef __sparc_v8__
#include <arch/frontend.h>
#endif 

#ifdef __NETWORK__
#include <arch/nic_common.h>
extern char *CT(PACKET_HEADER_SIZE + len) (*packet_wrap)(const char *CT(len) data, size_t len);
extern int (*send_frame)(const char *CT(len) data, size_t len);
#endif

//Do absolutely nothing.  Used for profiling.
static void sys_null(void)
{
	return;
}

//Write a buffer over the serial port
static ssize_t sys_serial_write(env_t* e, const char *DANGEROUS buf, size_t len)
{
	if (len == 0)
		return 0;
	#ifdef SERIAL_IO
		char *COUNT(len) _buf = user_mem_assert(e, buf, len, PTE_USER_RO);
		for(int i =0; i<len; i++)
			serial_send_byte(buf[i]);
		return (ssize_t)len;
	#else
		return -EINVAL;
	#endif
}

//Read a buffer over the serial port
static ssize_t sys_serial_read(env_t* e, char *DANGEROUS _buf, size_t len)
{
	if (len == 0)
		return 0;

	#ifdef SERIAL_IO
	    char *COUNT(len) buf = user_mem_assert(e, _buf, len, PTE_USER_RO);
		size_t bytes_read = 0;
		int c;
		while((c = serial_read_byte()) != -1) {
			buf[bytes_read++] = (uint8_t)c;
			if(bytes_read == len) break;
		}
		return (ssize_t)bytes_read;
	#else
		return -EINVAL;
	#endif
}

//
/* START OF REMOTE SYSTEMCALL SUPPORT SYSCALLS. THESE WILL GO AWAY AS THINGS MATURE */
//

static ssize_t sys_fork(env_t* e)
{
	// TODO: right now we only support fork for single-core processes
	if(e->state != PROC_RUNNING_S)
		return -1;

	env_t* env = proc_create(NULL,0);
	assert(env != NULL);

	env->heap_bottom = e->heap_bottom;
	env->heap_top = e->heap_top;
	env->ppid = e->pid;
	env->env_tf = *current_tf;

	env->cache_colors_map = cache_colors_map_alloc();
	for(int i=0; i < llc_cache->num_colors; i++)
		if(GET_BITMASK_BIT(e->cache_colors_map,i))
			cache_color_alloc(llc_cache, env->cache_colors_map);

	// copy page table and page contents.
	// TODO: does not work with mmap.  only text, heap, stack are copied.
	for(char* va = 0; va < (char*)UTOP; va += PGSIZE)
	{
		// copy [0,heaptop] and [stackbot,utop]
		if(va == env->heap_top)
			va = (char*)USTACKBOT;

		int perms = get_va_perms(e->env_pgdir,va);
		if(perms) // I think this should always be true
		{
			page_t* pp;
			assert(upage_alloc(env,&pp) == 0);
			assert(page_insert(env->env_pgdir,pp,va,perms) == 0);
			assert(memcpy_from_user(e,page2kva(pp),va,PGSIZE) == 0);
		}
	}

	__proc_set_state(env, PROC_RUNNABLE_S);
	schedule_proc(env);

	// don't decref the new process.
	// that will happen when the parent waits for it.

	printd("[PID %d] fork PID %d\n",e->pid,env->pid);

	return env->pid;
}

static ssize_t sys_trywait(env_t* e, pid_t pid, int* status)
{
	struct proc* p = pid2proc(pid);

	// TODO: this syscall is racy, so we only support for single-core procs
	if(e->state != PROC_RUNNING_S)
		return -1;

	// TODO: need to use errno properly.  sadly, ROS error codes conflict..

	if(p)
	{
		ssize_t ret;

		if(current->pid == p->ppid)
		{
			if(p->state == PROC_DYING)
			{
				memcpy_to_user(e,status,&p->exitcode,sizeof(int));
				printd("[PID %d] waited for PID %d (code %d)\n",
				       e->pid,p->pid,p->exitcode);
				ret = 0;
			}
			else // not dead yet
			{
				set_errno(current_tf,0);
				ret = -1;
			}
		}
		else // not a child of the calling process
		{
			set_errno(current_tf,1);
			ret = -1;
		}

		// if the wait succeeded, decref twice
		proc_decref(p,1 + (ret == 0));
		return ret;
	}

	set_errno(current_tf,1);
	return -1;
}

static ssize_t sys_exec(env_t* e, void *DANGEROUS binary_buf, size_t len,
                        void*DANGEROUS arg, void*DANGEROUS env)
{
	// TODO: right now we only support exec for single-core processes
	if(e->state != PROC_RUNNING_S)
		return -1;

	if(memcpy_from_user(e,e->env_procinfo->argv_buf,arg,PROCINFO_MAX_ARGV_SIZE))
		return -1;
	if(memcpy_from_user(e,e->env_procinfo->env_buf,env,PROCINFO_MAX_ENV_SIZE))
		return -1;

	void* binary = kmalloc(len,0);
	if(binary == NULL)
		return -1;
	if(memcpy_from_user(e,binary,binary_buf,len))
	{
		kfree(binary);
		return -1;
	}

	// TODO: this is probably slow.  as with fork, should walk page table
	env_segment_free(e,0,USTACKTOP);

	env_load_icode(e,NULL,binary,len);
	proc_init_trapframe(current_tf,0);

	/*printk("[PID %d] exec ",e->pid);
	char argv[PROCINFO_MAX_ARGV_SIZE];
	int* offsets = (int*)e->env_procinfo->argv_buf;
	for(int i = 0; offsets[i]; i++)
		printk("%s%c",e->env_procinfo->argv_buf+offsets[i],offsets[i+1] ? ' ' : '\n');*/

	kfree(binary);
	return 0;
}

static ssize_t sys_run_binary(env_t* e, void *DANGEROUS binary_buf, size_t len,
                              void*DANGEROUS arg, size_t num_colors)
{
	env_t* env = proc_create(NULL,0);
	assert(env != NULL);

	static_assert(PROCINFO_NUM_PAGES == 1);
	assert(memcpy_from_user(e,env->env_procinfo->argv_buf,arg,PROCINFO_MAX_ARGV_SIZE) == ESUCCESS);
	*(intptr_t*)env->env_procinfo->env_buf = 0;

	env_load_icode(env,e,binary_buf,len);
	__proc_set_state(env, PROC_RUNNABLE_S);
	schedule_proc(env);
	if(num_colors > 0) {
		env->cache_colors_map = cache_colors_map_alloc();
		for(int i=0; i<num_colors; i++)
			cache_color_alloc(llc_cache, env->cache_colors_map);
	}
	proc_decref(env, 1);
	proc_yield(e);
	return 0;
}

#ifdef __NETWORK__
// This is not a syscall we want. Its hacky. Here just for syscall stuff until get a stack.
static ssize_t sys_eth_write(env_t* e, const char *DANGEROUS buf, size_t len)
{
	extern int eth_up;

	if (eth_up) {

		if (len == 0)
			return 0;

		char *COUNT(len) _buf = user_mem_assert(e, buf, len, PTE_U);
		int total_sent = 0;
		int just_sent = 0;
		int cur_packet_len = 0;
		while (total_sent != len) {
			cur_packet_len = ((len - total_sent) > MAX_PACKET_DATA) ? MAX_PACKET_DATA : (len - total_sent);
			char* wrap_buffer = packet_wrap(_buf + total_sent, cur_packet_len);
			just_sent = send_frame(wrap_buffer, cur_packet_len + PACKET_HEADER_SIZE);

			if (just_sent < 0)
				return 0; // This should be an error code of its own

			if (wrap_buffer)
				kfree(wrap_buffer);

			total_sent += cur_packet_len;
		}

		return (ssize_t)len;

	}
	else
		return -EINVAL;
}

// This is not a syscall we want. Its hacky. Here just for syscall stuff until get a stack.
static ssize_t sys_eth_read(env_t* e, char *DANGEROUS buf, size_t len)
{
	extern int eth_up;

	if (eth_up) {
		extern int packet_waiting;
		extern int packet_buffer_size;
		extern char*CT(packet_buffer_size) packet_buffer;
		extern char*CT(MAX_FRAME_SIZE) packet_buffer_orig;
		extern int packet_buffer_pos;

		if (len == 0)
			return 0;

		char *CT(len) _buf = user_mem_assert(e, buf,len, PTE_U);

		if (packet_waiting == 0)
			return 0;

		int read_len = ((packet_buffer_pos + len) > packet_buffer_size) ? packet_buffer_size - packet_buffer_pos : len;

		memcpy(_buf, packet_buffer + packet_buffer_pos, read_len);

		packet_buffer_pos = packet_buffer_pos + read_len;

		if (packet_buffer_pos == packet_buffer_size) {
			kfree(packet_buffer_orig);
			packet_waiting = 0;
		}

		return read_len;
	}
	else
		return -EINVAL;
}
#endif // Network

//
/* END OF REMOTE SYSTEMCALL SUPPORT SYSCALLS. */
//

static ssize_t sys_shared_page_alloc(env_t* p1,
                                     void**DANGEROUS _addr, pid_t p2_id,
                                     int p1_flags, int p2_flags
                                    )
{
	//if (!VALID_USER_PERMS(p1_flags)) return -EPERM;
	//if (!VALID_USER_PERMS(p2_flags)) return -EPERM;

	void * COUNT(1) * COUNT(1) addr = user_mem_assert(p1, _addr, sizeof(void *),
                                                      PTE_USER_RW);
	struct proc *p2 = pid2proc(p2_id);
	if (!p2)
		return -EBADPROC;

	page_t* page;
	error_t e = upage_alloc(p1, &page);
	if (e < 0) {
		proc_decref(p2, 1);
		return e;
	}

	void* p2_addr = page_insert_in_range(p2->env_pgdir, page,
	                (void*SNT)UTEXT, (void*SNT)UTOP, p2_flags);
	if (p2_addr == NULL) {
		page_free(page);
		proc_decref(p2, 1);
		return -EFAIL;
	}

	void* p1_addr = page_insert_in_range(p1->env_pgdir, page,
	                (void*SNT)UTEXT, (void*SNT)UTOP, p1_flags);
	if(p1_addr == NULL) {
		page_remove(p2->env_pgdir, p2_addr);
		page_free(page);
		proc_decref(p2, 1);
		return -EFAIL;
	}
	*addr = p1_addr;
	proc_decref(p2, 1);
	return ESUCCESS;
}

static void sys_shared_page_free(env_t* p1, void*DANGEROUS addr, pid_t p2)
{
}

// Invalidate the cache of this core.  Only useful if you want a cold cache for
// performance testing reasons.
static void sys_cache_invalidate(void)
{
	#ifdef __i386__
		wbinvd();
	#endif
	return;
}

// Writes 'val' to 'num_writes' entries of the well-known array in the kernel
// address space.  It's just #defined to be some random 4MB chunk (which ought
// to be boot_alloced or something).  Meant to grab exclusive access to cache
// lines, to simulate doing something useful.
static void sys_cache_buster(struct proc *p, uint32_t num_writes,
                             uint32_t num_pages, uint32_t flags)
{ TRUSTEDBLOCK /* zra: this is not really part of the kernel */
	#define BUSTER_ADDR		0xd0000000  // around 512 MB deep
	#define MAX_WRITES		1048576*8
	#define MAX_PAGES		32
	#define INSERT_ADDR 	(UINFO + 2*PGSIZE) // should be free for these tests
	uint32_t* buster = (uint32_t*)BUSTER_ADDR;
	static spinlock_t buster_lock = SPINLOCK_INITIALIZER;
	uint64_t ticks = -1;
	page_t* a_page[MAX_PAGES];

	/* Strided Accesses or Not (adjust to step by cachelines) */
	uint32_t stride = 1;
	if (flags & BUSTER_STRIDED) {
		stride = 16;
		num_writes *= 16;
	}

	/* Shared Accesses or Not (adjust to use per-core regions)
	 * Careful, since this gives 8MB to each core, starting around 512MB.
	 * Also, doesn't separate memory for core 0 if it's an async call.
	 */
	if (!(flags & BUSTER_SHARED))
		buster = (uint32_t*)(BUSTER_ADDR + core_id() * 0x00800000);

	/* Start the timer, if we're asked to print this info*/
	if (flags & BUSTER_PRINT_TICKS)
		ticks = start_timing();

	/* Allocate num_pages (up to MAX_PAGES), to simulate doing some more
	 * realistic work.  Note we don't write to these pages, even if we pick
	 * unshared.  Mostly due to the inconvenience of having to match up the
	 * number of pages with the number of writes.  And it's unnecessary.
	 */
	if (num_pages) {
		spin_lock(&buster_lock);
		for (int i = 0; i < MIN(num_pages, MAX_PAGES); i++) {
			upage_alloc(p, &a_page[i]);
			page_insert(p->env_pgdir, a_page[i], (void*)INSERT_ADDR + PGSIZE*i,
			            PTE_USER_RW);
		}
		spin_unlock(&buster_lock);
	}

	if (flags & BUSTER_LOCKED)
		spin_lock(&buster_lock);
	for (int i = 0; i < MIN(num_writes, MAX_WRITES); i=i+stride)
		buster[i] = 0xdeadbeef;
	if (flags & BUSTER_LOCKED)
		spin_unlock(&buster_lock);

	if (num_pages) {
		spin_lock(&buster_lock);
		for (int i = 0; i < MIN(num_pages, MAX_PAGES); i++) {
			page_remove(p->env_pgdir, (void*)(INSERT_ADDR + PGSIZE * i));
			page_decref(a_page[i]);
		}
		spin_unlock(&buster_lock);
	}

	/* Print info */
	if (flags & BUSTER_PRINT_TICKS) {
		ticks = stop_timing(ticks);
		printk("%llu,", ticks);
	}
	return;
}

// Print a string to the system console.
// The string is exactly 'len' characters long.
// Destroys the environment on memory errors.
static ssize_t sys_cputs(env_t* e, const char *DANGEROUS s, size_t len)
{
	// Check that the user has permission to read memory [s, s+len).
	// Destroy the environment if not.
	char *COUNT(len) _s = user_mem_assert(e, s, len, PTE_USER_RO);

	// Print the string supplied by the user.
	printk("%.*s", len, _s);
	return (ssize_t)len;
}

// Read a character from the system console.
// Returns the character.
static uint16_t sys_cgetc(env_t* e)
{
	uint16_t c;

	// The cons_getc() primitive doesn't wait for a character,
	// but the sys_cgetc() system call does.
	while ((c = cons_getc()) == 0)
		cpu_relax();

	return c;
}

/* Returns the calling process's pid */
static pid_t sys_getpid(struct proc *p)
{
	return p->pid;
}

/* Returns the id of the cpu this syscall is executed on. */
static uint32_t sys_getcpuid(void)
{
	return core_id();
}

// TODO: Temporary hack until thread-local storage is implemented on i386
static size_t sys_getvcoreid(env_t* e)
{
	if(e->state == PROC_RUNNING_S)
		return 0;

	size_t i;
	for(i = 0; i < e->num_vcores; i++)
		if(core_id() == e->vcoremap[i])
			return i;

	panic("virtual core id not found in sys_getvcoreid()!");
}

/* Destroy proc pid.  If this is called by the dying process, it will never
 * return.  o/w it will return 0 on success, or an error.  Errors include:
 * - EBADPROC: if there is no such process with pid
 * - EPERM: if caller does not control pid */
static error_t sys_proc_destroy(struct proc *p, pid_t pid, int exitcode)
{
	error_t r;
	struct proc *p_to_die = pid2proc(pid);

	if (!p_to_die)
		return -EBADPROC;
	if (!proc_controls(p, p_to_die)) {
		proc_decref(p_to_die, 1);
		return -EPERM;
	}
	if (p_to_die == p) {
		// syscall code and pid2proc both have edible references, only need 1.
		p->exitcode = exitcode;
		proc_decref(p, 1);
		printd("[PID %d] proc exiting gracefully (code %d)\n", p->pid,exitcode);
	} else {
		panic("Destroying other processes is not supported yet.");
		//printk("[%d] destroying proc %d\n", p->pid, p_to_die->pid);
	}
	proc_destroy(p_to_die);
	return ESUCCESS;
}

/*
 * Creates a process found at the user string 'path'.  Currently uses KFS.
 * Not runnable by default, so it needs it's status to be changed so that the
 * next call to schedule() will try to run it.
 * TODO: once we have a decent VFS, consider splitting this up
 * and once there's an mmap, can have most of this in process.c
 */
static int sys_proc_create(struct proc *p, const char *DANGEROUS path)
{
	#define MAX_PATH_LEN 256 // totally arbitrary
	int pid = 0;
	char tpath[MAX_PATH_LEN];
	/*
	 * There's a bunch of issues with reading in the path, which we'll
	 * need to sort properly in the VFS.  Main concerns are TOCTOU (copy-in),
	 * whether or not it's a big deal that the pointer could be into kernel
	 * space, and resolving both of these without knowing the length of the
	 * string. (TODO)
	 * Change this so that all syscalls with a pointer take a length.
	 *
	 * zra: I've added this user_mem_strlcpy, which I think eliminates the
     * the TOCTOU issue. Adding a length arg to this call would allow a more
	 * efficient implementation, though, since only one call to user_mem_check
	 * would be required.
	 */
	int ret = user_mem_strlcpy(p,tpath, path, MAX_PATH_LEN, PTE_USER_RO);
	int kfs_inode = kfs_lookup_path(tpath);
	if (kfs_inode < 0)
		return -EINVAL;
	struct proc *new_p = kfs_proc_create(kfs_inode);
	pid = new_p->pid;
	proc_decref(new_p, 1); // let go of the reference created in proc_create()
	return pid;
}

/* Makes process PID runnable.  Consider moving the functionality to process.c */
static error_t sys_proc_run(struct proc *p, unsigned pid)
{
	struct proc *target = pid2proc(pid);
	error_t retval = 0;

	if (!target)
		return -EBADPROC;
 	// note we can get interrupted here. it's not bad.
	spin_lock_irqsave(&p->proc_lock);
	// make sure we have access and it's in the right state to be activated
	if (!proc_controls(p, target)) {
		proc_decref(target, 1);
		retval = -EPERM;
	} else if (target->state != PROC_CREATED) {
		proc_decref(target, 1);
		retval = -EINVAL;
	} else {
		__proc_set_state(target, PROC_RUNNABLE_S);
		schedule_proc(target);
	}
	spin_unlock_irqsave(&p->proc_lock);
	proc_decref(target, 1);
	return retval;
}

static error_t sys_brk(struct proc *p, void* addr) {
	size_t range;

	if((addr < p->heap_bottom) || (addr >= (void*)USTACKBOT))
		return -EINVAL;
	if(addr == p->heap_top)
		return ESUCCESS;

	if (addr > p->heap_top) {
		range = addr - p->heap_top;
		env_segment_alloc(p, p->heap_top, range);
	}
	else if (addr < p->heap_top) {
		range = p->heap_top - addr;
		env_segment_free(p, addr, range);
	}
	p->heap_top = addr;
	return ESUCCESS;
}

/* Executes the given syscall.
 *
 * Note tf is passed in, which points to the tf of the context on the kernel
 * stack.  If any syscall needs to block, it needs to save this info, as well as
 * any silly state.
 *
 * TODO: Build a dispatch table instead of switching on the syscallno
 * Dispatches to the correct kernel function, passing the arguments.
 */
intreg_t syscall(struct proc *p, uintreg_t syscallno, uintreg_t a1,
                 uintreg_t a2, uintreg_t a3, uintreg_t a4, uintreg_t a5)
{
	// Call the function corresponding to the 'syscallno' parameter.
	// Return any appropriate return value.

	//cprintf("Incoming syscall on core: %d number: %d\n    a1: %x\n   "
	//        " a2: %x\n    a3: %x\n    a4: %x\n    a5: %x\n", core_id(),
	//        syscallno, a1, a2, a3, a4, a5);

	// used if we need more args, like in mmap
	int32_t _a4, _a5, _a6, *COUNT(3) args;

	assert(p); // should always have a process for every syscall
	//printk("Running syscall: %d\n", syscallno);
	if (INVALID_SYSCALL(syscallno))
		return -EINVAL;

	switch (syscallno) {
		case SYS_null:
			sys_null();
			return ESUCCESS;
		case SYS_cache_buster:
			sys_cache_buster(p, a1, a2, a3);
			return 0;
		case SYS_cache_invalidate:
			sys_cache_invalidate();
			return 0;
		case SYS_shared_page_alloc:
			return sys_shared_page_alloc(p, (void** DANGEROUS) a1,
		                                 a2, (int) a3, (int) a4);
		case SYS_shared_page_free:
			sys_shared_page_free(p, (void* DANGEROUS) a1, a2);
		    return ESUCCESS;
		case SYS_cputs:
			return sys_cputs(p, (char *DANGEROUS)a1, (size_t)a2);
		case SYS_cgetc:
			return sys_cgetc(p); // this will need to block
		case SYS_getcpuid:
			return sys_getcpuid();
		case SYS_getvcoreid:
			return sys_getvcoreid(p);
		case SYS_getpid:
			return sys_getpid(p);
		case SYS_proc_destroy:
			return sys_proc_destroy(p, (pid_t)a1, (int)a2);
		case SYS_yield:
			proc_yield(p);
			return ESUCCESS;
		case SYS_proc_create:
			return sys_proc_create(p, (char *DANGEROUS)a1);
		case SYS_proc_run:
			return sys_proc_run(p, (size_t)a1);
		case SYS_mmap:
			// we only have 4 parameters from sysenter currently, need to copy
			// in the others.  if we stick with this, we can make a func for it.
			args = user_mem_assert(p, (void*DANGEROUS)a4,
			                       3*sizeof(_a4), PTE_USER_RW);
			_a4 = args[0];
			_a5 = args[1];
			_a6 = args[2];
			return (intreg_t) mmap(p, a1, a2, a3, _a4, _a5, _a6);
		case SYS_brk:
			return sys_brk(p, (void*)a1);
		case SYS_resource_req:
			return resource_req(p, a1, a2, a3, a4);

	#ifdef __i386__
		case SYS_serial_write:
			return sys_serial_write(p, (char *DANGEROUS)a1, (size_t)a2);
		case SYS_serial_read:
			return sys_serial_read(p, (char *DANGEROUS)a1, (size_t)a2);
	#endif
		case SYS_run_binary:
			return sys_run_binary(p, (char *DANGEROUS)a1, (size_t)a2, (void* DANGEROUS)a3, (size_t)a4);
	#ifdef __NETWORK__
		case SYS_eth_write:
			return sys_eth_write(p, (char *DANGEROUS)a1, (size_t)a2);
		case SYS_eth_read:
			return sys_eth_read(p, (char *DANGEROUS)a1, (size_t)a2);
	#endif
	#ifdef __sparc_v8__
		case SYS_frontend:
			return frontend_syscall_from_user(p,a1,a2,a3,a4,a5);
	#endif

		case SYS_reboot:
			reboot();
			return 0;

		case SYS_fork:
			return sys_fork(p);

		case SYS_trywait:
			return sys_trywait(p,(pid_t)a1,(int*)a2);

		case SYS_exec:
			return sys_exec(p, (char *DANGEROUS)a1, (size_t)a2, (void* DANGEROUS)a3, (void* DANGEROUS)a4);

		default:
			// or just return -EINVAL
			panic("Invalid syscall number %d for proc %x!", syscallno, *p);
	}
	return 0xdeadbeef;
}

intreg_t syscall_async(struct proc *p, syscall_req_t *call)
{
	return syscall(p, call->num, call->args[0], call->args[1],
	               call->args[2], call->args[3], call->args[4]);
}

/* You should already have a refcnt'd ref to p before calling this */
intreg_t process_generic_syscalls(struct proc *p, size_t max)
{
	size_t count = 0;
	syscall_back_ring_t* sysbr = &p->syscallbackring;

	/* make sure the proc is still alive, and keep it from dying from under us
	 * incref will return ESUCCESS on success.  This might need some thought
	 * regarding when the incref should have happened (like by whoever passed us
	 * the *p). */
	// TODO: ought to be unnecessary, if you called this right, kept here for
	// now in case anyone actually uses the ARSCs.
	proc_incref(p, 1);

	// max is the most we'll process.  max = 0 means do as many as possible
	while (RING_HAS_UNCONSUMED_REQUESTS(sysbr) && ((!max)||(count < max)) ) {
		if (!count) {
			// ASSUME: one queue per process
			// only switch cr3 for the very first request for this queue
			// need to switch to the right context, so we can handle the user pointer
			// that points to a data payload of the syscall
			lcr3(p->env_cr3);
		}
		count++;
		//printk("DEBUG PRE: sring->req_prod: %d, sring->rsp_prod: %d\n",
		//	   sysbr->sring->req_prod, sysbr->sring->rsp_prod);
		// might want to think about 0-ing this out, if we aren't
		// going to explicitly fill in all fields
		syscall_rsp_t rsp;
		// this assumes we get our answer immediately for the syscall.
		syscall_req_t* req = RING_GET_REQUEST(sysbr, ++(sysbr->req_cons));
		rsp.retval = syscall_async(p, req);
		// write response into the slot it came from
		memcpy(req, &rsp, sizeof(syscall_rsp_t));
		// update our counter for what we've produced (assumes we went in order!)
		(sysbr->rsp_prod_pvt)++;
		RING_PUSH_RESPONSES(sysbr);
		//printk("DEBUG POST: sring->req_prod: %d, sring->rsp_prod: %d\n",
		//	   sysbr->sring->req_prod, sysbr->sring->rsp_prod);
	}
	// load sane page tables (and don't rely on decref to do it for you).
	lcr3(boot_cr3);
	proc_decref(p, 1);
	return (intreg_t)count;
}
