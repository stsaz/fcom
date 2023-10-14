/** fcom: copy: write output file
2022, Simon Zolin */

static int output_init(struct copy *c)
{
	struct fcom_file_conf fc = {};
	fc.buffer_size = c->cmd->buffer_size;
	fc.n_buffers = 1;
	c->out = core->file->create(&fc);
	return 0;
}

static void output_close(struct copy *c)
{
	if (c->del_on_close) {
		if (!c->write_into) {
			core->file->delete(c->oname_tmp, 0);
		}
		core->file->delete(c->oname, 0);
	}
	core->file->destroy(c->out);
	ffmem_free0(c->oname);
	ffmem_free0(c->oname_tmp);
}

static void output_trash_complete(void *param, int result)
{
	struct copy *c = param;
	copy_run(c);
}

static int output_fin(struct copy *c)
{
	switch (c->ostate) {
	case 0:
		if (1)
			core->file->attr_set(c->out, fffileinfo_attr(&c->fi));
		core->file->close(c->out);
		c->del_on_close = 0;

		if (!c->cmd->stdout && !c->write_into && 0 != fffileinfo_size(&c->ofi)) {

			fcom_cominfo *ci = core->com->create();
			ci->operation = ffsz_dup("trash");

			ffstr *p = ffvec_pushT(&ci->input, ffstr);
			char *sz = ffsz_dup(c->oname);
			ffstr_setz(p, sz);

			ci->test = c->cmd->test;
			ci->buffer_size = c->cmd->buffer_size;

			ci->on_complete = output_trash_complete;
			ci->opaque = c;
			core->com->run(ci);
			fcom_dbglog("copy: trash: %s", c->oname);
			c->ostate = 1;
			return 123;
		}
		// fallthrough

	case 1:
		if (!c->cmd->stdout
			&& 0 != fffile_rename(c->oname_tmp, c->oname)) {
			fcom_syserrlog("fffile_rename: %s -> %s", c->oname_tmp, c->oname);
			return 0xbad;
		}
		break;
	}

	return 0;
}

static void output_reset(struct copy *c)
{
	c->ostate = 0;
	c->total = 0;
	c->out_off = 0;
	ffmem_free0(c->oname);
	ffmem_free0(c->oname_tmp);
}

/** Prepare output file name */
static char* out_name(struct copy *c, ffstr in, ffstr base)
{
	char *s;
	if (c->cmd->output.len != 0 && c->cmd->chdir.len == 0) {
		s = ffsz_dupstr(&c->cmd->output);

	} else if (c->cmd->output.len != 0 && c->cmd->chdir.len != 0) {
		s = ffsz_allocfmt("%S%c%S"
			, &c->cmd->chdir, FFPATH_SLASH, &c->cmd->output);

	} else if (c->cmd->output.len == 0 && c->cmd->chdir.len != 0) {
		/*
		`in -C out`: "in" -> "out/in"
		`d -R -C out`: "d/f" -> "out/d/f"
		`/tmp/d -R -C out`: "/tmp/d/f" -> "out/d/f"
		*/
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
	uint flags = FCOM_FILE_WRITE;
	if (c->verify)
		flags = FCOM_FILE_READWRITE | FCOM_FILE_DIRECTIO;
	flags |= fcom_file_cominfo_flags_o(c->cmd);

	int r = core->file->open(c->out, c->oname, flags);
	if (r == FCOM_FILE_ERR)
		return 0xbad;

	if (!c->cmd->stdout) {
		r = core->file->info(c->out, &c->ofi);
		if (r == FCOM_FILE_ERR)
			return 0xbad;
		if (fffile_isdir(fffileinfo_attr(&c->ofi))) {
			fcom_errlog("output file is an existing directory. Use '-C DIR' to copy files into this directory.");
			return 0xbad;
		}

		if (c->update) {
			fftime imt = fffileinfo_mtime(&c->fi);
			fftime omt = fffileinfo_mtime(&c->ofi);
			if (fftime_cmp(&imt, &omt) <= 0) {
				fcom_dbglog("--update: skipping file");
				return 'skip';
			}
		}

		if (!c->write_into) {
			core->file->close(c->out);
			r = core->file->open(c->out, c->oname_tmp, flags);
			if (r == FCOM_FILE_ERR)
				return 0xbad;

			fffileinfo fi;
			r = core->file->info(c->out, &fi);
			if (r == FCOM_FILE_ERR)
				return 0xbad;
			if (0 != fffileinfo_size(&fi)) {
				fcom_errlog("temp file %s already exists - check & delete it manually", c->oname_tmp);
				return 0xbad;
			}
		}
	}

	c->del_on_close = !c->cmd->stdout && !c->cmd->test;
	return 0;
}

static int output_write(struct copy *c, ffstr input)
{
	int r = core->file->write(c->out, input, c->out_off);
	if (r == FCOM_FILE_ERR) return 0xbad;
	c->out_off += input.len;
	c->total += input.len;
	return 0;
}
