#                                               -*- Autoconf -*-
# Process this file with autoconf to produce a configure script.

AC_INIT([AMon], [1.0], [amon@example.com])
AM_INIT_AUTOMAKE([-Wall -Werror -Wno-portability foreign])
AC_CONFIG_SRCDIR([config.h.in])
AC_CONFIG_HEADERS([config.h])
AC_CONFIG_MACRO_DIR([m4])

# Checks for programs.
: ${CXXFLAGS="-g"}
AC_PROG_CXX
: ${CFLAGS="-g"}
AC_PROG_CC
#AM_PROG_AR
AC_PROG_LIBTOOL
AC_PROG_RANLIB

AC_SUBST([AM_CXXFLAGS], ["-std=c++11 -Wall -Wno-parentheses -Wno-unused-local-typedefs -Wno-unused-variable -Wno-reorder"])
AC_SUBST([AM_CFLAGS], ["-Wall -Wno-parentheses -Wno-unused-local-typedefs -Wno-unused-variable"])
AC_SUBST([AM_CPPFLAGS], [])

# Checks for libraries.

# Checks for header files.
AC_CHECK_HEADERS([limits.h stdint.h stdlib.h string.h sys/ioctl.h sys/socket.h sys/time.h unistd.h])

# Checks for typedefs, structures, and compiler characteristics.
AC_HEADER_STDBOOL
AC_C_INLINE
AC_TYPE_INT16_T
AC_TYPE_INT32_T
AC_TYPE_INT64_T
AC_TYPE_SIZE_T
AC_TYPE_SSIZE_T
AC_TYPE_UINT16_T
AC_TYPE_UINT32_T
AC_TYPE_UINT64_T
AC_TYPE_UINT8_T
AC_CHECK_TYPES([ptrdiff_t])

# Checks for library functions.
AC_FUNC_ERROR_AT_LINE
AC_FUNC_MALLOC
AC_FUNC_REALLOC
AC_CHECK_FUNCS([gethostname memchr memmove select socket strrchr])

AC_CONFIG_FILES([Makefile src/Makefile])
AC_OUTPUT


