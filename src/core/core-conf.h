/** fcom: core config
2021, Simon Zolin */

int conf_workers(ffconf_scheme *cs, struct fcom_conf *conf, int64 val)
{
	if (conf->workers == 0)
		conf->workers = val;
	return 0;
}
int conf_codepage(ffconf_scheme *cs, struct fcom_conf *conf, const ffstr *val)
{
	static const char *const cp_str[] = {
		"win1251", // FFUNICODE_WIN1251
		"win1252", // FFUNICODE_WIN1252
		"win866", // FFUNICODE_WIN866
	};
	int cp = ffszarr_ifindsorted(cp_str, FF_COUNT(cp_str), val->ptr, val->len);
	if (cp < 0)
		return FFCONF_EBADVAL;
	conf->codepage = cp + _FFUNICODE_CP_BEGIN;
	return 0;
}
static int conf_mod(ffconf_scheme *cs, void *obj, const ffstr *val)
{
	if (0 != mod_add(val, NULL))
		return FFCONF_EBADVAL;
	return 0;
}
static int conf_modconf(ffconf_scheme *cs, void *obj)
{
	const ffstr *name = ffconf_scheme_objval(cs);
	if (0 != mod_add(name, cs))
		return FFCONF_EBADVAL;
	return 0;
}
static const ffconf_arg conf_args[] = {
	{ "codepage",	FFCONF_TSTR, (ffsize)conf_codepage },
	{ "workers",	FFCONF_TINT32, (ffsize)conf_workers },
	{ "mod_conf",	FFCONF_TOBJ | FFCONF_FNOTEMPTY | FFCONF_FMULTI, (ffsize)conf_modconf },
	{ "mod",	FFCONF_TSTR | FFCONF_FNOTEMPTY | FFCONF_FMULTI, (ffsize)conf_mod },
	{}
};

static int readconf(const char *fn)
{
	int r = -1;
	char *fullfn;
	if (NULL == (fullfn = core_getpath(fn, ffsz_len(fn))))
		goto end;

	dbglog(0, "reading config file %s", fullfn);

	ffstr errmsg = {};
	if (0 != ffconf_parse_file(conf_args, &g->conf, fullfn, 0, &errmsg)) {
		errlog("parse config: %s: %S", fullfn, &errmsg);
		goto end;
	}

	r = 0;

end:
	ffstr_free(&errmsg);
	ffmem_safefree(fullfn);
	return r;
}

static int setconf(fcom_conf *conf)
{
	g->conf.loglev = conf->loglev;
	g->conf.workers = conf->workers;
	return 0;
}
