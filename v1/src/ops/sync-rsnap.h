/** fcom: sync: read snapshot file
2022, Simon Zolin */

#include <util/conf-scheme.h>

static void rsnap_destroy(struct sync *s)
{
	ffstr_free(&s->sr.ent.name);
	ffvec_free(&s->sr.ibuf);
}

static int rsnap_read(struct sync *s, ffstr *output)
{
	if (s->cmd->input.len == 0) {
		fcom_fatlog("Please specify input snapshot file");
		return FCOM_FILE_ERR;
	}

	ffstr *in = s->cmd->input.ptr;
	const char *fn = in[0].ptr;
	if (0 != fffile_readwhole(fn, &s->sr.ibuf, 100*1024*1024)) {
		fcom_syserrlog("file read: %s", fn);
		return FCOM_FILE_ERR;
	}
	ffstr_setstr(output, &s->sr.ibuf);
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
		ffdatetime dt;
		ffstr s = *val;
		uint n = fftime_fromstr1(&dt, s.ptr, s.len, FFTIME_DATE_YMD);
		if (n == 0 || s.ptr[n] != '+')
			goto err;
		ffstr_shift(&s, n+1);
		n = fftime_fromstr1(&dt, s.ptr, s.len, FFTIME_HMS_MSEC);
		if (n == 0)
			goto err;
		fftime_join1(&ent->d.mtime, &dt);
		break;
	}
	case 7: {
		if (!ffstr_to_uint32(val, &ent->d.crc32))
			goto err;

		s->sr.state = 0;
		fntree_entry *e = fntree_add(&s->sr.curblock, ent->name, sizeof(struct entdata));
		struct entdata *d = fntree_data(e);
		*d = ent->d;
		fcom_dbglog("added entry '%S'", &ent->name);
		ffstr_free(&ent->name);
		break;
	}
	}
	return 0;

err:
	fcom_errlog("bad snapshot file: near '%S'", val);
	return 0xbad;
}

static const ffconf_arg branch_args[] = {
	{ "v",	FFCONF_TSTR, (ffsize)rsnap_ver },
	{ "f",	FFCONF_TSTR | FFCONF_FLIST | FFCONF_FMULTI, (ffsize)rsnap_file },
	{ "d",	FFCONF_TSTR | FFCONF_FLIST | FFCONF_FMULTI, (ffsize)rsnap_file },
	{}
};

static int rsnap_branch(ffconf_scheme *cs, struct sync *s)
{
	ffstr *path = ffconf_scheme_objval(cs);
	fntree_block *b = fntree_create(*path);
	if (s->sr.sd->root == NULL) {
		s->sr.sd->root = b;
	} else {
		s->sr.sd->total += s->sr.curblock->entries;
		fntree_block *parent = s->sr.curblock;
		fntree_entry *e;
		for (;;) {
			e = fntree_cur_next_r(&s->sr.cur, &parent);
			if (e == NULL) {
				fcom_errlog("bad snapshot file: near '%S'", path);
				return 0xbad;
			}
			const struct entdata *d = fntree_data(e);
			if (d->unixattr & FFFILE_UNIX_DIR)
				break;
		}
		fntree_attach(e, b);
	}
	fcom_dbglog("added branch '%S'", path);
	s->sr.curblock = b;
	ffconf_scheme_addctx(cs, branch_args, s);
	return 0;
}

static const ffconf_arg root_args[] = {
	{ "b",	FFCONF_TOBJ | FFCONF_FMULTI, (ffsize)rsnap_branch },
	{}
};

static int rsnap_parse(struct sync *s, struct srcdst *sd, ffstr input)
{
	s->sr.sd = sd;
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
	}

	ffstr_free(&errmsg);
	return r;
}
