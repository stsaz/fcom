/** fcom: core modules
2021, Simon Zolin */

static const void* core_iface(const char *nm)
{
	struct iface *pif;
	FFSLICE_WALK(&g->ifaces, pif) {
		if (ffsz_eq(pif->name, nm))
			return pif->iface;
	}

	// get hidden (e.g. not specified in fcom.conf) interface from the loaded module

	ffstr mod_name, if_name;
	ffs_split2by(nm, ffsz_len(nm), '.', &mod_name, &if_name);

	struct mod *m;
	FFSLICE_WALK(&g->mods, m) {
		if (ffstr_eqz(&mod_name, m->name)) {
			const void *r = m->mod->iface(if_name.ptr);
			if (r != NULL)
				return r;
			break;
		}
	}

	errlog("no such interface: %s", nm);
	return NULL;
}

static void mod_destroy(struct mod *m)
{
	FF_SAFECLOSE(m->dl, NULL, ffdl_close);
	ffmem_safefree(m->name);
}

static void mods_destroy(void)
{
	struct mod *m;
	FFSLICE_WALK(&g->mods, m) {
		mod_destroy(m);
	}
	ffarr_free(&g->mods);
}

static int mods_sig(uint sig)
{
	int r = 0;
	struct mod *m;
	FFSLICE_WALK(&g->mods, m) {
		if (0 != m->mod->sig(sig))
			r = 1;
	}
	return r;
}

static struct mod* mod_find(const ffstr *soname)
{
	struct mod *m;
	FFSLICE_WALK(&g->mods, m) {
		if (ffstr_eqz(soname, m->name))
			return m;
	}
	return NULL;
}

static struct mod* mod_load(const ffstr *soname)
{
	ffdl dl = NULL;
	fcom_getmod_t getmod;
	struct mod *m, *rc = NULL;
	char *fn = NULL;

	if (NULL == (m = ffarr_pushgrowT(&g->mods, 16, struct mod)))
		goto fail;
	ffmem_tzero(m);
	if (NULL == (m->name = ffsz_alcopy(soname->ptr, soname->len)))
		goto fail;

	if (ffstr_eqcz(soname, "core")) {
		getmod = &coremod_getmod;

	} else {
		if (NULL == (fn = ffsz_alfmt("%Smod%c%S." FFDL_EXT, &g->rootdir, FFPATH_SLASH, soname)))
			goto fail;

		dbglog(0, "loading module %s", fn);
		if (NULL == (dl = ffdl_open(fn, FFDL_SELFDIR))) {
			errlog("loading %s: %s", fn, ffdl_errstr());
			goto fail;
		}
		if (NULL == (getmod = (void*)ffdl_addr(dl, FCOM_MODFUNCNAME))) {
			errlog("resolving '%s' from %s: %s", FCOM_MODFUNCNAME, fn, ffdl_errstr());
			goto fail;
		}
	}

	if (NULL == (m->mod = getmod(core)))
		goto fail;

	if (0 != m->mod->sig(FCOM_SIGINIT))
		goto fail;
	m->dl = dl;
	rc = m;

fail:
	if (rc == NULL) {
		mod_destroy(m);
		g->mods.len--;
		FF_SAFECLOSE(dl, NULL, ffdl_close);
	}
	ffmem_safefree(fn);
	return rc;
}

static int mod_add(const ffstr *name, ffconf_scheme *cs)
{
	ffstr soname, iface;
	char fn[128];
	struct mod *m;
	struct iface *pif = NULL;

	ffs_split2by(name->ptr, name->len, '.', &soname, &iface);
	if (soname.len == 0 || iface.len == 0) {
		fferr_set(EINVAL);
		goto fail;
	}

	if (NULL == (m = mod_find(&soname))) {
		if (NULL == (m = mod_load(&soname)))
			goto fail;
	}

	if (0 == ffs_fmt(fn, fn + sizeof(fn), "%S%Z", &iface))
		goto fail;

	if (cs != NULL) {
		if (m->mod->conf == NULL)
			goto fail;
		if (0 != m->mod->conf(fn, cs))
			goto fail;
	}

	if (NULL == (pif = ffarr_pushgrowT(&g->ifaces, 16, struct iface)))
		goto fail;
	ffmem_tzero(pif);

	if (NULL == (pif->iface = m->mod->iface(fn)))
		goto fail;

	if (NULL == (pif->name = ffsz_alcopy(name->ptr, name->len)))
		goto fail;
	pif->m = m;
	return 0;

fail:
	return -1;
}
