/** Windows OS operations.
Copyright (c) 2017 Simon Zolin
*/

#include <fcom.h>

#include <util/wreg.h>
#include <util/time.h>


#define dbglog(dbglev, fmt, ...)  fcom_dbglog(dbglev, "com", fmt, __VA_ARGS__)
#define syserrlog(fmt, ...)  fcom_syserrlog("com", fmt, __VA_ARGS__)

extern const fcom_core *core;
static const fcom_command *com;

// WREGFIND
static void* wregfind_open(fcom_cmd *cmd);
static void wregfind_close(void *p, fcom_cmd *cmd);
static int wregfind_process(void *p, fcom_cmd *cmd);
const fcom_filter wregfind_filt = { &wregfind_open, &wregfind_close, &wregfind_process };

struct wregfind;
static int wrf_inadd(struct wregfind *c, fcom_cmd *cmd);
static void wrf_enum_vals(struct wregfind *c);
static int wrf_enum_keys(struct wregfind *c);
static struct key_ent* wrf_addkey(struct wregfind *c);
static int wrf_find(struct wregfind *c, const ffstr *where);


#define FILT_NAME  "wregfind"

struct wregfind {
	ffarr find; //ffstr[]
	ffwreg_enum e;
	ffwreg key;
	ffchain blocks;
	ffchain keys;
	ffchain_item *lr;
	struct {
		uint subkeys
			, subkeys_all
			, vals
			, vals_all;
	} stat;
};

struct key_ent {
	char *key;
	ssize_t hkey;
	const char *hkey_name;
	ffmblk *mblk;
	ffchain_item sib;
};

static void* wregfind_open(fcom_cmd *cmd)
{
	struct wregfind *c;
	if (NULL == (c = ffmem_new(struct wregfind)))
		return FCOM_OPEN_SYSERR;
	c->key = FFWREG_BADKEY;
	ffchain_init(&c->blocks);
	ffchain_init(&c->keys);
	c->lr = ffchain_sentl(&c->keys);

	if (0 != ffwreg_enuminit(&c->e, FFWREG_ENUM_VALSTR))
		goto end;

	if (com == NULL)
		com = core->iface("core.com");

	return c;

end:
	wregfind_close(c, cmd);
	return FCOM_OPEN_SYSERR;
}

static void wregfind_close(void *p, fcom_cmd *cmd)
{
	struct wregfind *c = p;
	FF_SAFECLOSE(c->key, FFWREG_BADKEY, ffwreg_close);
	ffwreg_enumclose(&c->e);

	ffarr_free(&c->find);

	struct key_ent *ke;
	ffchain_item *it;
	ffmblk *mblk;
	FFCHAIN_FOR(&c->blocks, it) {
		mblk = FF_GETPTR(ffmblk, sib, it);
		it = it->next;
		FFARR_WALKT(&mblk->buf, ke, struct key_ent) {
			ffmem_safefree(ke->key);
		}
		ffmblk_free(mblk);
	}

	ffstdout_fmt("subkeys:%u (%u)  values:%u (%u)\n"
		, c->stat.subkeys, c->stat.subkeys_all, c->stat.vals, c->stat.vals_all);

	ffmem_free(c);
}

static const int hkeys[] = {
	(ssize_t)HKEY_CURRENT_USER,
	(ssize_t)HKEY_LOCAL_MACHINE,
};

static int wregfind_process(void *p, fcom_cmd *cmd)
{
	struct wregfind *c = p;
	int r = FCOM_ERR;
	struct key_ent *ke;

	if (0 != (r = wrf_inadd(c, cmd)))
		return r;

	if (cmd->members.len != 0) {
		char **it;
		ffchain_item *lw = c->lr;
		ssize_t n;
		FFARR_WALKT(&cmd->members, it, char*) {
			ffstr skey, spath, s;
			ffstr_setz(&s, *it);
			ffwreg_pathsplit(s.ptr, s.len, &skey, &spath);
			n = ffwreg_hkey_fromstr(skey.ptr, skey.len);
			if (n == 0) {
				fcom_errlog(FILT_NAME, "unknown HKEY %S", &skey);
				return FCOM_ERR;
			}

			if (NULL == (ke = wrf_addkey(c)))
				return FCOM_SYSERR;
			ffchain_append(&ke->sib, lw);
			if (NULL == (ke->key = ffsz_alcopystr(&spath)))
				return FCOM_SYSERR;
			ke->hkey = n;
			ke->hkey_name = ffwreg_hkey_tostr(n);
			lw = &ke->sib;
		}

	} else {
		ffchain_item *lw = c->lr;
		int *it;
		FFARR_WALKNT(hkeys, FFCNT(hkeys), it, int) {
			if (NULL == (ke = wrf_addkey(c)))
				return FCOM_SYSERR;
			ffchain_append(&ke->sib, lw);
			ke->hkey = *it;
			ke->hkey_name = ffwreg_hkey_tostr(*it);
			if (NULL == (ke->key = ffsz_alcopy(NULL, 0)))
				return FCOM_SYSERR;
			lw = &ke->sib;
		}
	}

	for (;;) {

		c->lr = c->lr->next;
		if (c->lr == ffchain_sentl(&c->keys))
			return FCOM_DONE;
		if (c->lr->prev != ffchain_sentl(&c->keys)) {
			ke = FF_GETPTR(struct key_ent, sib, c->lr->prev);
			ffmem_free0(ke->key);
			ffchain_unlink(&ke->sib);
			if (ke->mblk->buf.len == 0) {
				ffchain_unlink(&ke->mblk->sib);
				ffmblk_free(ke->mblk);
			}
		}

		ke = FF_GETPTR(struct key_ent, sib, c->lr);
		if (FFWREG_BADKEY == (c->key = ffwreg_open((HKEY)ke->hkey, ke->key, O_RDONLY))) {
			fcom_syswarnlog(FILT_NAME, "ffwreg_open(): %s\\%s", ke->hkey_name, ke->key);
			continue;
		}
		c->e.path = ke->key;

		wrf_enum_vals(c);

		if (0 != (r = wrf_enum_keys(c)))
			return r;
		FF_SAFECLOSE(c->key, FFWREG_BADKEY, ffwreg_close);
	}

	return FCOM_ERR;
}

/** Create an array of data to search for. */
static int wrf_inadd(struct wregfind *c, fcom_cmd *cmd)
{
	const char *s;
	for (;;) {
		if (NULL == (s = com->arg_next(cmd, 0)))
			break;
		ffstr *ns;
		if (NULL == (ns = ffarr_pushgrowT(&c->find, 8, ffstr)))
			return FCOM_SYSERR;
		ffstr_setz(ns, s);
	}
	return 0;
}

/** Search for needed data among the values of the opened registry key. */
static void wrf_enum_vals(struct wregfind *c)
{
	int r;
	struct key_ent *ke;
	ke = FF_GETPTR(struct key_ent, sib, c->lr);

	for (;;) {

		r = ffwreg_nextval(&c->e, c->key);
		if (r < 0) {
			fcom_syswarnlog(FILT_NAME, "ffwreg_nextval()", 0);
			break;
		} else if (r == 0)
			break;

		c->stat.vals_all++;

		if (0 != wrf_find(c, (ffstr*)&c->e.name) || 0 != wrf_find(c, (ffstr*)&c->e.value)) {
			c->stat.vals++;

			if (ke->key[0] != '\0')
				fcom_userlog("%s\\%s\\%S = \"%S\"", ke->hkey_name, ke->key, &c->e.name, &c->e.value);
			else
				fcom_userlog("%s\\%S = \"%S\"", ke->hkey_name, &c->e.name, &c->e.value);
		}
	}
	ffwreg_enumreset(&c->e);
}

/** Search for needed data among the subkeys of the opened registry key.
Add all subkeys to a list for later processing. */
static int wrf_enum_keys(struct wregfind *c)
{
	int r;
	struct key_ent *nke, *ke;
	ffarr path = {0};
	ffchain_item *lw = c->lr;
	ke = FF_GETPTR(struct key_ent, sib, c->lr);

	for (;;) {

		r = ffwreg_nextkey(&c->e, c->key);
		if (r < 0) {
			fcom_syswarnlog(FILT_NAME, "ffwreg_nextkey()", 0);
			break;
		} else if (r == 0)
			break;

		c->stat.subkeys_all++;

		if (0 == ffstr_catfmt(&path, (ke->key[0] != '\0') ? "%s\\%S%Z" : "%s%S%Z", ke->key, &c->e.name))
			return FCOM_SYSERR;
		path.len--;

		if (0 != wrf_find(c, (ffstr*)&c->e.name)) {
			c->stat.subkeys++;

			struct ffwreg_info nf;
			r = -1;

			if (fcom_logchk(core->conf->loglev, FCOM_LOGVERB)) {
				ffwreg k;
				if (FFWREG_BADKEY != (k = ffwreg_open((HKEY)ke->hkey, path.ptr, O_RDONLY))) {
					r = ffwreg_info(k, &nf);
				}
				ffwreg_close(k);
			}

			if (r == 0) {
				fftime t = fftime_from_winftime((void*)&nf.mtime);
				ffdtm dt;
				fftime_split(&dt, &t, FFTIME_TZLOCAL);
				char buf[128];
				r = fftime_tostr(&dt, buf, sizeof(buf), FFTIME_DATE_YMD | FFTIME_HMS);
				fcom_userlog("%s\\%S\\  subkeys:%u  values:%u  modified:%*s"
					, ke->hkey_name, &path
					, (uint)nf.subkeys, (uint)nf.values, (size_t)r, buf);
			} else {
				fcom_userlog("%s\\%S\\", ke->hkey_name, &path);
			}
		}

		if (NULL == (nke = wrf_addkey(c)))
			return FCOM_SYSERR;
		ffchain_append(&nke->sib, lw);
		nke->key = path.ptr;
		ffarr_null(&path);
		nke->hkey = ke->hkey;
		nke->hkey_name = ke->hkey_name;
		lw = &nke->sib;
	}

	ffwreg_enumreset(&c->e);
	ffarr_free(&path);
	return 0;
}

/** Search for needed data within the specified string.
Return 1 if found. */
static int wrf_find(struct wregfind *c, const ffstr *where)
{
	if (c->find.len == 0)
		return 1;

	ffstr *s;
	FFARR_WALKT(&c->find, s, ffstr) {
		if (0 <= ffstr_ifindstr(where, s))
			return 1;
	}
	return 0;
}

/** Add a subkey to a list for later processing. */
static struct key_ent* wrf_addkey(struct wregfind *c)
{
	ffmblk *mblk = ffmblk_chain_last(&c->blocks);
	if (mblk == NULL || 0 == ffarr_unused(&mblk->buf)) {

		if (NULL == (mblk = ffmblk_chain_push(&c->blocks)))
			return NULL;
		if (NULL == ffarr_allocT(&mblk->buf, 1024, struct key_ent))
			return NULL;

	}

	struct key_ent *ke = ffarr_pushT(&mblk->buf, struct key_ent);
	ke->mblk = mblk;
	return ke;
}

#undef FILT_NAME
