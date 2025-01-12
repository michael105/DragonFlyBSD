/*
 * Copyright (c) 1991 Regents of the University of California.
 * All rights reserved.
 *
 * This code is derived from software contributed to Berkeley by
 * the Systems Programming Group of the University of Utah Computer
 * Science Department and William Jolitz of UUNET Technologies Inc.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * Derived from hp300 version by Mike Hibler, this version by William
 * Jolitz uses a recursive map [a pde points to the page directory] to
 * map the page tables using the pagetables themselves. This is done to
 * reduce the impact on kernel virtual memory for lots of sparse address
 * space, and to reduce the cost of memory to each process.
 *
 * from: hp300: @(#)pmap.h	7.2 (Berkeley) 12/16/90
 * from: @(#)pmap.h	7.4 (Berkeley) 5/12/91
 * $FreeBSD: src/sys/i386/include/pmap.h,v 1.65.2.3 2001/10/03 07:15:37 peter Exp $
 */

#ifndef _MACHINE_PMAP_H_
#define	_MACHINE_PMAP_H_

#include <cpu/pmap.h>

/*
 * Pte related macros
 */
#define KVADDR(l4, l3, l2, l1) ( \
	((unsigned long)-1 << 47) | \
	((unsigned long)(l4) << PML4SHIFT) | \
	((unsigned long)(l3) << PDPSHIFT) | \
	((unsigned long)(l2) << PDRSHIFT) | \
	((unsigned long)(l1) << PAGE_SHIFT))

#define UVADDR(l4, l3, l2, l1) ( \
	((unsigned long)(l4) << PML4SHIFT) | \
	((unsigned long)(l3) << PDPSHIFT) | \
	((unsigned long)(l2) << PDRSHIFT) | \
	((unsigned long)(l1) << PAGE_SHIFT))

/*
 * Initial number of kernel page tables.  NKPT is now calculated in the
 * pmap code.
 *
 * Give NKPDPE a generous value, allowing the kernel to map up to 128G.
 */
#define NKPML4E		1		/* number of kernel PML4 slots */
#define NKPDPE		128		/* number of kernel PDP slots */

#define	NUPDP_TOTAL	(NPML4EPG/2)		/* total PDP pages */
#define	NUPD_TOTAL	(NUPDP_TOTAL*NPDPEPG)	/* total PD pages */
#define	NUPT_TOTAL	(NUPD_TOTAL*NPDEPG)	/* total PT pages */

#define	NDMPML4E	1		/* number of dmap PML4 slots */
#define	NDMPDPE		NPTEPG		/* number of dmap PDPE slots */

/*
 * The *PML4I values control the layout of virtual memory
 */
#define	PML4PML4I	NUPDP_TOTAL	/* Index of recursive pml4 mapping */
#define NUPML4E		NUPDP_TOTAL	/* for vmparam.h */



#ifndef LOCORE

#ifndef _SYS_TYPES_H_
#include <sys/types.h>
#endif
#ifndef _SYS_QUEUE_H_
#include <sys/queue.h>
#endif
#ifndef _SYS_SPINLOCK_H_
#include <sys/spinlock.h>
#endif
#ifndef _SYS_THREAD_H_
#include <sys/thread.h>
#endif
#ifndef _SYS_VKERNEL_H_
#include <sys/vkernel.h>
#endif
#ifndef _MACHINE_TYPES_H_
#include <machine/types.h>
#endif
#ifndef _MACHINE_PARAM_H_
#include <machine/param.h>
#endif

#ifdef _KERNEL

/*
 * XXX
 */
#define	vtophys(va)	pmap_kextract(((vm_offset_t)(va)))
#define	vtophys_pte(va)	((pt_entry_t)pmap_kextract(((vm_offset_t)(va))))

#endif

#define	pte_load_clear(pte)	atomic_readandclear_long(pte)

/*
 * Pmap stuff
 */
struct pv_entry;
struct vm_page;
struct vm_object;

struct pv_entry_rb_tree;
RB_PROTOTYPE2(pv_entry_rb_tree, pv_entry, pv_entry,
	      pv_entry_compare, vm_offset_t);

struct md_page {
	int pv_list_count;
	TAILQ_HEAD(,pv_entry)	pv_list;
};

struct md_object {
};

/*
 * Each machine dependent implementation is expected to
 * keep certain statistics.  They may do this anyway they
 * so choose, but are expected to return the statistics
 * in the following structure.
 */
struct pmap_statistics {
	long resident_count;    /* # of pages mapped (total) */
	long wired_count;       /* # of pages wired */
};
typedef struct pmap_statistics *pmap_statistics_t;

struct pmap {
	pml4_entry_t		*pm_pml4;	/* KVA of level 4 page table */
	struct vm_page		*pm_pdirm;	/* VM page for pg directory */
	vpte_t			pm_pdirpte;	/* pte mapping phys page */
	struct vm_object	*pm_pteobj;	/* Container for pte's */
	TAILQ_ENTRY(pmap)	pm_pmnode;	/* list of pmaps */
	RB_HEAD(pv_entry_rb_tree, pv_entry) pm_pvroot;
	int			pm_count;	/* reference count */
	cpulock_t		pm_active_lock; /* interlock */
	cpumask_t		pm_active;	/* active on cpus */
	vm_pindex_t		pm_pdindex;	/* page dir page in obj */
	struct pmap_statistics	pm_stats;	/* pmap statistics */
	struct	vm_page		*pm_ptphint;	/* pmap ptp hint */
	int			pm_generation;	/* detect pvlist deletions */
	struct spinlock		pm_spin;
};

#define pmap_resident_count(pmap) ((pmap)->pm_stats.resident_count)
#define pmap_resident_tlnw_count(pmap) ((pmap)->pm_stats.resident_count - \
					(pmap)->pm_stats.wired_count)

typedef struct pmap	*pmap_t;

#ifdef _KERNEL
extern struct pmap	kernel_pmap;
#endif

/*
 * For each vm_page_t, there is a list of all currently valid virtual
 * mappings of that page.  An entry is a pv_entry_t, the list is pv_table.
 */
typedef struct pv_entry {
	pmap_t		pv_pmap;	/* pmap where mapping lies */
	vm_offset_t	pv_va;		/* virtual address for mapping */
	TAILQ_ENTRY(pv_entry)	pv_list;
	RB_ENTRY(pv_entry)	pv_entry;
	struct vm_page	*pv_ptem;	/* VM page for pte */
} *pv_entry_t;

#ifdef	_KERNEL

extern caddr_t	CADDR1;
extern pt_entry_t *CMAP1;
extern char *ptvmmap;		/* poor name! */
extern vm_offset_t clean_sva;
extern vm_offset_t clean_eva;

#ifndef __VM_PAGE_T_DEFINED__
#define __VM_PAGE_T_DEFINED__
typedef struct vm_page *vm_page_t;
#endif
#ifndef __VM_MEMATTR_T_DEFINED__
#define __VM_MEMATTR_T_DEFINED__
typedef char vm_memattr_t;
#endif

void	pmap_bootstrap(vm_paddr_t *, int64_t);
void	*pmap_mapdev (vm_paddr_t, vm_size_t);
void	pmap_page_set_memattr(vm_page_t m, vm_memattr_t ma);
void	pmap_unmapdev (vm_offset_t, vm_size_t);
void	pmap_release(struct pmap *pmap);
void	pmap_interlock_wait (struct vmspace *);

struct vm_page *pmap_use_pt (pmap_t, vm_offset_t);

static __inline int
pmap_emulate_ad_bits(pmap_t pmap) {
	return 0;
}

#endif /* _KERNEL */

#endif /* !LOCORE */

#endif /* !_MACHINE_PMAP_H_ */
