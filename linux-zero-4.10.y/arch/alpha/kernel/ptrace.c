/* ptrace.c */
/* By Ross Biro 1/23/92 */
/* edited by Linus Torvalds */
/* mangled further by Bob Manson (manson@santafe.edu) */
/* more mutilation by David Mosberger (davidm@azstarnet.com) */

#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/mm.h>
#include <linux/smp.h>
#include <linux/errno.h>
#include <linux/ptrace.h>
#include <linux/user.h>
#include <linux/security.h>
#include <linux/signal.h>
#include <linux/tracehook.h>
#include <linux/audit.h>

#include <linux/uaccess.h>
#include <asm/pgtable.h>
#include <asm/fpu.h>

#include "proto.h"

#define DEBUG	DBG_MEM
#undef DEBUG

#ifdef DEBUG
enum {
	DBG_MEM		= (1<<0),
	DBG_BPT		= (1<<1),
	DBG_MEM_ALL	= (1<<2)
};
#define DBG(fac,args)	{if ((fac) & DEBUG) printk args;}
#else
#define DBG(fac,args)
#endif

#define BREAKINST	0x00000080	/* call_pal bpt */

/*
 * does not yet catch signals sent when the child dies.
 * in exit.c or in signal.c.
 */

/*
 * Processes always block with the following stack-layout:
 *
 *  +================================+ <---- task + 2*PAGE_SIZE
 *  | PALcode saved frame (ps, pc,   | ^
 *  | gp, a0, a1, a2)		     | |
 *  +================================+ | struct pt_regs
 *  |	        		     | |
 *  | frame generated by SAVE_ALL    | |
 *  |	        		     | v
 *  +================================+
 *  |	        		     | ^
 *  | frame saved by do_switch_stack | | struct switch_stack
 *  |	        		     | v
 *  +================================+
 */

/* 
 * The following table maps a register index into the stack offset at
 * which the register is saved.  Register indices are 0-31 for integer
 * regs, 32-63 for fp regs, and 64 for the pc.  Notice that sp and
 * zero have no stack-slot and need to be treated specially (see
 * get_reg/put_reg below).
 */
enum {
	REG_R0 = 0, REG_F0 = 32, REG_FPCR = 63, REG_PC = 64
};

#define PT_REG(reg) \
  (PAGE_SIZE*2 - sizeof(struct pt_regs) + offsetof(struct pt_regs, reg))

#define SW_REG(reg) \
 (PAGE_SIZE*2 - sizeof(struct pt_regs) - sizeof(struct switch_stack) \
  + offsetof(struct switch_stack, reg))

static int regoff[] = {
	PT_REG(	   r0), PT_REG(	   r1), PT_REG(	   r2), PT_REG(	  r3),
	PT_REG(	   r4), PT_REG(	   r5), PT_REG(	   r6), PT_REG(	  r7),
	PT_REG(	   r8), SW_REG(	   r9), SW_REG(	  r10), SW_REG(	 r11),
	SW_REG(	  r12), SW_REG(	  r13), SW_REG(	  r14), SW_REG(	 r15),
	PT_REG(	  r16), PT_REG(	  r17), PT_REG(	  r18), PT_REG(	 r19),
	PT_REG(	  r20), PT_REG(	  r21), PT_REG(	  r22), PT_REG(	 r23),
	PT_REG(	  r24), PT_REG(	  r25), PT_REG(	  r26), PT_REG(	 r27),
	PT_REG(	  r28), PT_REG(	   gp),		   -1,		   -1,
	SW_REG(fp[ 0]), SW_REG(fp[ 1]), SW_REG(fp[ 2]), SW_REG(fp[ 3]),
	SW_REG(fp[ 4]), SW_REG(fp[ 5]), SW_REG(fp[ 6]), SW_REG(fp[ 7]),
	SW_REG(fp[ 8]), SW_REG(fp[ 9]), SW_REG(fp[10]), SW_REG(fp[11]),
	SW_REG(fp[12]), SW_REG(fp[13]), SW_REG(fp[14]), SW_REG(fp[15]),
	SW_REG(fp[16]), SW_REG(fp[17]), SW_REG(fp[18]), SW_REG(fp[19]),
	SW_REG(fp[20]), SW_REG(fp[21]), SW_REG(fp[22]), SW_REG(fp[23]),
	SW_REG(fp[24]), SW_REG(fp[25]), SW_REG(fp[26]), SW_REG(fp[27]),
	SW_REG(fp[28]), SW_REG(fp[29]), SW_REG(fp[30]), SW_REG(fp[31]),
	PT_REG(	   pc)
};

static unsigned long zero;

/*
 * Get address of register REGNO in task TASK.
 */
static unsigned long *
get_reg_addr(struct task_struct * task, unsigned long regno)
{
	unsigned long *addr;

	if (regno == 30) {
		addr = &task_thread_info(task)->pcb.usp;
	} else if (regno == 65) {
		addr = &task_thread_info(task)->pcb.unique;
	} else if (regno == 31 || regno > 65) {
		zero = 0;
		addr = &zero;
	} else {
		addr = task_stack_page(task) + regoff[regno];
	}
	return addr;
}

/*
 * Get contents of register REGNO in task TASK.
 */
static unsigned long
get_reg(struct task_struct * task, unsigned long regno)
{
	/* Special hack for fpcr -- combine hardware and software bits.  */
	if (regno == 63) {
		unsigned long fpcr = *get_reg_addr(task, regno);
		unsigned long swcr
		  = task_thread_info(task)->ieee_state & IEEE_SW_MASK;
		swcr = swcr_update_status(swcr, fpcr);
		return fpcr | swcr;
	}
	return *get_reg_addr(task, regno);
}

/*
 * Write contents of register REGNO in task TASK.
 */
static int
put_reg(struct task_struct *task, unsigned long regno, unsigned long data)
{
	if (regno == 63) {
		task_thread_info(task)->ieee_state
		  = ((task_thread_info(task)->ieee_state & ~IEEE_SW_MASK)
		     | (data & IEEE_SW_MASK));
		data = (data & FPCR_DYN_MASK) | ieee_swcr_to_fpcr(data);
	}
	*get_reg_addr(task, regno) = data;
	return 0;
}

static inline int
read_int(struct task_struct *task, unsigned long addr, int * data)
{
	int copied = access_process_vm(task, addr, data, sizeof(int),
			FOLL_FORCE);
	return (copied == sizeof(int)) ? 0 : -EIO;
}

static inline int
write_int(struct task_struct *task, unsigned long addr, int data)
{
	int copied = access_process_vm(task, addr, &data, sizeof(int),
			FOLL_FORCE | FOLL_WRITE);
	return (copied == sizeof(int)) ? 0 : -EIO;
}

/*
 * Set breakpoint.
 */
int
ptrace_set_bpt(struct task_struct * child)
{
	int displ, i, res, reg_b, nsaved = 0;
	unsigned int insn, op_code;
	unsigned long pc;

	pc  = get_reg(child, REG_PC);
	res = read_int(child, pc, (int *) &insn);
	if (res < 0)
		return res;

	op_code = insn >> 26;
	if (op_code >= 0x30) {
		/*
		 * It's a branch: instead of trying to figure out
		 * whether the branch will be taken or not, we'll put
		 * a breakpoint at either location.  This is simpler,
		 * more reliable, and probably not a whole lot slower
		 * than the alternative approach of emulating the
		 * branch (emulation can be tricky for fp branches).
		 */
		displ = ((s32)(insn << 11)) >> 9;
		task_thread_info(child)->bpt_addr[nsaved++] = pc + 4;
		if (displ)		/* guard against unoptimized code */
			task_thread_info(child)->bpt_addr[nsaved++]
			  = pc + 4 + displ;
		DBG(DBG_BPT, ("execing branch\n"));
	} else if (op_code == 0x1a) {
		reg_b = (insn >> 16) & 0x1f;
		task_thread_info(child)->bpt_addr[nsaved++] = get_reg(child, reg_b);
		DBG(DBG_BPT, ("execing jump\n"));
	} else {
		task_thread_info(child)->bpt_addr[nsaved++] = pc + 4;
		DBG(DBG_BPT, ("execing normal insn\n"));
	}

	/* install breakpoints: */
	for (i = 0; i < nsaved; ++i) {
		res = read_int(child, task_thread_info(child)->bpt_addr[i],
			       (int *) &insn);
		if (res < 0)
			return res;
		task_thread_info(child)->bpt_insn[i] = insn;
		DBG(DBG_BPT, ("    -> next_pc=%lx\n",
			      task_thread_info(child)->bpt_addr[i]));
		res = write_int(child, task_thread_info(child)->bpt_addr[i],
				BREAKINST);
		if (res < 0)
			return res;
	}
	task_thread_info(child)->bpt_nsaved = nsaved;
	return 0;
}

/*
 * Ensure no single-step breakpoint is pending.  Returns non-zero
 * value if child was being single-stepped.
 */
int
ptrace_cancel_bpt(struct task_struct * child)
{
	int i, nsaved = task_thread_info(child)->bpt_nsaved;

	task_thread_info(child)->bpt_nsaved = 0;

	if (nsaved > 2) {
		printk("ptrace_cancel_bpt: bogus nsaved: %d!\n", nsaved);
		nsaved = 2;
	}

	for (i = 0; i < nsaved; ++i) {
		write_int(child, task_thread_info(child)->bpt_addr[i],
			  task_thread_info(child)->bpt_insn[i]);
	}
	return (nsaved != 0);
}

void user_enable_single_step(struct task_struct *child)
{
	/* Mark single stepping.  */
	task_thread_info(child)->bpt_nsaved = -1;
}

void user_disable_single_step(struct task_struct *child)
{
	ptrace_cancel_bpt(child);
}

/*
 * Called by kernel/ptrace.c when detaching..
 *
 * Make sure the single step bit is not set.
 */
void ptrace_disable(struct task_struct *child)
{ 
	user_disable_single_step(child);
}

long arch_ptrace(struct task_struct *child, long request,
		 unsigned long addr, unsigned long data)
{
	unsigned long tmp;
	size_t copied;
	long ret;

	switch (request) {
	/* When I and D space are separate, these will need to be fixed.  */
	case PTRACE_PEEKTEXT: /* read word at location addr. */
	case PTRACE_PEEKDATA:
		copied = ptrace_access_vm(child, addr, &tmp, sizeof(tmp),
				FOLL_FORCE);
		ret = -EIO;
		if (copied != sizeof(tmp))
			break;
		
		force_successful_syscall_return();
		ret = tmp;
		break;

	/* Read register number ADDR. */
	case PTRACE_PEEKUSR:
		force_successful_syscall_return();
		ret = get_reg(child, addr);
		DBG(DBG_MEM, ("peek $%lu->%#lx\n", addr, ret));
		break;

	/* When I and D space are separate, this will have to be fixed.  */
	case PTRACE_POKETEXT: /* write the word at location addr. */
	case PTRACE_POKEDATA:
		ret = generic_ptrace_pokedata(child, addr, data);
		break;

	case PTRACE_POKEUSR: /* write the specified register */
		DBG(DBG_MEM, ("poke $%lu<-%#lx\n", addr, data));
		ret = put_reg(child, addr, data);
		break;
	default:
		ret = ptrace_request(child, request, addr, data);
		break;
	}
	return ret;
}

asmlinkage unsigned long syscall_trace_enter(void)
{
	unsigned long ret = 0;
	struct pt_regs *regs = current_pt_regs();
	if (test_thread_flag(TIF_SYSCALL_TRACE) &&
	    tracehook_report_syscall_entry(current_pt_regs()))
		ret = -1UL;
	audit_syscall_entry(regs->r0, regs->r16, regs->r17, regs->r18, regs->r19);
	return ret ?: current_pt_regs()->r0;
}

asmlinkage void
syscall_trace_leave(void)
{
	audit_syscall_exit(current_pt_regs());
	if (test_thread_flag(TIF_SYSCALL_TRACE))
		tracehook_report_syscall_exit(current_pt_regs(), 0);
}
