/* src/queue_config.h.  Generated from queue_config.h.in by configure.  */
/* src/queue_config.h.in.  Generated from configure.in by autoheader.  */

/* do not sync at checkpoints */
/* #undef FDATASYNC_SKIP */

/* use fcntl for fdatasysnc */
/* #undef FDATASYNC_USE_FCNTL */

/* use fsync instead of fdatasync */
/* #undef FDATASYNC_USE_FSYNC */

/* Define to 1 if you have the <dlfcn.h> header file. */
#define HAVE_DLFCN_H 1

/* Define to 1 if you have the <inttypes.h> header file. */
#define HAVE_INTTYPES_H 1

/* Define to 1 if you have the <limits.h> header file. */
#define HAVE_LIMITS_H 1

/* Define to 1 if you have the `lseek64' function. */
#define HAVE_LSEEK64 1

/* Define to 1 if you have the <memory.h> header file. */
#define HAVE_MEMORY_H 1

/* use pthread_mutex_timedlock */
#define HAVE_PTHREAD_MUTEX_TIMEDLOCK 1

/* Define to 1 if you have the <stdint.h> header file. */
#define HAVE_STDINT_H 1

/* Define to 1 if you have the <stdlib.h> header file. */
#define HAVE_STDLIB_H 1

/* Define to 1 if you have the <strings.h> header file. */
#define HAVE_STRINGS_H 1

/* Define to 1 if you have the <string.h> header file. */
#define HAVE_STRING_H 1

/* Define to 1 if you have the <syslimits.h> header file. */
/* #undef HAVE_SYSLIMITS_H */

/* Define to 1 if you have the <sys/stat.h> header file. */
#define HAVE_SYS_STAT_H 1

/* Define to 1 if you have the <sys/types.h> header file. */
#define HAVE_SYS_TYPES_H 1

/* Define to 1 if you have the <unistd.h> header file. */
#define HAVE_UNISTD_H 1

/* Define to the sub-directory in which libtool stores uninstalled libraries.
   */
#define LT_OBJDIR ".libs/"

/* Source directory for MySQL */
#define MYSQL_SRC 1

/* Define to 1 if your C compiler doesn't accept -c and -o together. */
/* #undef NO_MINUS_C_MINUS_O */

/* Name of package */
/*
#define PACKAGE "q4m"
*/

/* Define to the address where bug reports for this package should be sent. */
#define PACKAGE_BUGREPORT ""

/* Define to the full name of this package. */
/*
#define PACKAGE_NAME ""
*/

/* Define to the full name and version of this package. */
/*
#define PACKAGE_STRING ""
*/

/* Define to the one symbol short name of this package. */
/*
#define PACKAGE_TARNAME ""
*/

/* Define to the home page for this package. */
#define PACKAGE_URL ""

/* Define to the version of this package. */
/*
#define PACKAGE_VERSION ""
*/

/* use msync for row deletions */
#define Q4M_DELETE_METHOD Q4M_DELETE_SERIAL_PWRITE

/* use mmap for reading data */
#define Q4M_USE_MMAP 1

/* do not serialize pwrite */
#define Q4M_USE_MT_PWRITE 1

/* use pthread_cond_timedwait_relative_np */
/* #undef Q4M_USE_RELATIVE_TIMEDWAIT */

/* hexadecimal version no */
#define Q4M_VERSION_HEX 0x0009

/* The size of `int*', as computed by sizeof. */
#define SIZEOF_INTP 8

/* Define to 1 if you have the ANSI C header files. */
#define STDC_HEADERS 1

/* Version number of package */
/*
#define VERSION "0.9.5"
*/

/* Define to empty if `const' does not conform to ANSI C. */
/* #undef const */

/* use pow instead of powl */
#define powl pow

/* Define to `unsigned int' if <sys/types.h> does not define. */
/* #undef size_t */
