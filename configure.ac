AC_INIT(pm, 0.1)

# Check for support of more precise mtime
# BSD: st_mtimespec
AC_CHECK_MEMBER([struct stat.st_mtimespec], [AC_DEFINE(HAVE_STRUCT_STAT_ST_MTIMESPEC)], [],
                [#include <sys/stat.h>])
# Linux: st_mtim
AC_CHECK_MEMBER([struct stat.st_mtim], [AC_DEFINE(HAVE_STRUCT_STAT_ST_MTIM)], [],
                [#include <sys/stat.h>])

cp confdefs.h config.h
