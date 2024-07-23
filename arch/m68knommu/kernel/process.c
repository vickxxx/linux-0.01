/*
 *  linux/arch/m68knommu/kernel/process.c
 *
 *  Copyright (C) 1995  Hamish Macdonald
 *
 *  68060 fixes by Jesper Skov
 *
 *  uClinux changes
 *  Copyright (C) 2000-2002, David McCullough <davidm@snapgear.com>
 */

/*
 * This file handles the architecture-dependent parts of process handling..
 */

#include <linux/config.h>
#include <linux/errno.h>
#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/smp.h>
#include <linux/smp_lock.h>
#include <linux/stddef.h>
#include <linux/unistd.h>
#include <linux/ptrace.h>
#include <linux/slab.h>
#include <linux/user.h>
#include <linux/a.out.h>
#include <linux/interrupt.h>
#include <linux/reboot.h>

#include <asm/uaccess.h>
#include <asm/system.h>
#include <asm/traps.h>
#include <asm/machdep.h>
#include <asm/setup.h>
#include <asm/pgtable.h>

asmlinkage void ret_from_fork(void);


/*
 * The idle loop on an m68knommu..
 */
void default_idle(void)
{
	while(1) {
		if (need_resched())
			__asm__("stop #0x2000" : : : "cc");
		schedule();
	}
}

void (*idle)(void) = default_idle;

/*
 * The idle thread. There's no useful work to be
 * done, so just try to conserve power and have a
 * low exit latency (ie sit in a loop waiting for
 * somebody to say that they'd like to reschedule)
 */
void cpu_idle(void)
{
	/* endless idle loop with no priority at all */
	idle();
}

void machine_restart(char * __unused)
{
	if (mach_reset)
		mach_reset();
	for (;;);
}

void machine_halt(void)
{
	if (mach_halt)
		mach_halt();
	for (;;);
}

void machine_power_off(void)
{
	if (mach_power_off)
		mach_power_off();
	for (;;);
}

void show_regs(struct pt_regs * regs)
{
	printk("\n");
	printk("Format %02x  Vector: %04x  PC: %08lx  Status: %04x    %s\n",
	       regs->format, regs->vector, regs->pc, regs->sr, print_tainted());
	printk("ORIG_D0: %08lx  D0: %08lx  A2: %08lx  A1: %08lx\n",
	       regs->orig_d0, regs->d0, regs->a2, regs->a1);
	printk("A0: %08lx  D5: %08lx  D4: %08lx\n",
	       regs->a0, regs->d5, regs->d4);
	printk("D3: %08lx  D2: %08lx  D1: %08lx\n",
	       regs->d3, regs->d2, regs->d1);
	if (!(regs->sr & PS_S))
		printk("USP: %08lx\n", rdusp());
}

/*
 * Create a kernel thread
 */
int kernel_thread(int (*fn)(void *), void * arg, unsigned long flags)
{
	long retval;
	long clone_arg = flags | CLONE_VM;
	mm_segment_t fs;

	fs = get_fs();
	set_fs(KERNEL_DS);

	__asm__ __volatile__ (
			"movel	%%sp, %%d2\n\t"
			"movel	%5, %%d1\n\t"
			"movel	%1, %%d0\n\t"
			"trap	#0\n\t"
			"cmpl	%%sp, %%d2\n\t"
			"jeq	1f\n\t"
			"movel	%3, %%sp@-\n\t"
			"jsr	%4@\n\t"
			"movel	%2, %%d0\n\t"
			"trap	#0\n"
			"1:"
		: "=d" (retval)
		: "i" (__NR_clone),
		  "i" (__NR_exit),
		  "a" (arg),
		  "a" (fn),
		  "a" (clone_arg)
		: "cc", "%d0", "%d1", "%d2");

	set_fs(fs);
	return retval;
}

void flush_thread(void)
{
#ifdef CONFIG_FPU
	unsigned long zero = 0;
#endif
	set_fs(USER_DS);
	current->thread.fs = __USER_DS;
#ifdef CONFIG_FPU
	if (!FPU_IS_EMU)
		asm volatile (".chip 68k/68881\n\t"
			      "frestore %0@\n\t"
			      ".chip 68k" : : "a" (&zero));
#endif
}

/*
 * "m68k_fork()".. By the time we get here, the
 * non-volatile registers have also been saved on the
 * stack. We do some ugly pointer stuff here.. (see
 * also copy_thread)
 */

asmlinkage int m68k_fork(struct pt_regs *regs)
{
	/* fork almost works, enough to trick you into looking elsewhere :-( */
	return(-EINVAL);
}

asmlinkage int m68k_vfork(struct pt_regs *regs)
{
	return do_fork(CLONE_VFORK | CLONE_VM | SIGCHLD, rdusp(), regs, 0, NULL, NULL);
}

asmlinkage int m68k_clone(struct pt_regs *regs)
{
	unsigned long clone_flags;
	unsigned long newsp;

	/* syscall2 puts clone_flags in d1 and usp in d2 */
	clone_flags = regs->d1;
	newsp = regs->d2;
	if (!newsp)
		newsp = rdusp();
        return do_fork(clone_flags & ~CLONE_IDLETASK, newsp, regs, 0, NULL, NULL);
}

int copy_thread(int nr, unsigned long clone_flags,
		unsigned long usp, unsigned long topstk,
		struct task_struct * p, struct pt_regs * regs)
{
	struct pt_regs * childregs;
	struct switch_stack * childstack, *stack;
	unsigned long stack_offset, *retp;

	stack_offset = KTHREAD_SIZE - sizeof(struct pt_regs);
	childregs = (struct pt_regs *) ((unsigned long) p->thread_info + stack_offset);

	*childregs = *regs;
	childregs->d0 = 0;

	retp = ((unsigned long *) regs);
	stack = ((struct switch_stack *) retp) - 1;

	childstack = ((struct switch_stack *) childregs) - 1;
	*childstack = *stack;
	childstack->retpc = (unsigned long)ret_from_fork;

	p->thread.usp = usp;
	p->thread.ksp = (unsigned long)childstack;
	/*
	 * Must save the current SFC/DFC value, NOT the value when
	 * the parent was last descheduled - RGH  10-08-96
	 */
	p->thread.fs = get_fs().seg;

#ifdef CONFIG_FPU
	if (!FPU_IS_EMU) {
		/* Copy the current fpu state */
		asm volatile ("fsave %0" : : "m" (p->thread.fpstate[0]) : "memory");

		if (p->thread.fpstate[0])
		  asm volatile ("fmovemx %/fp0-%/fp7,%0\n\t"
				"fmoveml %/fpiar/%/fpcr/%/fpsr,%1"
				: : "m" (p->thread.fp[0]), "m" (p->thread.fpcntl[0])
				: "memory");
		/* Restore the state in case the fpu was busy */
		asm volatile ("frestore %0" : : "m" (p->thread.fpstate[0]));
	}
#endif

	return 0;
}

/* Fill in the fpu structure for a core dump.  */

int dump_fpu(struct pt_regs *regs, struct user_m68kfp_struct *fpu)
{
#ifdef CONFIG_FPU
	char fpustate[216];

	if (FPU_IS_EMU) {
		int i;

		memcpy(fpu->fpcntl, current->thread.fpcntl, 12);
		memcpy(fpu->fpregs, current->thread.fp, 96);
		/* Convert internal fpu reg representation
		 * into long double format
		 */
		for (i = 0; i < 24; i += 3)
			fpu->fpregs[i] = ((fpu->fpregs[i] & 0xffff0000) << 15) |
			                 ((fpu->fpregs[i] & 0x0000ffff) << 16);
		return 1;
	}

	/* First dump the fpu context to avoid protocol violation.  */
	asm volatile ("fsave %0" :: "m" (fpustate[0]) : "memory");
	if (!fpustate[0])
		return 0;

	asm volatile ("fmovem %/fpiar/%/fpcr/%/fpsr,%0"
		:: "m" (fpu->fpcntl[0])
		: "memory");
	asm volatile ("fmovemx %/fp0-%/fp7,%0"
		:: "m" (fpu->fpregs[0])
		: "memory");
#endif
	return 1;
}

/*
 * fill in the user structure for a core dump..
 */
void dump_thread(struct pt_regs * regs, struct user * dump)
{
	struct switch_stack *sw;

	/* changed the size calculations - should hopefully work better. lbt */
	dump->magic = CMAGIC;
	dump->start_code = 0;
	dump->start_stack = rdusp() & ~(PAGE_SIZE - 1);
	dump->u_tsize = ((unsigned long) current->mm->end_code) >> PAGE_SHIFT;
	dump->u_dsize = ((unsigned long) (current->mm->brk +
					  (PAGE_SIZE-1))) >> PAGE_SHIFT;
	dump->u_dsize -= dump->u_tsize;
	dump->u_ssize = 0;

	if (dump->start_stack < TASK_SIZE)
		dump->u_ssize = ((unsigned long) (TASK_SIZE - dump->start_stack)) >> PAGE_SHIFT;

	dump->u_ar0 = (struct user_regs_struct *)((int)&dump->regs - (int)dump);
	sw = ((struct switch_stack *)regs) - 1;
	dump->regs.d1 = regs->d1;
	dump->regs.d2 = regs->d2;
	dump->regs.d3 = regs->d3;
	dump->regs.d4 = regs->d4;
	dump->regs.d5 = regs->d5;
	dump->regs.d6 = sw->d6;
	dump->regs.d7 = sw->d7;
	dump->regs.a0 = regs->a0;
	dump->regs.a1 = regs->a1;
	dump->regs.a2 = regs->a2;
	dump->regs.a3 = sw->a3;
	dump->regs.a4 = sw->a4;
	dump->regs.a5 = sw->a5;
	dump->regs.a6 = sw->a6;
	dump->regs.d0 = regs->d0;
	dump->regs.orig_d0 = regs->orig_d0;
	dump->regs.stkadj = regs->stkadj;
	dump->regs.sr = regs->sr;
	dump->regs.pc = regs->pc;
	dump->regs.fmtvec = (regs->format << 12) | regs->vector;
	/* dump floating point stuff */
	dump->u_fpvalid = dump_fpu (regs, &dump->m68kfp);
}

/*
 *	Generic dumping code. Used for panic and debug.
 */
void dump(struct pt_regs *fp)
{
	unsigned long	*sp;
	unsigned char	*tp;
	int		i;

	printk("\nCURRENT PROCESS:\n\n");
	printk("COMM=%s PID=%d\n", current->comm, current->pid);

	if (current->mm) {
		printk("TEXT=%08x-%08x DATA=%08x-%08x BSS=%08x-%08x\n",
			(int) current->mm->start_code,
			(int) current->mm->end_code,
			(int) current->mm->start_data,
			(int) current->mm->end_data,
			(int) current->mm->end_data,
			(int) current->mm->brk);
		printk("USER-STACK=%08x  KERNEL-STACK=%08x\n\n",
			(int) current->mm->start_stack,
			(int)(((unsigned long) current) + KTHREAD_SIZE));
	}

	printk("PC: %08lx\n", fp->pc);
	printk("SR: %08lx    SP: %08lx\n", (long) fp->sr, (long) fp);
	printk("d0: %08lx    d1: %08lx    d2: %08lx    d3: %08lx\n",
		fp->d0, fp->d1, fp->d2, fp->d3);
	printk("d4: %08lx    d5: %08lx    a0: %08lx    a1: %08lx\n",
		fp->d4, fp->d5, fp->a0, fp->a1);
	printk("\nUSP: %08x   TRAPFRAME: %08x\n", (unsigned int) rdusp(),
		(unsigned int) fp);

	printk("\nCODE:");
	tp = ((unsigned char *) fp->pc) - 0x20;
	for (sp = (unsigned long *) tp, i = 0; (i < 0x40);  i += 4) {
		if ((i % 0x10) == 0)
			printk("\n%08x: ", (int) (tp + i));
		printk("%08x ", (int) *sp++);
	}
	printk("\n");

	printk("\nKERNEL STACK:");
	tp = ((unsigned char *) fp) - 0x40;
	for (sp = (unsigned long *) tp, i = 0; (i < 0xc0); i += 4) {
		if ((i % 0x10) == 0)
			printk("\n%08x: ", (int) (tp + i));
		printk("%08x ", (int) *sp++);
	}
	printk("\n");
	printk("\n");

	printk("\nUSER STACK:");
	tp = (unsigned char *) (rdusp() - 0x10);
	for (sp = (unsigned long *) tp, i = 0; (i < 0x80); i += 4) {
		if ((i % 0x10) == 0)
			printk("\n%08x: ", (int) (tp + i));
		printk("%08x ", (int) *sp++);
	}
	printk("\n\n");
}

/*
 * sys_execve() executes a new program.
 */
asmlinkage int sys_execve(char *name, char **argv, char **envp)
{
	int error;
	char * filename;
	struct pt_regs *regs = (struct pt_regs *) &name;

	lock_kernel();
	filename = getname(name);
	error = PTR_ERR(filename);
	if (IS_ERR(filename))
		goto out;
	error = do_execve(filename, argv, envp, regs);
	putname(filename);
out:
	unlock_kernel();
	return error;
}

/*
 * These bracket the sleeping functions..
 */
extern void scheduling_functions_start_here(void);
extern void scheduling_functions_end_here(void);
#define first_sched	((unsigned long) scheduling_functions_start_here)
#define last_sched	((unsigned long) scheduling_functions_end_here)

unsigned long get_wchan(struct task_struct *p)
{
	unsigned long fp, pc;
	unsigned long stack_page;
	int count = 0;
	if (!p || p == current || p->state == TASK_RUNNING)
		return 0;

	stack_page = (unsigned long)p;
	fp = ((struct switch_stack *)p->thread.ksp)->a6;
	do {
		if (fp < stack_page+sizeof(struct task_struct) ||
		    fp >= 8184+stack_page)
			return 0;
		pc = ((unsigned long *)fp)[1];
		/* FIXME: This depends on the order of these functions. */
		if (pc < first_sched || pc >= last_sched)
			return pc;
		fp = *(unsigned long *) fp;
	} while (count++ < 16);
	return 0;
}

/*
 * Return saved PC of a blocked thread.
 */
unsigned long thread_saved_pc(struct task_struct *tsk)
{
	extern void scheduling_functions_start_here(void);
	extern void scheduling_functions_end_here(void);
	struct switch_stack *sw = (struct switch_stack *)tsk->thread.ksp;

	/* Check whether the thread is blocked in resume() */
	if (sw->retpc > (unsigned long)scheduling_functions_start_here &&
	    sw->retpc < (unsigned long)scheduling_functions_end_here)
		return ((unsigned long *)sw->a6)[1];
	else
		return sw->retpc;
}

