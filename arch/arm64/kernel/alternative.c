/*
 * alternative runtime patching
 * inspired by the x86 version
 *
 * Copyright (C) 2014 ARM Ltd.
 * Copyright (C) 2021 XiaoMi, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#define pr_fmt(fmt) "alternatives: " fmt

#include <linux/init.h>
#include <linux/cpu.h>
#include <asm/cacheflush.h>
#include <asm/alternative.h>
#include <asm/cpufeature.h>
#include <asm/insn.h>
#include <asm/sections.h>
#include <linux/stop_machine.h>

#define __ALT_PTR(a,f)		((void *)&(a)->f + (a)->f)
#define ALT_ORIG_PTR(a)		__ALT_PTR(a, orig_offset)
#define ALT_REPL_PTR(a)		__ALT_PTR(a, alt_offset)

int alternatives_applied;

struct alt_region {
	struct alt_instr *begin;
	struct alt_instr *end;
};

/*
 * Check if the target PC is within an alternative block.
 */
static bool branch_insn_requires_update(struct alt_instr *alt, unsigned long pc)
{
	unsigned long replptr = (unsigned long)ALT_REPL_PTR(alt);
	return !(pc >= replptr && pc <= (replptr + alt->alt_len));
}

#define align_down(x, a)	((unsigned long)(x) & ~(((unsigned long)(a)) - 1))

static u32 get_alt_insn(struct alt_instr *alt, __le32 *insnptr, __le32 *altinsnptr)
{
	u32 insn;

	insn = le32_to_cpu(*altinsnptr);

	if (aarch64_insn_is_branch_imm(insn)) {
		s32 offset = aarch64_get_branch_offset(insn);
		unsigned long target;

		target = (unsigned long)altinsnptr + offset;

		/*
		 * If we're branching inside the alternate sequence,
		 * do not rewrite the instruction, as it is already
		 * correct. Otherwise, generate the new instruction.
		 */
		if (branch_insn_requires_update(alt, target)) {
			offset = target - (unsigned long)insnptr;
			insn = aarch64_set_branch_offset(insn, offset);
		}
	} else if (aarch64_insn_is_adrp(insn)) {
		s32 orig_offset, new_offset;
		unsigned long target;

		/*
		 * If we're replacing an adrp instruction, which uses PC-relative
		 * immediate addressing, adjust the offset to reflect the new
		 * PC. adrp operates on 4K aligned addresses.
		 */
		orig_offset  = aarch64_insn_adrp_get_offset(insn);
		target = align_down(altinsnptr, SZ_4K) + orig_offset;
		new_offset = target - align_down(insnptr, SZ_4K);
		insn = aarch64_insn_adrp_set_offset(insn, new_offset);
	} else if (aarch64_insn_uses_literal(insn)) {
		/*
		 * Disallow patching unhandled instructions using PC relative
		 * literal addresses
		 */
		BUG();
	}

	return insn;
}

static void patch_alternative(struct alt_instr *alt,
			      __le32 *origptr, __le32 *updptr, int nr_inst)
{
	__le32 *replptr;
	int i;

	replptr = ALT_REPL_PTR(alt);
	for (i = 0; i < nr_inst; i++) {
		u32 insn;

		insn = get_alt_insn(alt, origptr + i, replptr + i);
		updptr[i] = cpu_to_le32(insn);
	}
}

static void __nocfi __apply_alternatives(void *alt_region, bool use_linear_alias)
{
	struct alt_instr *alt;
	struct alt_region *region = alt_region;
	__le32 *origptr, *updptr;
	alternative_cb_t alt_cb;

	for (alt = region->begin; alt < region->end; alt++) {
		int nr_inst;

		/* Use ARM64_CB_PATCH as an unconditional patch */
		if (alt->cpufeature < ARM64_CB_PATCH &&
		    !cpus_have_cap(alt->cpufeature))
			continue;

		if (alt->cpufeature == ARM64_CB_PATCH)
			BUG_ON(alt->alt_len != 0);
		else
			BUG_ON(alt->alt_len != alt->orig_len);

		pr_info_once("patching kernel code\n");

		origptr = ALT_ORIG_PTR(alt);
		updptr = use_linear_alias ? lm_alias(origptr) : origptr;
		nr_inst = alt->orig_len / AARCH64_INSN_SIZE;

		if (alt->cpufeature < ARM64_CB_PATCH)
			alt_cb = patch_alternative;
		else
			alt_cb  = ALT_REPL_PTR(alt);

		alt_cb(alt, origptr, updptr, nr_inst);

		flush_icache_range((uintptr_t)origptr,
				   (uintptr_t)(origptr + nr_inst));
	}
}

/*
 * We might be patching the stop_machine state machine, so implement a
 * really simple polling protocol here.
 */
static int __apply_alternatives_multi_stop(void *unused)
{
	struct alt_region region = {
		.begin	= (struct alt_instr *)__alt_instructions,
		.end	= (struct alt_instr *)__alt_instructions_end,
	};

	/* We always have a CPU 0 at this point (__init) */
	if (smp_processor_id()) {
		while (!READ_ONCE(alternatives_applied))
			cpu_relax();
		isb();
	} else {
		BUG_ON(alternatives_applied);
		__apply_alternatives(&region, true);
		/* Barriers provided by the cache flushing */
		WRITE_ONCE(alternatives_applied, 1);
	}

	return 0;
}

void __init apply_alternatives_all(void)
{
	/* better not try code patching on a live SMP system */
	stop_machine(__apply_alternatives_multi_stop, NULL, cpu_online_mask);
}

void apply_alternatives(void *start, size_t length)
{
	struct alt_region region = {
		.begin	= start,
		.end	= start + length,
	};

	__apply_alternatives(&region, false);
}
