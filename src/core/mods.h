/** fcom: core: modules
2022, Simon Zolin */

#include <ffsys/dylib.h>

struct mod {
	char *name;
	const char *alias_of;
	ffdl dl;
	struct fcom_module *mod;
};

static int modmap_keyeq(void *opaque, const void *key, ffsize keylen, void *val)
{
	struct mod *m = val;
	return !ffs_cmpz(key, keylen, m->name);
}

void mods_init()
{
	ffmap_init(&com.mods, modmap_keyeq);

	struct alias {
		const char *name, *alias_of;
	};
	static const struct alias aliases[] = {
		{ "un7z",	"7z" },
		{ "ungz",	"gz" },
		{ "uniso",	"iso" },
		{ "untar",	"tar" },
		{ "unxz",	"xz" },
		{ "unzip",	"zip" },
		{ "unzst",	"zst" },
	};

	const struct alias *it;
	FF_FOREACH(aliases, it) {
		struct mod *m = ffmem_new(struct mod);
		m->name = ffsz_dup(it->name);
		m->alias_of = it->alias_of;
		ffmap_add(&com.mods, it->name, ffsz_len(it->name), m);
	}
}

static void mod_free(struct mod *m)
{
	if (m->mod != NULL)
		m->mod->destroy();
	if (m->dl != NULL) {
		ffdl_close(m->dl);
		dbglog("closed module %s", m->name);
	}
	ffmem_free(m->name);
	ffmem_free(m);
}

void mods_free()
{
	struct _ffmap_item *it;
	FFMAP_WALK(&com.mods, it) {
		if (!_ffmap_item_occupied(it))
			continue;
		struct mod *m = it->val;
		mod_free(m);
	}
	ffmap_free(&com.mods);
}

static struct mod* mod_load(ffstr modname)
{
	struct mod *m = ffmem_new(struct mod);
	char *name = ffsz_allocfmt("ops%c%S." FFDL_EXT, FFPATH_SLASH, &modname);
	char *fn = core->path(name);

	dbglog("loading '%s'...", fn);
	if (NULL == (m->dl = ffdl_open(fn, FFDL_SELFDIR))) {
		errlog("dl open: %s: %s", fn, ffdl_errstr());
		goto err;
	}

	if (NULL == (m->mod = ffdl_addr(m->dl, "fcom_module"))) {
		errlog("dl addr '%s': %s: %s", "fcom_module", fn, ffdl_errstr());
		goto err;
	}

	if (m->mod->ver_core != FCOM_CORE_VER) {
		errlog("module %s is built for another fcom version", fn);
		goto err;
	}

	m->name = ffsz_dupstr(&modname);
	dbglog("initializing module '%s'...", m->name);
	m->mod->init(core);
	dbglog("initialized module '%s' v%s", m->name, m->mod->version);
	ffmem_free(name);
	ffmem_free(fn);
	return m;

err:
	ffmem_free(name);
	ffmem_free(fn);
	mod_free(m);
	return NULL;
}

/**
"a" -> "ops/a.so".addr("a")
"a.b" -> "ops/a.so".addr("b")
*/
const void* com_provide(const char *operation, uint flags)
{
	ffstr op = FFSTR_INITZ(operation), modname, opname;
	if (-1 == ffstr_splitby(&op, '.', &modname, &opname))
		opname = modname;

	struct mod *m;
	for (;;) {
		if (NULL == (m = ffmap_find(&com.mods, modname.ptr, modname.len, NULL))) {
			if (NULL == (m = mod_load(modname)))
				return NULL;
			ffmap_add(&com.mods, modname.ptr, modname.len, m);
		} else {
			if (m->alias_of != NULL) {
				fcom_dbglog("alias: %S -> %s", &modname, m->alias_of);
				ffstr_setz(&modname, m->alias_of);
				continue;
			}
		}
		break;
	}

	dbglog("requesting operation '%s' from module '%s'...", opname.ptr, m->name);
	const void *opif = m->mod->provide_op(opname.ptr);
	if (opif == NULL) {
		if (flags & FCOM_COM_PROVIDE_PRIM) {
			errlog("'%s': no registered operation '%s'", m->name, opname.ptr);
			return NULL;
		}
		if (NULL == (opif = ffdl_addr(m->dl, opname.ptr))) {
			errlog("'%s': no registered operation '%s': %s", m->name, opname.ptr, ffdl_errstr());
			return NULL;
		}
	}

	return opif;
}
