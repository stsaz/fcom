/** ff: stream buffer
2022, Simon Zolin
*/

/*
ffstream_realloc ffstream_free
ffstream_reset
ffstream_used
ffstream_view
ffstream_gather ffstream_gather_ref
ffstream_consume
*/

#pragma once
#include <ffbase/string.h>

typedef struct ffstream {
	char *ptr;
	ffuint r, w;
	ffuint cap, mask;
	ffstr ref;
} ffstream;

/** Grow the buffer
Return the actual buffer capacity;
 0 on error */
static inline ffsize ffstream_realloc(ffstream *s, ffsize newcap)
{
	newcap = ffint_align_power2(newcap);
	if (newcap <= s->cap)
		return s->cap;

	char *p;
	if (NULL == (p = (char*)ffmem_alloc(newcap)))
		return 0;

	if (s->ref.len == 0) {
		// copy used data from old buffer to new buffer
		ffuint used = s->w - s->r;
		ffuint i = s->r & s->mask;
		ffmem_copy(p, s->ptr + i, used);
		s->r = 0;
		s->w = used;
	}

	ffmem_free(s->ptr);
	s->ptr = p;
	s->cap = newcap;
	s->mask = newcap - 1;
	return s->cap;
}

static inline void ffstream_free(ffstream *s)
{
	ffmem_free(s->ptr);
	s->ptr = NULL;
}

static inline void ffstream_reset(ffstream *s)
{
	s->r = s->w = 0;
}

/** Get N of valid bytes */
static inline ffuint ffstream_used(ffstream *s)
{
	ffuint used = s->w - s->r;
	return used;
}

/** Get all available data */
static inline ffstr ffstream_view(ffstream *s)
{
	ffuint used = s->w - s->r;
	ffuint i = s->r & s->mask;
	ffstr view = FFSTR_INITN(s->ptr + i, used);
	if (s->ref.len != 0)
		view.ptr = s->ref.ptr + s->ref.len - used;
	return view;
}

/** Gather a contiguous region of at least `gather` bytes of data.
output: buffer view, valid until the next call to this function
Return N of input bytes consumed */
static inline ffuint ffstream_gather(ffstream *s, ffstr input, ffsize gather, ffstr *output)
{
	FF_ASSERT(gather <= s->cap);
	ffuint i, n = 0, used = s->w - s->r;

	if (used < gather) {
		// need to append data into our buffer

		i = s->r & s->mask;
		if (i + gather > s->cap) {
			// not enough space in tail: move tail bytes to front
			ffmem_move(s->ptr, s->ptr + i, used); // "...DD" -> "DD"
			s->r -= i;
			s->w -= i;
		}

		// going to append input data to tail as much as we can fit
		i = s->w & s->mask;
		ffuint unused_seq = s->cap - i; // "...DDU"
		n = unused_seq;

		// append input data to tail
		n = ffmin(n, input.len);
		ffmem_copy(s->ptr + i, input.ptr, n);
		s->w += n;

		used = s->w - s->r;
	}

	i = s->r & s->mask;
	ffstr_set(output, s->ptr + i, used);
	return n;
}

/** Gather a contiguous region of at least `gather` bytes of data with minimum data copying.
Reference (don't copy) input data when possible to minimize data copying.
However, the use-case of data searching with partial consuming becomes very slow
 because there will always be some data left in the buffer,
 which means that referencing the data becomes impossible,
 thus defeating the purpose of this function.
 TODO Implement _avpack_gather_header/_trailer()-like algorithm?
input: input data, must stay valid while 'output' is used
output: buffer view or input data view, valid until the next call to this function
Return N of input bytes referenced or consumed */
static inline ffuint ffstream_gather_ref(ffstream *s, ffstr input, ffsize gather, ffstr *output)
{
	FF_ASSERT(gather <= s->cap);
	ffuint i, n = 0, used = s->w - s->r;

	if (used < gather) {
		if (used == 0) {
			// reference and return the whole input buffer
			s->ref = input;
			s->w += input.len;
			*output = input;
			return input.len;
		}

		// need to append data into our buffer

		if (s->ref.len != 0) {
			// copy used data from the referenced buffer into our buffer
			ffmem_copy(s->ptr, s->ref.ptr + s->ref.len - used, used);
			s->ref.len = 0;
			s->r = 0;
			s->w = used;

			// going to append minimum input data to tail until we have `gather` bytes available
			n = gather - used;
			i = s->w;

		} else {
			i = s->r & s->mask;
			if (i + gather > s->cap) {
				// not enough space in tail: move tail bytes to front
				ffmem_move(s->ptr, s->ptr + i, used); // "...DD" -> "DD"
				s->r -= i;
				s->w -= i;
			}

			// going to append input data to tail as much as we can fit
			i = s->w & s->mask;
			ffuint unused_seq = s->cap - i; // "...DDU"
			n = unused_seq;
		}

		// append input data to tail
		n = ffmin(n, input.len);
		ffmem_copy(s->ptr + i, input.ptr, n);
		s->w += n;

		used = s->w - s->r;

	} else if (s->ref.len != 0) {
		// we have enough data in the referenced buffer
		ffstr_set(output, s->ref.ptr + s->ref.len - used, used);
		return 0;
	}

	i = s->r & s->mask;
	ffstr_set(output, s->ptr + i, used);
	return n;
}

/** Discard some data */
static inline void ffstream_consume(ffstream *s, ffsize n)
{
	ffuint used = s->w - s->r;
	(void)used;
	FF_ASSERT(used >= n);
	s->r += n;
}
