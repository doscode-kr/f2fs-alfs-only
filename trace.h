/*
 * f2fs IO tracer
 *
 * Copyright (c) 2014 Motorola Mobility
 * Copyright (c) 2014 Jaegeuk Kim <jaegeuk@kernel.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#ifndef __F2FS_TRACE_H__
#define __F2FS_TRACE_H__

#ifdef CONFIG_F2FS_IO_TRACE
#include <trace/events/f2fs.h>

enum file_type {
	__NORMAL_FILE,
	__DIR_FILE,
	__NODE_FILE,
	__META_FILE,
	__ATOMIC_FILE,
	__VOLATILE_FILE,
	__MISC_FILE,
};

struct last_io_info {
	int major, minor;
	pid_t pid;
	enum file_type type;
	struct f2fs_io_info fio;
	block_t len;
};

extern void f2fs_trace_pid(struct page *);
extern void f2fs_trace_ios(struct f2fs_io_info *, int);
extern void f2fs_build_trace_ios(void);
extern void f2fs_destroy_trace_ios(void);
extern void f2fs_alfs_trace(const char *func_name, int op, int type);
extern void f2fs_alfs_trace_l2p_discard(int blk_addr, int num_section);
extern void f2fs_alfs_trace_locate_dt_sgmt_discard(int blk_addr, int num_section, int length);

#else
#define f2fs_trace_pid(p)
#define f2fs_trace_ios(i, n)
#define f2fs_build_trace_ios()
#define f2fs_destroy_trace_ios()
#define f2fs_alfs_trace(func_name, op, type)
#define f2fs_alfs_trace_l2p_discard(blk_addr, num_section)
#define f2fs_alfs_trace_locate_dt_sgmt_discard(blk_addr, num_section, length)
#endif
#endif /* __F2FS_TRACE_H__ */
