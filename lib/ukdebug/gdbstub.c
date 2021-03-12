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

#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <uk/print.h>
#include <uk/assert.h>
#include <sys/types.h>
#include <uk/gdbstub.h>
#include <uk/plat/console.h>
#include <uk/plat/gdbstub.h>
#include <uk/essentials.h>

static int gdb_initialized = 0;
static char gdb_recv_buffer[2048];
static char gdb_send_buffer[2048];

#define GDB_PACKET_RETRIES 5

#define GDB_CHECK(expr) \
	do { \
		ssize_t r = (expr); \
		if (r < 0) { return (int)r; } \
	} while (0)

typedef int (*gdb_cmd_handler_func)(char *buf, struct gdb_dbgstate *dbgstate);

struct gdb_cmd_table_entry {
	gdb_cmd_handler_func f;
	const char *cmd;
};

extern const char __gdb_target_xml_start[];
extern const char __gdb_target_xml_end[];

static int gdb_main_loop(struct gdb_dbgstate *dbgstate);

static char gdb_checksum(const char *buf, size_t len)
{
	char c = 0;

	while (len--) {
		c += *buf++;
	}

	return c;
}

static char* gdb_byte2hex(char *hex, size_t hex_len, char b)
{
	static const char map_bin2hex[] = "0123456789abcdef";

	UK_ASSERT(hex_len >= 2);

	*hex++ = map_bin2hex[(b & 0xf0) >> 4];
	*hex++ = map_bin2hex[(b & 0x0f) >> 0];

	return hex;
}

static void gdb_bin2hex(char *hex, size_t hex_len,
	const char *bin, size_t bin_len)
{
	UK_ASSERT(hex_len >= bin_len * 2);

	while (bin_len--) {
		hex = gdb_byte2hex(hex, hex_len, *bin++);
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

	return 0;
}

static void gdb_hex2bin(char *bin, size_t bin_len,
	const char *hex, size_t hex_len)
{
	UK_ASSERT(bin_len >= hex_len / 2);
	UK_ASSERT(hex_len % 2 == 0);

	while (hex_len--) {
		*bin  = gdb_hex2int(*hex++) << 4;
		*bin |= gdb_hex2int(*hex++);

		bin++;
	}
}

static ssize_t gdb_send(const char *buf, size_t len)
{
	size_t l = len;

	while (l--) {
		GDB_CHECK(ukplat_gdb_putc(*buf++));
	}

	return len;
}

static ssize_t gdb_recv(char *buf, size_t len)
{
	size_t l = len;
	int r;

	while (l--) {
		r = ukplat_gdb_getc();
		if (r < 0) {
			return r;
		}

		*buf++ = (char)r;
	}

	return len;
}

static int gdb_send_ack(void)
{
	return ukplat_gdb_putc('+');
}

static int gdb_send_nack(void)
{
	return ukplat_gdb_putc('-');
}

static int gdb_recv_ack(void)
{
	int r = ukplat_gdb_getc();

	return (r < 0) ? r : ((char)r == '+');
}

static ssize_t gdb_send_packet(const char *buf, size_t len)
{
	char hex[2];
	char chksum = gdb_checksum(buf, len);
	int r, retries = 0;

	gdb_bin2hex(hex, sizeof(hex), &chksum, 1);

	/* GDB packet format: $<DATA>#<CC>
	 * where CC is the GDB packet checksum
	 */
	do {
		if (retries++ > GDB_PACKET_RETRIES) {
			return -1;
		}

		GDB_CHECK(ukplat_gdb_putc('$'));
		GDB_CHECK(gdb_send(buf, len));
		GDB_CHECK(ukplat_gdb_putc('#'));
		GDB_CHECK(gdb_send(hex, sizeof(hex)));
	} while ((r = gdb_recv_ack()) == 0);

	return (r == 1) ? (ssize_t)len : r;
}

static ssize_t gdb_send_empty_packet(void)
{
	return gdb_send_packet(NULL, 0);
}

static ssize_t gdb_send_signal_packet(int signr)
{
	char buf[3];

	buf[0] = 'S';
	gdb_byte2hex(buf + 1, sizeof(buf) - 1, signr);

	return gdb_send_packet(buf, sizeof(buf));
}

static ssize_t gdb_recv_packet(char *buf, size_t len)
{
	int c, retries = -1;
	char *p = NULL;
	size_t n = 0;

	while (n < len) {
		c = ukplat_gdb_getc();
		if (c < 0) {
			return c;
		} else if (c == '#') {
			char hex[2];
			char chksum = 0;

			GDB_CHECK(gdb_recv(hex, sizeof(hex)));
			gdb_hex2bin(&chksum, 1, hex, sizeof(hex));

			if (chksum != gdb_checksum(buf, n)) {
				GDB_CHECK(gdb_send_nack());
				continue;
			}

			GDB_CHECK(gdb_send_ack());

			/* Null-terminate the data */
			*p = 0;
			return n;
		} else if (c == '$') {
			if (retries++ > GDB_PACKET_RETRIES) {
				break;
			}

			/* We received a packet start character and maybe
			 * missed some characters on the way.
			 * Start all over again.
			 */
			p = buf;
			n = 0;
		} else if (p != NULL) {
			*p++ = c;
			n++;
		}
	}

	/* We ran out of space */
	return -ENOMEM;
}

int uk_gdb_init(void)
{
	GDB_CHECK(ukplat_gdb_init());

	gdb_initialized = 1;
	return 0;
}

int uk_gdb_trap(struct gdb_dbgstate *dbgstate)
{
	if (!gdb_initialized) {
		return GDB_DBG_CONT;
	}

	return gdb_main_loop(dbgstate);
}

/* ? */
static int gdb_handle_stop_reason(char *buf __unused,
		struct gdb_dbgstate *dbgstate)
{
	ssize_t r = gdb_send_signal_packet(dbgstate->signr);
	return (r < 0) ? (int)r : 0;
}

/* c */
static int gdb_handle_continue(char *buf __unused,
		struct gdb_dbgstate *dbgstate __unused)
{
	return GDB_DBG_CONT;
}

/* s */
static int gdb_handle_step(char *buf __unused,
		struct gdb_dbgstate *dbgstate __unused)
{
	return GDB_DBG_STEP;
}

/* qSupported [:gdbfeature [;gdbfeature]... ] */
static int gdb_handle_qsupported(char *buf __unused,
		struct gdb_dbgstate *dbgstate __unused)
{
	const char *supported = "qXfer:features:read+";
	ssize_t r = gdb_send_packet(supported, strlen(supported));
	return (r < 0) ? (int)r : 0;
}

/* qXfer:features:read:annex:offset,length */
static int gdb_handle_qXfer(char *buf,
		struct gdb_dbgstate *dbgstate __unused)
{
	unsigned long offset, length;
	size_t rem_len, xml_len = __gdb_target_xml_end - __gdb_target_xml_start;
	ssize_t r;

	if (strncmp(buf, "features:read:target.xml:", 25)) {
		return -ENOTSUP;
	}

	buf += 25;

	offset = strtol(buf, &buf, 16);
	if (*buf++ != ',') {
		return -EINVAL;
	}
	length = strtol(buf, &buf, 16);
	if (offset >= xml_len) {
		r = gdb_send_packet("l", 1);
	} else {
		length = MIN(length, sizeof(gdb_send_buffer) - 1);
		rem_len = xml_len - offset;
		if (rem_len <= length) {
			length = rem_len;
			gdb_send_buffer[0] = 'l';
		} else {
			gdb_send_buffer[0] = 'm';
		}

		/* gdb_bin2hex(gdb_send_buffer + 1, sizeof(gdb_send_buffer) - 1,
			__gdb_target_xml_start + offset, length);

		r = gdb_send_packet(gdb_send_buffer, length * 2 + 1);*/

		memcpy(gdb_send_buffer + 1, __gdb_target_xml_start + offset,
			length);

		r = gdb_send_packet(gdb_send_buffer, length + 1);
	}

	return (r < 0) ? (int)r : 0;
}

static struct gdb_cmd_table_entry gdb_q_cmd_table[] = {
	{ gdb_handle_qsupported, "Supported" },
	{ gdb_handle_qXfer, "Xfer"}
};

#define NUM_GDB_Q_CMDS (sizeof(gdb_q_cmd_table) / \
		sizeof(struct gdb_cmd_table_entry))

int gdb_handle_q_cmd(char *buf, struct gdb_dbgstate *dbgstate)
{
	const char *p = strchr(buf, ':');
	size_t i, l = (p) ? (size_t)(p - buf) : strlen(buf);

	for (i = 0; i < NUM_GDB_Q_CMDS; i++) {
		/* TODO: compare length */
		if (strncmp(buf, gdb_q_cmd_table[i].cmd, l)) {
			continue;
		}

		return gdb_q_cmd_table[i].f(buf + l + 1, dbgstate);
	}

	return (int)gdb_send_empty_packet(); /* returns 0 on succcess */
}

static struct gdb_cmd_table_entry gdb_cmd_table[] = {
	{ gdb_handle_stop_reason, "?" },
	{ gdb_handle_continue, "c" },
	{ gdb_handle_step, "s" },
	{ gdb_handle_q_cmd, "q" }
};

#define NUM_GDB_CMDS (sizeof(gdb_cmd_table) / \
		sizeof(struct gdb_cmd_table_entry))

static int gdb_main_loop(struct gdb_dbgstate *dbgstate)
{
	ssize_t r;
	size_t i, l;

	if ((r = gdb_send_signal_packet(dbgstate->signr)) < 0) {
		return (int)r;
	}

	do {
		r = gdb_recv_packet(gdb_recv_buffer, sizeof(gdb_recv_buffer));
		if (r < 0) {
			break;
		} else if (r == 0) {
			/* We received an empty packet */
			continue;
		}

		for (i = 0; i < NUM_GDB_CMDS; i++) {
			l = strlen(gdb_cmd_table[i].cmd);
			/* TODO: compare length */
			if (strncmp(gdb_recv_buffer, gdb_cmd_table[i].cmd, l)) {
				continue;
			}

			r = gdb_cmd_table[i].f(gdb_recv_buffer + l, dbgstate);
			break;
		}

		if (i == NUM_GDB_CMDS) {
			r = gdb_send_empty_packet(); /* returns 0 on succcess */
		}
	} while (r == 0);

	return (int)r;
}