## Makefile.am

AM_CPPFLAGS = ${libuuid_CFLAGS} ${libblkid_CFLAGS} -I$(top_srcdir)/include
AM_CFLAGS = -Wall -DWITH_BLKDISCARD -Wl,-rpath="/usr/local/lib"
sbin_PROGRAMS = mkfs.f2fs
noinst_HEADERS = f2fs_format_utils.h
include_HEADERS = $(top_srcdir)/include/f2fs_fs.h
mkfs_f2fs_SOURCES = f2fs_format_main.c f2fs_format.c f2fs_format_utils.c
mkfs_f2fs_LDADD = ${libuuid_LIBS} ${libblkid_LIBS} $(top_builddir)/lib/libf2fs.la 
mkfs_f2fs_LDFLAGS = -fopenmp -llightnvm

#生成的动态库，然后指定动态库依赖的源文件
lib_LTLIBRARIES = libf2fs_format.la
libf2fs_format_la_SOURCES = f2fs_format_main.c f2fs_format.c f2fs_format_utils.c
libf2fs_format_la_CFLAGS = -DWITH_BLKDISCARD -Wl,-rpath="/usr/local/lib"
libf2fs_format_la_CPPFLAGS = -I$(top_srcdir)/include
libf2fs_format_la_LDFLAGS = -luuid -fopenmp -llightnvm -L$(top_builddir)/lib -lf2fs \
	-version-info $(FMT_CURRENT):$(FMT_REVISION):$(FMT_AGE) 
#libf2fs_format_la_LIBADD = -fopenmp -llightnvm

