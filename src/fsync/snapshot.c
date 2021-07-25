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
	ffuint list_idx;
};

static int ss_d_v(ffconf_scheme *cs, void *obj, int64 v)
{
	if (v != 0)
		return FFCONF_EBADVAL;
	return 0;
}
// name size attr uid gid mtime crc
static int ss_d_f(ffconf_scheme *cs, void *obj, ffstr *v)
{
	struct ssloader *ssl = obj;
	uint64 i8;
	uint i4;

	switch (ssl->list_idx++) {
	case 0:
		ssl->fcur = dir_newfile(ssl->dcur);
		ffmem_tzero(ssl->fcur);
		ssl->fcur->parent = ssl->dcur;
		ssl->fcur->name = ffsz_alcopystr(v);
		break;
	case 1:
		if (!ffstr_toint(v, &i8, FFS_INT64))
			return FFCONF_EBADVAL;
		ssl->fcur->size = i8;
		break;
	case 2:
		if (!ffstr_toint(v, &i4, FFS_INT32 | FFS_INTHEX))
			return FFCONF_EBADVAL;
		ssl->fcur->attr = i4;
		break;
	case 3:
	case 4:
		break;
	case 5: {
		ffdatetime dt;
		if (v->len != fftime_fromstr1(&dt, v->ptr, v->len, FFTIME_YMD))
			return FFCONF_EBADVAL;
		fftime_join1(&ssl->fcur->mtime, &dt);
		ssl->fcur->mtime.sec -= FFTIME_1970_SECONDS;
		break;
	}
	case 6:
		dbglog(0, "added %s/%s", fsync_if.get(FSYNC_DIRNAME, ssl->fcur), ssl->fcur->name);
		break;
	default:
		return FFCONF_EBADVAL;
	}
	return 0;
}
static const ffconf_arg ss_d_args[] = {
	{ "v",	FFCONF_TINT32, (ffsize)ss_d_v },
	{ "f",	FFCONF_TSTR | FFCONF_FLIST | FFCONF_FMULTI, (ffsize)ss_d_f },
	{}
};

static int ss_d(ffconf_scheme *cs, void *obj)
{
	struct ssloader *ssl = obj;
	struct dir *d;
	ffstr *name = ffconf_scheme_objval(cs);
	if (NULL == (d = dir_new(name)))
		return FFCONF_ESYS;
	if (ssl->top == NULL)
		ssl->top = d;
	else {
		struct file *f = tree_file_find(ssl->top, name);
		if (f == NULL) {
			fsync_if.tree_free(d);
			warnlog("skipping entry %S", name);
			ffconf_scheme_skipctx(cs);
			return 0;
		}
		f->dir = d;
	}
	ssl->dcur = d;
	ssl->list_idx = 0;
	ffconf_scheme_addctx(cs, ss_d_args, ssl);
	return 0;
}
static const ffconf_arg ss_args[] = {
	{ "d",	FFCONF_TOBJ | FFCONF_FNOTEMPTY | FFCONF_FMULTI, (ffsize)ss_d },
	{}
};

struct dir* snapshot_load(const char *name, uint flags)
{
	struct ssloader ssl = {};
	void *rc = NULL;

	ffstr errmsg = {};
	int r = ffconf_parse_file(ss_args, &ssl, name, 0, &errmsg);
	if (r != 0) {
		errlog("conf parse: %s: %S", name, &errmsg);
		goto err;
	}
	rc = ssl.top;

err:
	ffstr_free(&errmsg);
	if (rc == NULL)
		fsync_if.tree_free(ssl.top);
	return rc;
}


/** Write dir info into snapshot. */
void snapshot_writedir(ffconfw *cw, struct dir *d, ffbool close)
{
	if (close)
		ffconfw_addobj(cw, 0);
	ffconfw_addkeyz(cw, "d");
	ffconfw_addstrz(cw, dir_path(d));
	ffconfw_addobj(cw, 1);
	ffconfw_addkeyz(cw, "v");
	ffconfw_addstrz(cw, "0");
}

/** Write file info into snapshot. */
void snapshot_writefile(ffconfw *cw, const struct file *f)
{
	ffconfw_addkeyz(cw, "f");
	ffconfw_addstrz(cw, f->name);
	ffconfw_addint(cw, f->size);
	ffconfw_addintf(cw, f->attr, FFINT_HEXLOW);
	ffconfw_addintf(cw, 0, FFINT_HEXLOW);
	ffconfw_addintf(cw, 0, FFINT_HEXLOW);

	char buf[128];
	fftime mt = f->mtime;
	ffdatetime dt;
	mt.sec += FFTIME_1970_SECONDS;
	fftime_split1(&dt, &mt);
	int n = fftime_tostr1(&dt, buf, sizeof(buf), FFTIME_YMD);
	ffstr s;
	ffstr_set(&s, buf, n);
	ffconfw_addstr(cw, &s);

	ffconfw_addstrz(cw, "0");
}
