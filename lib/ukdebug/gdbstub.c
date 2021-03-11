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

#include <stddef.h>
#include <string.h>
#include <uk/print.h>
#include <uk/assert.h>
#include <sys/types.h>
#include <uk/gdbstub.h>
#include <uk/plat/console.h>
#include <uk/plat/gdbstub.h>

static int gdb_attached = 0;
static char gdb_packet[4096];

int gdb_main_loop(struct gdb_dbgstate *dbgstate);

static char gdb_checksum(const char *buf, size_t len)
{
	char c = 0;

	while (len--) {
		c += *buf++;
	}

	return c;
}

static void gdb_bin2hex(char *hex, size_t hex_len, char *bin, size_t bin_len)
{
	static const char map_bin2hex[] = "0123456789abcdef";

	UK_ASSERT(hex_len >= bin_len * 2);

	while (bin_len--) {
		*hex++ = map_bin2hex[(*bin & 0xf0) >> 4];
		*hex++ = map_bin2hex[(*bin & 0x0f) >> 0];

		bin++;
	}
}

static int gdb_hex2int(char hex)
{
	if ((hex >= '0') && (hex <= '9')) {
		return hex - '0';
	} else if ((hex >= 'a') && (hex <= 'f')) {
		return hex - 'a' + 0xa;
	} else if ((hex >= 'A') && (hex <= 'F')) {
		return hex - 'A' + 0xa;
	}
}

static void gdb_hex2bin(char *bin, size_t bin_len, char *hex, size_t hex_len)
{
	UK_ASSERT(bin_len >= hex_len / 2);
	UK_ASSERT(hex_len % 2 == 0);

	while (hex_len--) {
		*bin  = gdb_hex2int(*hex++) << 4;
		*bin |= gdb_hex2int(*hex++);

		bin++;
	}
}

static void gdb_send(const char *buf, size_t len)
{
	while (len--) {
		ukplat_gdb_putc(*buf++);
	}
}

static void gdb_recv(char *buf, size_t len)
{
	while (len--) {
		*buf++ = ukplat_gdb_getc();
	}
}

static void gdb_send_ack(void)
{
	ukplat_gdb_putc('+');
}

static void gdb_send_nack(void)
{
	ukplat_gdb_putc('-');
}

static int gdb_recv_ack(void)
{
	return (ukplat_gdb_getc() == '+');
}

static ssize_t gdb_send_packet(const char *buf, size_t len)
{
	char hex[2];
	char chksum = gdb_checksum(buf, len);
	int retries = 0;

	gdb_bin2hex(hex, sizeof(hex), &chksum, 1);

	/* GDB packet format: $<DATA>#<CC>
	 * where CC is the GDB packet checksum
	 */
	do {
		if (retries++ >= 5) {
			return -1;
		}

		ukplat_gdb_putc('$');
		gdb_send(buf, len);
		ukplat_gdb_putc('#');
		gdb_send(hex, sizeof(hex));
	} while (!gdb_recv_ack());

	return len;
}

static ssize_t gdb_recv_packet(char *buf, size_t len)
{
	char c, *p = buf;
	size_t n = 0;
	int retries = 0;

	while ((c = ukplat_gdb_getc()) != '$')
		;

	while (n < len) {
		c = ukplat_gdb_getc();
		if (c == '#') {
			char hex[2];
			char chksum = 0;

			gdb_recv(hex, sizeof(hex));
			gdb_hex2bin(&chksum, 1, hex, sizeof(hex));

			if (chksum != gdb_checksum(buf, n)) {
				uk_pr_debug("CHKSUM broken: %x %x %c%c\n",
					chksum, gdb_checksum(buf, n), hex[0], hex[1]);
				gdb_send_nack();
				continue;
			}

			gdb_send_ack();

			/* Null-terminate the data */
			*p = 0;
			return n;
		} else if (c == '$') {
			if (retries++ > 5) {
				break;
			}

			/* We received a packet start character and maybe
			 * missed some characters on the way.
			 * Start all over again.
			 */
			p = buf;
			n = 0;
		} else {
			*p++ = c;
			n++;
		}
	}

	/* We ran out of space */
	gdb_send_nack();
	return -1;
}

static ssize_t gdb_get_cmd_len(const char *buf)
{
	const char *p = buf;

	while (*p++) {
		if (*p == ':') {
			break;
		}
	}

	return p - buf - 1;
}

static void gdb_attach(void)
{
	UK_ASSERT(!gdb_attached);

	gdb_attached = 1;
}

void uk_gdb_init(void)
{
	ukplat_gdb_init();
}

int uk_gdb_trap(struct gdb_dbgstate *dbgstate)
{
	if (!gdb_attached) {
		gdb_attach();
	}

	gdb_main_loop(dbgstate);

	return 0;
}

int gdb_handle_qsupported(const char *buf, struct gdb_dbgstate *dbgstate)
{
	gdb_send_packet(NULL, 0);
	return 0;
}

typedef int (*gdb_cmd_handler_func)(const char *buf,
		struct gdb_dbgstate *dbgstate);

struct gdb_cmd_table_entry {
	gdb_cmd_handler_func f;
	const char *cmd;
} gdb_cmd_table[] = {
	{ gdb_handle_qsupported, "qSupported" }
};

#define NUM_GDB_CMDS (sizeof(gdb_cmd_table) / \
		sizeof(struct gdb_cmd_table_entry))

int gdb_main_loop(struct gdb_dbgstate *dbgstate)
{
	ssize_t r;
	int handled;

	while (1) {
		r = gdb_recv_packet(gdb_packet, sizeof(gdb_packet));
		if (r < 0) {
			return -1;
		}

		r = gdb_get_cmd_len(gdb_packet);

		handled = 0;
		for (size_t i = 0; i < NUM_GDB_CMDS; i++) {
			if (strncmp(gdb_packet, gdb_cmd_table[i].cmd, r) == 0) {
				gdb_cmd_table[i].f(gdb_packet, dbgstate);
			}
		}

		if (!handled) {
			gdb_send_packet(NULL, 0);
		}
	}
}