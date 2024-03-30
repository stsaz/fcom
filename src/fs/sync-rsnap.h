/** fcom: sync: read snapshot file
2022, Simon Zolin */

#include <util/conf-scheme.h>

struct rsnap {
	uint	state;
	xxvec	ibuf;
	ffstr	data;
	struct ent ent;
	struct snapshot *sd;
	fntree_block *curblock;
	fntree_entry *cur_ent;
	fntree_cursor cur;
	char *fn;

	~rsnap() {
		ffstr_free(&this->ent.name);
		ffmem_free(this->fn);
	}

	int read(const char *fn, ffstr *output);
	int parse(fcom_sync_snapshot *ss, ffstr input);
};

int rsnap::read(const char *fn, ffstr *output)
{
	this->ibuf.len = 0;
	if (0 != fffile_readwhole(fn, &this->ibuf, 100*1024*1024)) {
		fcom_syserrlog("file read: %s", fn);
		return -1;
	}
	ffstr_setstr(output, &this->ibuf);
	this->fn = ffsz_dup(fn);
	return 0;
}

static int rsnap_ver(ffconf_scheme *cs, struct rsnap *sr, ffstr *val)
{
	if (!ffstr_eqz(val, "1"))
		return 0xbad;
	return 0;
}

static int rsnap_file(ffconf_scheme *cs, struct rsnap *sr, ffstr *val)
{
	struct ent *ent = &sr->ent;
	switch (sr->state++) {
	case 0:
		ffstr_dupstr(&ent->name, val);
		break;

	case 1:
		if (!ffstr_to_uint64(val, &ent->d.size))
			goto err;
		break;

	case 2:
		if (0 != ffstr_matchfmt(val, "%xu/%xu", &ent->d.unix_attr, &ent->d.win_attr))
			goto err;
		sr->state++;
		break;

	case 4:
		if (0 != ffstr_matchfmt(val, "%u:%u", &ent->d.uid, &ent->d.gid))
			goto err;
		sr->state++;
		break;

	case 6: {
		ffdatetime dt, dtt;
		ffstr s = *val;
		uint n = fftime_fromstr1(&dt, s.ptr, s.len, FFTIME_DATE_YMD);
		if (n == 0 || s.ptr[n] != '+')
			goto err;
		ffstr_shift(&s, n+1);
		n = fftime_fromstr1(&dtt, s.ptr, s.len, FFTIME_HMS_MSEC);
		if (n == 0)
			goto err;
		dt.hour = dtt.hour;
		dt.minute = dtt.minute;
		dt.second = dtt.second;
		dt.nanosecond = dtt.nanosecond;
		fftime_join1(&ent->d.mtime, &dt);
		break;
	}

	case 7: {
		if (!ffstr_to_uint32(val, &ent->d.crc32))
			goto err;

		sr->state = 0;
		fntree_entry *e = fntree_add(&sr->curblock, ent->name, sizeof(struct fcom_sync_entry));
		struct fcom_sync_entry *d = (struct fcom_sync_entry*)fntree_data(e);
		*d = ent->d;
		ffstr path = fntree_path(sr->curblock);
		fcom_dbglog("added entry '%S/%S'", &path, &ent->name);
		ffstr_free(&ent->name);
		break;
	}
	}
	return 0;

err:
	fcom_errlog("%s: bad snapshot file: near '%S'", sr->fn, val);
	return 0xbad;
}

static int rsnap_branch_close(ffconf_scheme *cs, struct rsnap *sr)
{
	fntree_block *b = sr->curblock;
	if (sr->sd->root == NULL) {
		sr->sd->root = b;
	} else {
		fntree_attach(sr->cur_ent, b);
		sr->sd->total += b->entries;
	}
	return 0;
}

static const ffconf_arg branch_args[] = {
	{ "v",	FFCONF_TSTR, (ffsize)rsnap_ver },
	{ "f",	FFCONF_TSTR | FFCONF_FLIST | FFCONF_FMULTI, (ffsize)rsnap_file },
	{ "d",	FFCONF_TSTR | FFCONF_FLIST | FFCONF_FMULTI, (ffsize)rsnap_file },
	{ NULL,	FFCONF_TCLOSE, (ffsize)rsnap_branch_close },
	{}
};

static char* rsnap_full_fn(fntree_block *parent, fntree_entry *e)
{
	ffstr parent_path = fntree_path(parent);
	ffstr name = fntree_name(e);
	char *full_fn;
	if (parent_path.len != 0)
		full_fn = ffsz_allocfmt("%S/%S", &parent_path, &name);
	else
		full_fn = ffsz_allocfmt("%S", &name);
	return full_fn;
}

static int rsnap_branch(ffconf_scheme *cs, struct rsnap *sr)
{
	char *full_fn = NULL;
	ffstr *path = ffconf_scheme_objval(cs);
	fntree_block *b = fntree_create(*path);
	if (sr->sd->root != NULL) {
		fntree_block *parent = sr->curblock;
		fntree_entry *e;
		for (;;) {
			e = fntree_cur_next_r(&sr->cur, &parent);
			if (e == NULL) {
				fcom_errlog("%s: bad snapshot file: near '%S'", sr->fn, path);
				return 0xbad;
			}

			const struct fcom_sync_entry *d = (struct fcom_sync_entry*)fntree_data(e);
			if (!(d->unix_attr & FFFILE_UNIX_DIR))
				continue;

			full_fn = rsnap_full_fn(parent, e);
			if (!ffstr_eqz(path, full_fn)) {
				fcom_dbglog("skip %s", full_fn);
				continue;
			}

			break;
		}
		sr->cur_ent = e;
	}
	fcom_dbglog("added branch '%S'", path);
	sr->curblock = b;
	ffconf_scheme_addctx(cs, branch_args, sr);
	ffmem_free(full_fn);
	return 0;
}

static const ffconf_arg root_args[] = {
	{ "b",	FFCONF_TOBJ | FFCONF_FMULTI, (ffsize)rsnap_branch },
	{}
};

int rsnap::parse(fcom_sync_snapshot *ss, ffstr input)
{
	this->sd = ss;
	this->cur_ent = NULL;
	ffmem_zero_obj(&this->cur);

	ffstr errmsg = {};
	int r = ffconf_parse_object(root_args, this, &input, 0, &errmsg);
	if (r != 0) {
		fcom_errlog("bad snapshot file: %S", &errmsg);
		r = 0xbad;
	} else {
		r = 0xdeed;
		if (ss->root == NULL) {
			fcom_errlog("bad snapshot file: no data");
			r = 0xbad;
		}

		if (this->cur_ent != NULL)
			fntree_attach(this->cur_ent, this->curblock);
	}

	ffstr_free(&errmsg);
	return r;
}

static fcom_sync_snapshot* sync_snapshot_open(const char *snapshot_path, uint flags)
{
	struct rsnap sr = {};
	ffstr data;
	int r = sr.read(snapshot_path, &data);
	if (r)
		return NULL;
	fcom_sync_snapshot *ss = ffmem_new(fcom_sync_snapshot);
	r = sr.parse(ss, data);
	if (r == 0xbad)
		return NULL;
	return ss;
}
