/** fcom: copy: write output file
2022, Simon Zolin */

static int output_init(struct copy *c)
{
	struct fcom_file_conf fc = {};
	fc.buffer_size = c->cmd->buffer_size;
	fc.n_buffers = 1;
	c->o.f = core->file->create(&fc);
	return 0;
}

static void output_close(struct copy *c)
{
	core->file->destroy(c->o.f);
	if (c->o.del_on_close) {
		if (!c->write_into) {
			core->file->del(c->o.name_tmp, 0);
		}
		core->file->del(c->o.name, 0);
	}
	ffmem_free0(c->o.name);
	ffmem_free0(c->o.name_tmp);
}

static void output_trash_complete(void *param, int result)
{
	struct copy *c = (struct copy*)param;
	copy_run(c);
}

static int output_fin(struct copy *c)
{
	switch (c->o.state) {
	case 0:
		if (1)
			core->file->attr_set(c->o.f, fffileinfo_attr(&c->fi));
		core->file->close(c->o.f);
		c->o.del_on_close = 0;

		if (!c->cmd->stdout && !c->write_into && 0 != fffileinfo_size(&c->o.fi)) {

			fcom_cominfo *ci = core->com->create();
			ci->operation = ffsz_dup("trash");

			ffstr *p = ffvec_pushT(&ci->input, ffstr);
			char *sz = ffsz_dup(c->o.name);
			ffstr_setz(p, sz);

			ci->test = c->cmd->test;
			ci->buffer_size = c->cmd->buffer_size;

			ci->on_complete = output_trash_complete;
			ci->opaque = c;
			fcom_dbglog("copy: trash: %s", c->o.name);
			c->o.state = 1;
			core->com->run(ci);
			return 'asyn';
		}
		// fallthrough

	case 1:
		if (!c->cmd->stdout
			&& !c->write_into
			&& 0 != fffile_rename(c->o.name_tmp, c->o.name)) {
			fcom_syserrlog("fffile_rename: %s -> %s", c->o.name_tmp, c->o.name);
			return 0xbad;
		}
		break;
	}

	return 0;
}

static void output_reset(struct copy *c)
{
	c->o.state = 0;
	c->o.total = 0;
	c->o.off = 0;
	ffmem_free0(c->o.name);
	ffmem_free0(c->o.name_tmp);
}

/** Prepare output file name
* `file -o out`				: "file" -> "out"
* `file -C odir -o oname`	: "file" -> "odir/oname"
* `file -C odir`			: "file" -> "odir/file"
* `dir -C odir`				: "dir/file" -> "odir/dir/file"
* `/tmp/dir -C odir`		: "/tmp/dir/file" -> "odir/dir/file" (base="/tmp/dir")
*/
static char* out_name(struct copy *c, ffstr in, ffstr base)
{
	char *s = NULL;
	if (c->cmd->output.len && !c->cmd->chdir.len) {
		s = ffsz_dupstr(&c->cmd->output);

	} else if (c->cmd->output.len && c->cmd->chdir.len) {
		s = ffsz_allocfmt("%S%c%S"
			, &c->cmd->chdir, FFPATH_SLASH, &c->cmd->output);

	} else if (!c->cmd->output.len && c->cmd->chdir.len) {
		ffstr name;
		if (!base.len)
			base = in;
		ffpath_splitpath(base.ptr, base.len, NULL, &name);
		if (name.len)
			ffstr_shift(&in, name.ptr - base.ptr);
		s = ffsz_allocfmt("%S%c%S"
			, &c->cmd->chdir, FFPATH_SLASH, &in);
	}

	uint f = 0;
#ifdef FF_WIN
	f = FFPATH_FORCE_BACKSLASH;
#endif
	int r = ffpath_normalize(s, -1, s, ffsz_len(s), f);
	FF_ASSERT(r >= 0);
	s[r] = '\0';

	fcom_dbglog("output file name: %s", s);
	return s;
}

static int output_open(struct copy *c)
{
	if (!c->cmd->stdout) {
		ffmem_free0(c->o.name);
		ffmem_free0(c->o.name_tmp);
		if (!(c->o.name = out_name(c, c->name, c->basename)))
			return 0xbad;
		if (!c->write_into)
			c->o.name_tmp = ffsz_allocfmt("%s.fcomtmp", c->o.name);
	}

	if (fffile_isdir(fffileinfo_attr(&c->fi))) {
		int r = core->file->dir_create(c->o.name, FCOM_FILE_DIR_RECURSIVE);
		if (r == FCOM_FILE_ERR) return 0xbad;
		return 'skip';
	}

	// Copy symlink
#ifdef FF_UNIX
	if ((fffileinfo_attr(&c->fi) & FFFILEATTR_UNIX_TYPEMASK) == FFFILEATTR_UNIX_LINK) {
		char target[4096];
		int r = readlink(c->iname, target, sizeof(target) - 1);
		if (r < 0 || r == sizeof(target) - 1) {
			if (r < 0)
				fcom_syserrlog("readlink: %s", c->iname);
			else
				fcom_errlog("readlink truncation: %s", c->iname);
			return 0xbad;
		}
		target[r] = '\0';
		if (FCOM_FILE_ERR == core->file->slink(target, c->o.name, fcom_file_cominfo_flags_o(c->cmd)))
			return 0xbad;
		return 'skip';
	}
#endif

	if (c->update
		&& !fffile_info_path(c->o.name, &c->o.fi)) {

		if (fffile_isdir(fffileinfo_attr(&c->o.fi))) {
			fcom_errlog("%s: target path is an existing directory", c->o.name);
			return 0xbad;
		}

		if (c->replace_date) {
			fftime t = fffileinfo_mtime(&c->fi);
			if (fffile_set_mtime_path(c->o.name, &t)) {
				fcom_syserrlog("fffile_set_mtime_path: %s", c->o.name);
				return 0xbad;
			}
			fcom_verblog("replace date: %s", c->o.name);
			return 'skip';
		}

		if (fftime_cmp_val(fffileinfo_mtime1(&c->fi), fffileinfo_mtime1(&c->o.fi)) <= 0) {
			fcom_dbglog("--update: target file is of the same date or newer; skipping");
			return 'skip';
		}
	}

	uint flags = FCOM_FILE_WRITE;
	if (c->verify)
		flags = FCOM_FILE_READWRITE | FCOM_FILE_DIRECTIO;
	flags |= fcom_file_cominfo_flags_o(c->cmd);

	int r = core->file->open(c->o.f, c->o.name, flags);
	if (r == FCOM_FILE_ERR)
		return 0xbad;

	if (!c->cmd->stdout) {
		r = core->file->info(c->o.f, &c->o.fi);
		if (r == FCOM_FILE_ERR)
			return 0xbad;

		if (!c->write_into) {
			core->file->close(c->o.f);
			r = core->file->open(c->o.f, c->o.name_tmp, flags);
			if (r == FCOM_FILE_ERR)
				return 0xbad;

			fffileinfo fi;
			r = core->file->info(c->o.f, &fi);
			if (r == FCOM_FILE_ERR)
				return 0xbad;
			if (0 != fffileinfo_size(&fi)) {
				fcom_errlog("temp file %s already exists - check & delete it manually", c->o.name_tmp);
				return 0xbad;
			}
		}
	}

	c->o.del_on_close = !c->cmd->stdout && !c->cmd->test;
	return 0;
}

static int output_write(struct copy *c, ffstr input)
{
	int r = core->file->write(c->o.f, input, c->o.off);
	if (r == FCOM_FILE_ERR) return 0xbad;
	c->o.off += input.len;
	c->o.total += input.len;
	return 0;
}
