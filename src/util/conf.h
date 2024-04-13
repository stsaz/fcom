#pragma once
#include <ffbase/vector.h>

typedef struct _fcom_conf {
	ffuint state, nextstate;
	ffvec buf; // char[]: buffer for partial input data
	ffuint ctxs; // contexts number
	char esc_char;
	ffuint clear :1;
	ffuint nextval :1;

// public:
	ffuint line, linechar; // line & character number
	ffstr val; // key or value
} _fcom_conf;

enum FFCONF_E {
	FFCONF_ESYS = 1,
	FFCONF_EESC,
	FFCONF_ESTR,
	FFCONF_ECTX,
	FFCONF_EBADVAL,
	FFCONF_EINCOMPLETE,
	FFCONF_ESCHEME,
};

enum {
	FFCONF_RMORE,
	FFCONF_RKEY, // KEY val
	FFCONF_RVAL, // key VAL
	FFCONF_RVAL_NEXT, // key val1 VAL2...
	FFCONF_ROBJ_OPEN, // {
	FFCONF_ROBJ_CLOSE, // }
};

/** Get or copy string value into a user's container */
static inline int ffconf_strval_acquire(_fcom_conf *c, ffstr *dst)
{
	if (c->val.len == 0) {
		ffstr_null(dst);
		return 0;
	}
	if (c->buf.cap != 0) {
		FF_ASSERT(c->val.ptr == c->buf.ptr);
		*dst = c->val;
		ffvec_free(&c->buf);
		return 0;
	}
	if (NULL == ffstr_dup2(dst, &c->val))
		return -1;
	return 0;
}
