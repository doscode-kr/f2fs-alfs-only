# Makefile for F2FS (for ALFS extension)
#
CONFIG_MODULE_SIG=n
TARGET = f2fs
SRCS = $(wildcard *.c)
OBJS = f2fs-y
MDIR = drivers/misc
CURRENT = $(shell uname -r)
KDIR = /lib/modules/$(CURRENT)/build
PWD = $(shell pwd)
DEST = /lib/modules/$(CURRENT)/kernel/$(MDIR)

#CONFIG_F2FS_STAT_FS = y
CONFIG_F2FS_FS_XATTR = y
#CONFIG_F2FS_FS_POSIX_ACL = y
CONFIG_F2FS_IO_TRACE = y

# for ALFS extension
CONFIG_ALFS_EXT = y
CONFIG_ALFS_PMU = n

EXTRA_CFLAGS += -DALFS_NO_SSR
EXTRA_CFLAGS += -DALFS_SNAPSHOT
EXTRA_CFLAGS += -DALFS_META_LOGGING
EXTRA_CFLAGS += -DALFS_TRIM
#EXTRA_CFLAGS += -DALFS_PMU
EXTRA_CFLAGS += -DCONFIG_F2FS_FS_XATTR
#EXTRA_CFLAGS += -DCONFIG_F2FS_STAT_FS
EXTRA_CFLAGS += -DCONFIG_F2FS_IO_TRACE
#EXTRA_CFLAGS += -DCONFIG_F2FS_FS_POSIX_ACL
EXTRA_CFLAGS += -DCONFIG_F2FS_FS_SECURITY


# improvement for direct I/O taken from
# - https://www.mail-archive.com/linux-f2fs-devel@lists.sourceforge.net/msg00727.html
# - http://lkml.iu.edu/hypermail/linux/kernel/1311.2/01080.html

obj-m		:= $(TARGET).o
module-objs	:= $(OBJS)
ccflags-y	+= -Wno-unused-function

default:
	$(MAKE) -C $(KDIR) SUBDIRS=$(PWD) modules

clean:
	-rm -rf *.o .*.cmd .*.flags *.mod.c *.c~ *.h~ .tmp_versions modules.order Module.symvers f2fs.ko
-include $(KDIR)/Rules.make

obj-$(CONFIG_F2FS_FS) += f2fs.o

f2fs-y		:= dir.o file.o inode.o namei.o hash.o super.o inline.o
f2fs-y		+= checkpoint.o gc.o data.o node.o segment.o recovery.o
f2fs-y		+= shrinker.o extent_cache.o
f2fs-$(CONFIG_ALFS_PMU)		+= alfs_pmu.o
f2fs-$(CONFIG_ALFS_EXT)		+= alfs_ext.o
f2fs-$(CONFIG_F2FS_STAT_FS) += debug.o
f2fs-$(CONFIG_F2FS_FS_XATTR) += xattr.o
f2fs-$(CONFIG_F2FS_FS_POSIX_ACL) += acl.o
f2fs-$(CONFIG_F2FS_IO_TRACE) += trace.o

