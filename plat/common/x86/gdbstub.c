/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Author(s): Marc Rittinghaus <marc.rittinghaus@kit.edu>
 *
 * Copyright (c) 2021, Karlsruhe Institute of Technology. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holder nor the names of its
 *    contributors may be used to endorse or promote products derived from
 *    this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <uk/print.h>
#include <uk/gdbstub.h>
#include <uk/plat/gdbstub.h>
#include <uk/assert.h>
#include <uk/essentials.h>

void ukplat_gdb_handle_trap(int trapnr, struct __regs *regs,
		unsigned long error_code __unused)
{
	struct gdb_dbgstate dbgstate = {0};

	dbgstate.regs[GDB_X86_REG_RAX] = regs->rax;
	dbgstate.regs[GDB_X86_REG_RBX] = regs->rbx;
	dbgstate.regs[GDB_X86_REG_RCX] = regs->rcx;
	dbgstate.regs[GDB_X86_REG_RDX] = regs->rdx;
	dbgstate.regs[GDB_X86_REG_RSI] = regs->rsi;
	dbgstate.regs[GDB_X86_REG_RDI] = regs->rdi;
	dbgstate.regs[GDB_X86_REG_RBP] = regs->rbp;
	dbgstate.regs[GDB_X86_REG_RSP] = regs->rsp;
	dbgstate.regs[GDB_X86_REG_R8] = regs->r8;
	dbgstate.regs[GDB_X86_REG_R9] = regs->r9;
	dbgstate.regs[GDB_X86_REG_R10] = regs->r10;
	dbgstate.regs[GDB_X86_REG_R11] = regs->r11;
	dbgstate.regs[GDB_X86_REG_R12] = regs->r12;
	dbgstate.regs[GDB_X86_REG_R13] = regs->r13;
	dbgstate.regs[GDB_X86_REG_R14] = regs->r14;
	dbgstate.regs[GDB_X86_REG_R15] = regs->r15;

	dbgstate.regs[GDB_X86_REG_RIP] = regs->rip;
	dbgstate.regs[GDB_X86_REG_EFLAGS] = regs->eflags;
	dbgstate.regs[GDB_X86_REG_CS] = regs->cs;
	dbgstate.regs[GDB_X86_REG_SS] = regs->ss;
	//dbgstate.regs[GDB_X86_REG_DS] = 0;
	//dbgstate.regs[GDB_X86_REG_ES] = 0;
	//dbgstate.regs[GDB_X86_REG_FS] = 0;
	//dbgstate.regs[GDB_X86_REG_GS] = 0;

	if ((trapnr == 1) || (trapnr == 3)) {
		dbgstate.signr = 5; // SIGTRAP
	} else {
		UK_ASSERT(0);
	}

	uk_pr_debug("DEBUG TRAP: %d, rip: 0x%lx\n", dbgstate.signr,
		dbgstate.regs[GDB_X86_REG_RIP]);

	int r = uk_gdb_trap(&dbgstate);
	if (r == 1) { /* Single step */
		dbgstate.regs[GDB_X86_REG_EFLAGS] |= 1 << 8;
	} else {
		dbgstate.regs[GDB_X86_REG_EFLAGS] &= ~(1 << 8);
	}

	regs->rax = dbgstate.regs[GDB_X86_REG_RAX];
	regs->rbx = dbgstate.regs[GDB_X86_REG_RBX];
	regs->rcx = dbgstate.regs[GDB_X86_REG_RCX];
	regs->rdx = dbgstate.regs[GDB_X86_REG_RDX];
	regs->rsi = dbgstate.regs[GDB_X86_REG_RSI];
	regs->rdi = dbgstate.regs[GDB_X86_REG_RDI];
	regs->rbp = dbgstate.regs[GDB_X86_REG_RBP];
	regs->rsp = dbgstate.regs[GDB_X86_REG_RSP];
	regs->r8 = dbgstate.regs[GDB_X86_REG_R8];
	regs->r9 = dbgstate.regs[GDB_X86_REG_R9];
	regs->r10 = dbgstate.regs[GDB_X86_REG_R10];
	regs->r11 = dbgstate.regs[GDB_X86_REG_R11];
	regs->r12 = dbgstate.regs[GDB_X86_REG_R12];
	regs->r13 = dbgstate.regs[GDB_X86_REG_R13];
	regs->r14 = dbgstate.regs[GDB_X86_REG_R14];
	regs->r15 = dbgstate.regs[GDB_X86_REG_R15];

	regs->rip = dbgstate.regs[GDB_X86_REG_RIP];
	regs->eflags = dbgstate.regs[GDB_X86_REG_EFLAGS];
	regs->cs = dbgstate.regs[GDB_X86_REG_CS];
	regs->ss = dbgstate.regs[GDB_X86_REG_SS];
	// dbgstate.regs[GDB_X86_REG_DS];
	// dbgstate.regs[GDB_X86_REG_ES];
	// dbgstate.regs[GDB_X86_REG_FS];
	// dbgstate.regs[GDB_X86_REG_GS];
}