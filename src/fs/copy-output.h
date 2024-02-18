/** fcom: copy: write output file
2022, Simon Zolin */

static int output_init(struct copy *c)
{
	struct fcom_file_conf fc = {};
	fc.buffer_size = c->cmd->buffer_size;
	fc.n_buffers = 1;
	c->o.f.create(&fc);
	return 0;
}

static void output_close(struct copy *c)
{
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
			c->o.f.attr(c->fi.attr());
		c->o.f.close();
		c->o.del_on_close = 0;

		if (!c->cmd->stdout && !c->write_into && 0 != c->o.fi.size()) {

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
	char *s;
	if (c->cmd->output.len != 0 && c->cmd->chdir.len == 0) {
		s = ffsz_dupstr(&c->cmd->output);

	} else if (c->cmd->output.len != 0 && c->cmd->chdir.len != 0) {
		s = ffsz_allocfmt("%S%c%S"
			, &c->cmd->chdir, FFPATH_SLASH, &c->cmd->output);

	} else if (c->cmd->output.len == 0 && c->cmd->chdir.len != 0) {
		ffstr name;
		if (base.len == 0)
			base = in;
		ffpath_splitpath(base.ptr, base.len, NULL, &name);
		if (name.len != 0)
			ffstr_shift(&in, name.ptr - base.ptr);
		s = ffsz_allocfmt("%S%c%S"
			, &c->cmd->chdir, FFPATH_SLASH, &in);

	} else {
		fcom_errlog("please use --output or --chdir to set destination");
		return NULL;
	}

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
		c->o.name_tmp = ffsz_allocfmt("%s.fcomtmp", c->o.name);
	}

	if (c->fi.dir()) {
		int r = core->file->dir_create(c->o.name, FCOM_FILE_DIR_RECURSIVE);
		if (r == FCOM_FILE_ERR) return 0xbad;
		return 'skip';
	}

	if (c->update
		&& !fffile_info_path(c->o.name, &c->o.fi.info)) {

		if (c->o.fi.dir()) {
			fcom_errlog("output file is an existing directory. Use '-C DIR' to copy files into this directory.");
			return 0xbad;
		}

		if (fftime_cmp(&xxrval(c->fi.mtime1()), &xxrval(c->o.fi.mtime1())) <= 0) {
			fcom_dbglog("--update: target file is of the same date or newer; skipping");
			return 'skip';
		}
	}

	uint flags = FCOM_FILE_WRITE;
	if (c->verify)
		flags = FCOM_FILE_READWRITE | FCOM_FILE_DIRECTIO;
	flags |= fcom_file_cominfo_flags_o(c->cmd);

	int r = c->o.f.open(c->o.name, flags);
	if (r == FCOM_FILE_ERR)
		return 0xbad;

	if (!c->cmd->stdout) {
		r = c->o.f.info(&c->o.fi);
		if (r == FCOM_FILE_ERR)
			return 0xbad;

		if (!c->write_into) {
			c->o.f.close();
			r = c->o.f.open(c->o.name_tmp, flags);
			if (r == FCOM_FILE_ERR)
				return 0xbad;

			xxfileinfo fi;
			r = c->o.f.info(&fi);
			if (r == FCOM_FILE_ERR)
				return 0xbad;
			if (0 != fi.size()) {
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
	int r = c->o.f.write(input, c->o.off);
	if (r == FCOM_FILE_ERR) return 0xbad;
	c->o.off += input.len;
	c->o.total += input.len;
	return 0;
}
