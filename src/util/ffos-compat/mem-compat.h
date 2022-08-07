/** Allocate N objects of type T. */
#define ffmem_allocT(N, T)  ((T*)ffmem_alloc((N) * sizeof(T)))

/** Allocate N objects of type T.  Zero the buffer. */
#define ffmem_callocT(N, T)  ((T*)ffmem_calloc(N, sizeof(T)))

/** Zero the object. */
#define ffmem_tzero(p)  memset(p, 0, sizeof(*(p)))

#define ffmem_safefree(p)  ffmem_free(p)

#define ffmem_safefree0(p)  FF_SAFECLOSE(p, NULL, ffmem_free)

#define ffmem_free0(p) \
do { \
	ffmem_free(p); \
	p = NULL; \
} while (0)
