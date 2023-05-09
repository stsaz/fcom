/** fcom: Windows Registry utils: search
2017,2023, Simon Zolin */

#include <util/mbuf.h>

struct key_ent {
	char *key;
	ssize_t hkey;
	const char *hkey_name;
	ffmbuf *mblk;
	ffchain_item sib;
};

static void reg_search_close(struct reg *g)
{
	fcom_verblog("subkeys:%u (%u)  values:%u (%u)\n"
		, g->stat.subkeys, g->stat.subkeys_all, g->stat.vals, g->stat.vals_all);

	if (g->key != FFWINREG_NULL) {
		ffwinreg_close(g->key);
		g->key = FFWINREG_NULL;
	}
	ffwinreg_enum_destroy(&g->e);

	ffchain_item *it;
	FFLIST_FOR(&g->blocks, it) {
		ffmbuf *mblk = FF_STRUCTPTR(ffmbuf, sib, it);
		it = it->next;

		struct key_ent *ke;
		FFSLICE_WALK(&mblk->buf, ke) {
			ffmem_free(ke->key);
		}
		ffmbuf_free(mblk);
	}

	ffvec_free(&g->values);
}

/** Add a subkey to a list for later processing. */
static struct key_ent* reg_addkey(struct reg *g)
{
	ffmbuf *mblk = ffmbuf_chain_last(&g->blocks);
	if (mblk == NULL || 0 == ffvec_unused(&mblk->buf)) {

		mblk = ffmbuf_chain_push(&g->blocks);
		ffvec_allocT(&mblk->buf, 1024, struct key_ent);
	}

	struct key_ent *ke = ffvec_pushT(&mblk->buf, struct key_ent);
	ke->mblk = mblk;
	return ke;
}

static const char names_short[][4] = {
	"HKCR",
	"HKCU",
	"HKLM",
	"HKU",
};
static const char names_long[][19] = {
	"HKEY_CLASSES_ROOT",
	"HKEY_CURRENT_USER",
	"HKEY_LOCAL_MACHINE",
	"HKEY_USERS",
};
static const int hkeys[] = {
	(ffssize)HKEY_CLASSES_ROOT,
	(ffssize)HKEY_CURRENT_USER,
	(ffssize)HKEY_LOCAL_MACHINE,
	(ffssize)HKEY_USERS,
};

/** Convert string -> HKEY. */
static inline int ffwinreg_hkey_fromstr(const char *name, size_t len)
{
	int r;
	if (len <= 4)
		r = ffcharr_findsorted(names_short, FF_COUNT(names_short), sizeof(*names_short), name, len);
	else
		r = ffcharr_findsorted(names_long, FF_COUNT(names_long), sizeof(*names_long), name, len);
	if (r < 0)
		return 0;
	return hkeys[r];
}

/** Split "HKEY_CURRENT_USER\Key1\Key2" -> {"HKEY_CURRENT_USER", "Key1\Key2"} */
static inline void ffwinreg_pathsplit(ffstr fullpath, ffstr *hkey, ffstr *subkey)
{
	ffstr_splitby(&fullpath, '\\', hkey, subkey);
}

/** Convert HKEY -> string. */
static inline const char* ffwinreg_hkey_tostr(int hkey)
{
	ssize_t r;
	if (0 > (r = ffarrint32_find((uint*)hkeys, FF_COUNT(hkeys), hkey)))
		return "";
	return names_long[r];
}

static int key_add(struct reg *g, ffstr name)
{
	ffstr skey, spath;
	ffwinreg_pathsplit(name, &skey, &spath);
	ffssize n = ffwinreg_hkey_fromstr(skey.ptr, skey.len);
	if (n == 0) {
		fcom_errlog("unknown HKEY %S", &skey);
		return -1;
	}

	struct key_ent *ke = reg_addkey(g);
	fflist_add(&g->keys, &ke->sib);
	ke->hkey = n;
	ke->hkey_name = ffwinreg_hkey_tostr(n);
	ke->key = ffsz_dupstr(&spath);
	return 0;
}

static void key_add2(struct reg *g, struct key_ent *parent, char *path)
{
	struct key_ent *ke = reg_addkey(g);
	ffchain_item_append(&ke->sib, g->lw);
	g->keys.len++;
	g->lw = &ke->sib;
	ke->hkey = parent->hkey;
	ke->hkey_name = parent->hkey_name;
	ke->key = path;
}

static int reg_search_args(struct reg *g)
{
	g->key = FFWINREG_NULL;
	fflist_init(&g->blocks);
	fflist_init(&g->keys);
	g->lr = fflist_sentl(&g->keys);
	ffwinreg_enum_init(&g->e, FFWINREG_ENUM_VALSTR);

	ffstr *it;
	FFSLICE_WALK(&g->cmd->input, it) {

		if (g->oper == 0) {
			if (ffstr_eqz(it, "search")) {
				g->oper = 'find';
			} else {
				fcom_fatlog("Operation not supported: %S", it);
				return -1;
			}
			continue;
		}

		if (ffstr_matchz(it, "HKEY_")) {
			if (0 != key_add(g, *it))
				return -1;
			continue;
		}

		*ffvec_pushT(&g->values, ffstr) = *it;
	}

	if (g->oper == 0) {
		fcom_fatlog("Please specify operation");
		return -1;
	}

	if (g->keys.len == 0) {
		static const char* def_keys[] = {
			"HKEY_CURRENT_USER",
			"HKEY_LOCAL_MACHINE",
		};
		for (uint i = 0;  i != FF_COUNT(def_keys);  i++) {
			ffstr s = FFSTR_INITZ(def_keys[i]);
			key_add(g, s);
		}
	}

	if (g->values.len == 0) {
		fcom_fatlog("Please specify values to search");
		return -1;
	}

	if (g->cmd->output.len == 0)
		g->cmd->stdout = 1;
	return 0;
}

/** Search for needed data within the specified string.
Return 1 if found. */
static int reg_find(struct reg *g, ffstr where)
{
	if (g->values.len == 0)
		return 1;

	ffstr *s;
	FFSLICE_WALK(&g->values, s) {
		if (0 <= ffstr_ifindstr(&where, s))
			return 1;
	}
	return 0;
}

/** Search for needed data among the subkeys of the opened registry key.
Add all subkeys to a list for later processing. */
static int reg_enum_keys(struct reg *g)
{
	int r;
	struct key_ent *ke;
	ffvec path = {};
	ke = FF_STRUCTPTR(struct key_ent, sib, g->lr);

	for (;;) {

		ffstr name;
		r = ffwinreg_enum_nextkey(&g->e, &name);
		if (r < 0) {
			fcom_syswarnlog("ffwinreg_enum_nextkey()");
			break;
		} else if (r > 0) {
			break;
		}

		g->stat.subkeys_all++;

		ffvec_addfmt(&path, (ke->key[0] != '\0') ? "%s\\%S%Z" : "%s%S%Z"
			, ke->key, &name);
		path.len--;
		fcom_dbglog("subkey: %s", path.ptr);

		key_add2(g, ke, path.ptr);

		if (0 != reg_find(g, name)) {
			g->stat.subkeys++;

			g->buf.len = 0;
			ffvec_addfmt(&g->buf, "%s\\%S\\\n", ke->hkey_name, &path);
			ffvec_null(&path);
			return 1;
		}
		ffvec_null(&path);
	}

	ffvec_free(&path);
	return 0;
}

/** Search for needed data among the values of the opened registry key. */
static int reg_enum_vals(struct reg *g)
{
	int r;
	struct key_ent *ke;
	ke = FF_STRUCTPTR(struct key_ent, sib, g->lr);

	for (;;) {

		ffstr name, val;
		ffuint t;
		r = ffwinreg_enum_nextval(&g->e, &name, &val, &t);
		if (r < 0) {
			fcom_syswarnlog("ffwinreg_enum_nextval()", 0);
			break;
		} else if (r > 0) {
			break;
		}
		fcom_dbglog("value: %S", &name);

		g->stat.vals_all++;

		if (0 != reg_find(g, name)
			|| 0 != reg_find(g, val)) {

			g->stat.vals++;

			g->buf.len = 0;
			if (ke->key[0] != '\0') {
				ffvec_addfmt(&g->buf, "%s\\%s\\%S = \"%S\"\n"
					, ke->hkey_name, ke->key, &name, &val);
			} else {
				ffvec_addfmt(&g->buf, "%s\\%S = \"%S\"\n"
					, ke->hkey_name, &name, &val);
			}
			return 1;
		}
	}

	return 0;
}

/**
Return 'data' with key-value pair inside 'output'. */
static int reg_search_next(struct reg *g, ffstr *output)
{
	enum { I_OPEN, I_VALS, I_KEYS, };
	switch (g->search_state) {
	case I_OPEN:
		struct key_ent *ke;
		g->lr = g->lr->next;
		g->lw = g->lr;
		if (g->lr == fflist_sentl(&g->keys))
			return 'done';

		if (g->lr->prev != fflist_sentl(&g->keys)) {
			ke = FF_STRUCTPTR(struct key_ent, sib, g->lr->prev);
			ffmem_free(ke->key);  ke->key = NULL;
			fflist_rm(&g->keys, &ke->sib);
			if (ke->mblk->buf.len == 0) {
				fflist_rm(&g->keys, &ke->mblk->sib);
				ffmbuf_free(ke->mblk);
			}
		}

		ke = FF_STRUCTPTR(struct key_ent, sib, g->lr);
		if (FFWINREG_NULL == (g->key = ffwinreg_open((HKEY)ke->hkey, ke->key, FFWINREG_READONLY))) {
			fcom_syswarnlog("ffwinreg_open(): %s\\%s", ke->hkey_name, ke->key);
			return 0;
		}
		fcom_dbglog("opened key '%s'", ke->key);
		ffwinreg_enum_begin(&g->e, g->key);
		g->search_state = I_VALS;
		// fallthrough

	case I_VALS:
		if (0 != reg_enum_vals(g)) {
			ffstr_setstr(output, &g->buf);
			return 'data';
		}
		ffwinreg_enum_begin(&g->e, g->key);
		g->search_state = I_KEYS;
		// fallthrough

	case I_KEYS:
		if (0 != reg_enum_keys(g)) {
			ffstr_setstr(output, &g->buf);
			return 'data';
		}

		if (g->key != FFWINREG_NULL) {
			ffwinreg_close(g->key);
			g->key = FFWINREG_NULL;
		}
		g->search_state = I_OPEN;
		break;
	}

	return 0;
}
