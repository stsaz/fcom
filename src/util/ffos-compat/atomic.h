/** Atomic operations.
Copyright (c) 2013 Simon Zolin
*/

#pragma once

#include "types.h"
#include <ffbase/lock.h>
#include <ffbase/atomic.h>

typedef struct { uint val; } ffatomic32;

#define ffatom_fence_rel()  ffcpu_fence_release()
#define ffatom_fence_acq()  ffcpu_fence_acquire()

/** Set new value. */
#define ffatom_set(a, set)  FF_WRITEONCE((a)->val, set)

/** Get value. */
#define ffatom_get(a)  FF_READONCE((a)->val)

/** Add integer and return new value. */
// static FFINL size_t ffatom_addret(ffatomic *a, size_t add)
// {
// 	return ffatom_fetchadd(a, add) + add;
// }

// static FFINL uint ffatom32_addret(ffatomic32 *a, uint add)
// {
// 	return ffatom32_fetchadd(a, add) + add;
// }

#define ffatom_inc(a)  ffint_fetch_add(&(a)->val, 1)

/** Increment and return new value. */
#define ffatom_incret(a)  (ffint_fetch_add(&(a)->val, 1) + 1)

/** Decrement and return new value. */
#define ffatom_decret(a)  (ffint_fetch_add(&(a)->val, -1) - 1)

static FFINL int ffatom_cmpset(ffatomic *a, size_t old, size_t newval)
{
	return (old == ffatomic_cmpxchg(a, old, newval));
}

static FFINL void ffatom32_inc(ffatomic32 *a)
{
	ffint_fetch_add(&a->val, 1);
}

static FFINL void ffatom32_dec(ffatomic32 *a)
{
	ffint_fetch_add(&a->val, -1);
}

static FFINL ffuint ffatom32_decret(ffatomic32 *a)
{
	return ffint_fetch_add(&a->val, -1) - 1;
}

#define fflk_init  fflock_init
#define fflk_lock  fflock_lock
#define fflk_trylock  fflock_trylock
#define fflk_unlock  fflock_unlock
