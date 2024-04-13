/** ffconf--_fcom_conf bridge
2022, Simon Zolin
*/

#pragma once
#include <util/conf.h>
#include <ffbase/conf.h>

typedef struct ffltconf {
	struct ffconf fc;
	_fcom_conf ff;
} ffltconf;

static inline void ffltconf_init(ffltconf *c)
{
	ffmem_zero_obj(c);
}

static inline int ffltconf_parse(ffltconf *c, ffstr *data)
{
	int r = ffconf_read(&c->fc, data, &c->ff.val);
	c->ff.line = ffconf_line(&c->fc);
	c->ff.linechar = ffconf_col(&c->fc);

	switch (r) {
	case FFCONF_MORE:
		r = FFCONF_RMORE;
		break;

	case FFCONF_KEY:
		r = FFCONF_RKEY;
		if (ffstr_eqcz(&c->ff.val, "}")
			&& !(c->fc.flags & FFCONF_FQUOTED)) {
			if (c->ff.ctxs == 0)
				return -FFCONF_ECTX;
			c->ff.ctxs--;
			r = FFCONF_ROBJ_CLOSE;
		}
		break;

	case FFCONF_VAL:
		r = FFCONF_RVAL;
		if (ffstr_eqcz(&c->ff.val, "{")
			&& !(c->fc.flags & FFCONF_FQUOTED)) {
			c->ff.ctxs++;
			r = FFCONF_ROBJ_OPEN;
		}
		break;

	case FFCONF_VAL_NEXT:
		r = FFCONF_RVAL_NEXT;
		if (ffstr_eqcz(&c->ff.val, "{")
			&& !(c->fc.flags & FFCONF_FQUOTED)) {
			c->ff.ctxs++;
			r = FFCONF_ROBJ_OPEN;
		}
		break;

	case FFCONF_ERROR:
		r = -FFCONF_ESTR;
		break;
	}
	return r;
}

static inline int ffltconf_parse3(ffltconf *c, ffstr *input, ffstr *output)
{
	int r = ffltconf_parse(c, input);
	*output = c->ff.val;
	return r;
}

static inline int ffltconf_fin(ffltconf *c)
{
	ffvec_free(&c->ff.buf);
	if (c->ff.ctxs != 0)
		return -FFCONF_ECTX;
	return 0;
}
