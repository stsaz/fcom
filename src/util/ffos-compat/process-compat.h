
#ifdef FF_WIN

#else

#include <sys/stat.h>
#include <sys/fcntl.h>

#endif

#include <FFOS/perf.h>
#include <FFOS/dylib.h>
#include <FFOS/environ.h>
#include <FFOS/sysconf.h>

#define ffenv  int
#define ffenv_destroy(a)
#define fflang_info  ffenv_locale
#define ffsc_init  ffsysconf_init
#define ffsc_get  ffsysconf_get
