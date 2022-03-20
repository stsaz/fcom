/** fcom: core: input file names
2021, Simon Zolin
*/

ffbool file_matches(comm *c, const char *full_fn, ffbool dir);
int dir_scan(comm *c, const char *name);

void args_free(comm *c)
{
	struct dir **d;
	FFSLICE_WALK(&c->dirs, d) {
		dir_free(*d);
	}
	ffvec_free(&c->dirs);
	ffvec_free(&c->curname);
}

/** Add an argument. */
int com_arg_add(fcom_cmd *_c, const ffstr *arg, uint flags)
{
	dbglog(0, "adding arg '%S'", arg);
	comm *c = FF_GETPTR(comm, cmd, _c);
	if (c->curdir == NULL) {
		ffstr empty = {};
		c->curdir = dir_create(empty, NULL);
	}
	c->curdir = dir_add(c->curdir, *arg);
	return 0;
}

/** Get next argument. */
char* com_arg_next(fcom_cmd *_c, uint flags)
{
	comm *c = FF_GETPTR(comm, cmd, _c);
	fffileinfo fi;
	const char *name;
	unsigned fi_valid = 0;

	if (c->curdir == NULL) {
		name = NULL;
		goto done;
	}

	for (;;) {

		if (c->cmd.recurse && c->curname.len != 0) {
			name = c->curname.ptr;
			ffuint attr = 0;
			if (flags & FCOM_CMD_ARG_USECURFILE) {
				flags &= ~FCOM_CMD_ARG_USECURFILE;
				attr = _c->input.attr;
			} else {
				if (fi_valid || 0 == fffile_info_path(name, &fi))
					attr = fffileinfo_attr(&fi);
				else
					syserrlog("fffile_info_path: %s", name);
			}
			if (fffile_isdir(attr) && file_matches(c, name, 1))
				dir_scan(c, name);
		}

		fi_valid = 0;
		c->curname.len = 0;

		name = dir_next(c->curdir, !!(flags & FCOM_CMD_ARG_PEEK));
		if (name == NULL) {
			if (dir_parent(c->curdir) == NULL)
				break;
			c->curdir = dir_parent(c->curdir);
			continue;
		}

		if (dir_path(c->curdir)[0] == '\0')
			ffvec_addfmt(&c->curname, "%s%Z", name);
		else
			ffvec_addfmt(&c->curname, "%s/%s%Z", dir_path(c->curdir), name);
		name = c->curname.ptr;

		if (!file_matches(c, name, 0)) {

		} else if ((flags & FCOM_CMD_ARG_FILE)
			&& 0 == fffile_info_path(name, &fi)
			&& (fi_valid = 1)) {

			if (fffile_isdir(fffileinfo_attr(&fi)))
				continue;
			c->curname.len = 0; // don't enter 'cmd.recurse' branch on the next iteration
			break;

		} else {
			break;
		}

		if (flags & FCOM_CMD_ARG_PEEK)
			dir_next(c->curdir, 0);
	}

done:
	dbglog(0, "arg_next: %s", name);
	return (char*)name;
}

/**
'include' filter matches files only.
'exclude' filter matches files & directories (names or full paths).
Return TRUE if filename matches user's filename wildcards. */
ffbool file_matches(comm *c, const char *full_fn, ffbool dir)
{
	const ffstr *wc;
	ffbool ok = 1;
	ffstr fn;
	ffstr_setz(&fn, full_fn);

	if (!dir) {
		ok = (c->cmd.include_files.len == 0);
		FFSLICE_WALK(&c->cmd.include_files, wc) {

			if (0 == ffs_wildcard(wc->ptr, wc->len, fn.ptr, fn.len, FFS_WC_ICASE)
				|| ffpath_match(&fn, wc, FFPATH_CASE_ISENS)) {
				ok = 1;
				break;
			}
		}
		if (!ok)
			return 0;
	}

	FFSLICE_WALK(&c->cmd.exclude_files, wc) {

		if (0 == ffs_wildcard(wc->ptr, wc->len, fn.ptr, fn.len, FFS_WC_ICASE)
			|| ffpath_match(&fn, wc, FFPATH_CASE_ISENS)) {
			return 0;
		}
	}

	return ok;
}

/** List directory contents and add its filenames to the arguments list. */
int dir_scan(comm *c, const char *name)
{
	ffdirscan ds = {};
	const char *fn;
	int r = -1;

	dbglog(0, "opening directory %s", name);

	uint flags = 0;
#ifdef FF_LINUX
	if (c->cmd.input_fd != FFFILE_NULL) {
		ds.fd = c->cmd.input_fd;
		flags |= FFDIRSCAN_USEFD;
		c->cmd.input_fd = FFFILE_NULL;
	}
#endif

	if (0 != ffdirscan_open(&ds, name, flags)) {
		syserrlog("ffdirscan_open: %s", name);
		return -1;
	}

	struct dir *d = NULL;

	while (NULL != (fn = ffdirscan_next(&ds))) {

		if (d == NULL) {
			ffstr path;
			ffstr_setz(&path, name);
			if (ffpath_slash(path.ptr[path.len-1]))
				path.len--; // rm trailing slash
			d = dir_create(path, c->curdir);
		}

		ffstr s;
		ffstr_setz(&s, fn);
		d = dir_add(d, s);
	}

	r = 0;
	ffdirscan_close(&ds);
	if (d != NULL) {
		*ffvec_pushT(&c->dirs, struct dir*) = d;
		c->curdir = d;
	}
	return r;
}
