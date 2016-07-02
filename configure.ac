AC_INIT(pm, 0.1)
AM_INIT_AUTOMAKE

# Linux test
AM_CONDITIONAL([LINUX], [test $(uname) = "Linux"])

# Check for support of more precise mtime
# BSD: st_mtimespec
AC_CHECK_MEMBER([struct stat.st_mtimespec], [AC_DEFINE(HAVE_STRUCT_STAT_ST_MTIMESPEC)], [],
                [#include <sys/stat.h>])
# Linux: st_mtim
AC_CHECK_MEMBER([struct stat.st_mtim], [AC_DEFINE(HAVE_STRUCT_STAT_ST_MTIM)], [],
                [#include <sys/stat.h>])

# Get and set PM_VERSION
AC_MSG_CHECKING([pm version])
pm_version="$(git describe --abbrev=7 2>/dev/null)"
test -z "$pm_version" && pm_version=0.2
AC_MSG_RESULT([$pm_version])
AC_DEFINE_UNQUOTED([PM_VERSION], ["$pm_version"])

AC_CONFIG_FILES([Makefile])
AC_OUTPUT

cp confdefs.h config.h
