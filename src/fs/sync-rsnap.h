/** fcom: sync: read snapshot file
2022, Simon Zolin */

#include <util/conf-scheme.h>

static void rsnap_destroy(struct sync *s)
{
	ffstr_free(&s->sr.ent.name);
}

static int rsnap_read(struct sync *s, const char *fn, ffstr *output)
{
	s->sr.ibuf.len = 0;
	if (0 != fffile_readwhole(fn, &s->sr.ibuf, 100*1024*1024)) {
		fcom_syserrlog("file read: %s", fn);
		return FCOM_FILE_ERR;
	}
	ffstr_setstr(output, &s->sr.ibuf);
	s->sr.fn = fn;
	return 0;
}

static int rsnap_ver(ffconf_scheme *cs, struct sync *s, ffstr *val)
{
	if (!ffstr_eqz(val, "1"))
		return 0xbad;
	return 0;
}

static int rsnap_file(ffconf_scheme *cs, struct sync *s, ffstr *val)
{
	struct ent *ent = &s->sr.ent;
	switch (s->sr.state++) {
	case 0:
		ffstr_dupstr(&ent->name, val);
		break;
	case 1:
		if (!ffstr_to_uint64(val, &ent->d.size))
			goto err;
		break;
	case 2:
		if (0 != ffstr_matchfmt(val, "%xu/%xu", &ent->d.unixattr, &ent->d.winattr))
			goto err;
		s->sr.state++;
		break;
	case 4:
		if (0 != ffstr_matchfmt(val, "%u:%u", &ent->d.uid, &ent->d.gid))
			goto err;
		s->sr.state++;
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

		s->sr.state = 0;
		fntree_entry *e = fntree_add(&s->sr.curblock, ent->name, sizeof(struct entdata));
		struct entdata *d = (entdata*)fntree_data(e);
		*d = ent->d;
		ffstr path = fntree_path(s->sr.curblock);
		fcom_dbglog("added entry '%S/%S'", &path, &ent->name);
		ffstr_free(&ent->name);
		break;
	}
	}
	return 0;

err:
	fcom_errlog("%s: bad snapshot file: near '%S'", s->sr.fn, val);
	return 0xbad;
}

static int rsnap_branch_close(ffconf_scheme *cs, struct sync *s)
{
	fntree_block *b = s->sr.curblock;
	if (s->sr.sd->root == NULL) {
		s->sr.sd->root = b;
	} else {
		fntree_attach(s->sr.cur_ent, b);
		s->sr.sd->total += b->entries;
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

static int rsnap_branch(ffconf_scheme *cs, struct sync *s)
{
	char *full_fn = NULL;
	ffstr *path = ffconf_scheme_objval(cs);
	fntree_block *b = fntree_create(*path);
	if (s->sr.sd->root != NULL) {
		fntree_block *parent = s->sr.curblock;
		fntree_entry *e;
		for (;;) {
			e = fntree_cur_next_r(&s->sr.cur, &parent);
			if (e == NULL) {
				fcom_errlog("%s: bad snapshot file: near '%S'", s->sr.fn, path);
				return 0xbad;
			}

			const struct entdata *d = (entdata*)fntree_data(e);
			if (!(d->unixattr & FFFILE_UNIX_DIR))
				continue;

			full_fn = rsnap_full_fn(parent, e);
			if (!ffstr_eqz(path, full_fn)) {
				fcom_dbglog("skip %s", full_fn);
				continue;
			}

			break;
		}
		s->sr.cur_ent = e;
	}
	fcom_dbglog("added branch '%S'", path);
	s->sr.curblock = b;
	ffconf_scheme_addctx(cs, branch_args, s);
	ffmem_free(full_fn);
	return 0;
}

static const ffconf_arg root_args[] = {
	{ "b",	FFCONF_TOBJ | FFCONF_FMULTI, (ffsize)rsnap_branch },
	{}
};

static int rsnap_parse(struct sync *s, struct srcdst *sd, ffstr input)
{
	s->sr.sd = sd;
	s->sr.cur_ent = NULL;
	ffmem_zero_obj(&s->sr.cur);

	ffstr errmsg = {};
	int r = ffconf_parse_object(root_args, s, &input, 0, &errmsg);
	if (r != 0) {
		fcom_errlog("bad snapshot file: %S", &errmsg);
		r = 0xbad;
	} else {
		r = 0xdeed;
		if (sd->root == NULL) {
			fcom_errlog("bad snapshot file: no data");
			r = 0xbad;
		}

		if (s->sr.cur_ent != NULL)
			fntree_attach(s->sr.cur_ent, s->sr.curblock);
	}

	ffstr_free(&errmsg);
	return r;
}
