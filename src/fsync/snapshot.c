/** Load/save snapshot from/to file.
Copyright (c) 2019 Simon Zolin */

/* ver.0 format:
# fcom file tree snapshot
d "/d" {
v 0 // version
// name size attr uid gid mtime crc
f "1" 0 0 0 0 0 0
}
d "/d/1" {
v 0
f "2" 0 0 0 0 0 0
}
*/

#include <fsync/fsync.h>


struct ssloader {
	struct dir *top;
	struct dir *dcur;
	struct file *fcur;
};

static int ss_d_v(ffparser_schem *p, void *obj, int64 *v)
{
	if (*v != 0)
		return FFPARS_EBADVAL;
	return 0;
}
// name size attr uid gid mtime crc
static int ss_d_f(ffparser_schem *p, void *obj, ffstr *v)
{
	struct ssloader *ssl = obj;
	uint64 i8;
	uint i4;

	switch (p->list_idx) {
	case 0:
		ssl->fcur = dir_newfile(ssl->dcur);
		ffmem_tzero(ssl->fcur);
		ssl->fcur->parent = ssl->dcur;
		ssl->fcur->name = ffsz_alcopystr(v);
		break;
	case 1:
		if (!ffstr_toint(v, &i8, FFS_INT64))
			return FFPARS_EBADVAL;
		ssl->fcur->size = i8;
		break;
	case 2:
		if (!ffstr_toint(v, &i4, FFS_INT32 | FFS_INTHEX))
			return FFPARS_EBADVAL;
		ssl->fcur->attr = i4;
		break;
	case 3:
	case 4:
		break;
	case 5: {
		ffdatetime dt;
		if (v->len != fftime_fromstr1(&dt, v->ptr, v->len, FFTIME_YMD))
			return FFPARS_EBADVAL;
		fftime_join1(&ssl->fcur->mtime, &dt);
		ssl->fcur->mtime.sec -= FFTIME_1970_SECONDS;
		break;
	}
	case 6:
		dbglog(0, "added %s/%s", fsync_if.get(FSYNC_DIRNAME, ssl->fcur), ssl->fcur->name);
		break;
	default:
		return FFPARS_EBADVAL;
	}
	return 0;
}
static const ffpars_arg ss_d_args[] = {
	{ "v",	FFPARS_TINT, FFPARS_DST(&ss_d_v) },
	{ "f",	FFPARS_TSTR | FFPARS_FLIST | FFPARS_FMULTI, FFPARS_DST(&ss_d_f) },
};

static int ss_d(ffparser_schem *p, void *obj, ffpars_ctx *ctx)
{
	struct ssloader *ssl = obj;
	struct dir *d;
	ffstr *name = ffpars_ctxname(p);
	if (NULL == (d = dir_new(name)))
		return FFPARS_ESYS;
	if (ssl->top == NULL)
		ssl->top = d;
	else {
		struct file *f = tree_file_find(ssl->top, name);
		if (f == NULL) {
			fsync_if.tree_free(d);
			warnlog("skipping entry %S", name);
			ffpars_ctx_skip(ctx);
			return 0;
		}
		f->dir = d;
	}
	ssl->dcur = d;
	ffpars_setargs(ctx, ssl, ss_d_args, FFCNT(ss_d_args));
	return 0;
}
static const ffpars_arg ss_args[] = {
	{ "d",	FFPARS_TOBJ | FFPARS_FOBJ1 | FFPARS_FMULTI, FFPARS_DST(&ss_d) },
};

struct dir* snapshot_load(const char *name, uint flags)
{
	ffarr data = {};
	struct ssloader ssl = {};
	ffparser_schem ps;
	ffconf c;
	ffpars_ctx ctx;
	int r;
	void *rc = NULL;

	ffpars_setargs(&ctx, &ssl, ss_args, FFCNT(ss_args));
	ffconf_scheminit(&ps, &c, &ctx);

	if (0 != fffile_readall(&data, name, 100 * 1024 * 1024))
		goto err;

	ffstr s;
	ffstr_set2(&s, &data);
	while (s.len != 0) {
		ffconf_parsestr(&c, &s);
		r = ffconf_schemrun(&ps);
		if (ffpars_iserr(r)) {
			errlog("conf parse: %s", ffpars_errstr(r));
			goto err;
		}
	}

	if (0 != (r = ffconf_schemfin(&ps))) {
		errlog("conf parse: %s", ffpars_errstr(r));
		goto err;
	}
	rc = ssl.top;

err:
	ffarr_free(&data);
	ffconf_parseclose(&c);
	ffpars_schemfree(&ps);
	if (rc == NULL)
		fsync_if.tree_free(ssl.top);
	return rc;
}


/** Write dir info into snapshot. */
void snapshot_writedir(ffconfw *cw, struct dir *d, ffbool close)
{
	if (close)
		ffconf_write(cw, NULL, FFCONF_CLOSE, FFCONF_TOBJ);
	ffconf_write(cw, "d", FFCONF_STRZ, FFCONF_TKEY);
	ffconf_write(cw, dir_path(d), FFCONF_STRZ, FFCONF_TVAL);
	ffconf_write(cw, NULL, FFCONF_OPEN, FFCONF_TOBJ);
	ffconf_write(cw, "v", FFCONF_STRZ, FFCONF_TKEY);
	ffconf_write(cw, "0", FFCONF_STRZ, FFCONF_TVAL);
}

/** Write file info into snapshot. */
void snapshot_writefile(ffconfw *cw, const struct file *f)
{
	ffconf_write(cw, "f", FFCONF_STRZ, FFCONF_TKEY);
	ffconf_write(cw, f->name, FFCONF_STRZ, FFCONF_TVAL);
	ffconf_writeint(cw, f->size, 0, FFCONF_TVAL);
	ffconf_writeint(cw, f->attr, FFINT_HEXLOW, FFCONF_TVAL);
	ffconf_writeint(cw, 0, FFINT_HEXLOW, FFCONF_TVAL);
	ffconf_writeint(cw, 0, FFINT_HEXLOW, FFCONF_TVAL);

	char buf[128];
	fftime mt = f->mtime;
	ffdatetime dt;
	mt.sec += FFTIME_1970_SECONDS;
	fftime_split1(&dt, &mt);
	int n = fftime_tostr1(&dt, buf, sizeof(buf), FFTIME_YMD);
	ffconf_write(cw, buf, n, FFCONF_TVAL);

	ffconf_write(cw, "0", FFCONF_STRZ, FFCONF_TVAL);
}
