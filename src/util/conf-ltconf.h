/** ltconf--ffconf bridge
2022, Simon Zolin
*/

#pragma once
#include <util/ltconf.h>
#include <util/conf.h>

typedef struct ffltconf {
	struct ltconf lt;
	ffconf ff;
} ffltconf;

static inline void ffltconf_init(ffltconf *c)
{
	ffmem_zero_obj(c);
}

static inline int ffltconf_parse(ffltconf *c, ffstr *data)
{
	int r = ltconf_read(&c->lt, data, &c->ff.val);
	c->ff.line = ltconf_line(&c->lt);
	c->ff.linechar = ltconf_column(&c->lt);

	switch (r) {
	case LTCONF_MORE:
		r = FFCONF_RMORE;
		break;

	case LTCONF_KEY:
		r = FFCONF_RKEY;
		if (ffstr_eqcz(&c->ff.val, "}")
			&& !(c->lt.flags & LTCONF_FQUOTED)) {
			if (c->ff.ctxs == 0)
				return -FFCONF_ECTX;
			c->ff.ctxs--;
			r = FFCONF_ROBJ_CLOSE;
		}
		break;

	case LTCONF_VAL:
		r = FFCONF_RVAL;
		if (ffstr_eqcz(&c->ff.val, "{")
			&& !(c->lt.flags & LTCONF_FQUOTED)) {
			c->ff.ctxs++;
			r = FFCONF_ROBJ_OPEN;
		}
		break;

	case LTCONF_VAL_NEXT:
		r = FFCONF_RVAL_NEXT;
		if (ffstr_eqcz(&c->ff.val, "{")
			&& !(c->lt.flags & LTCONF_FQUOTED)) {
			c->ff.ctxs++;
			r = FFCONF_ROBJ_OPEN;
		}
		break;

	case LTCONF_ERROR:
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
