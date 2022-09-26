/** fcom: sync: write snapshot file
2022, Simon Zolin */

static void wsnap_destroy(struct sync *s)
{
	ffvec_free(&s->sw.buf);
	if (s->sw.snap != NULL)
		core->file->close(s->sw.snap);
	core->file->destroy(s->sw.snap);
}

/** File header */
static ffstr wsnap_hdr()
{
	ffstr s = FFSTR_INITZ("# fcom file tree snapshot\r\n\r\n");
	return s;
}

/** Branch header */
static void wsnap_bhdr(ffvec *buf, ffstr dirname)
{
	ffvec_addfmt(buf, "b \"%S\" {\r\n\tv 1\r\n", &dirname);
}

/** File entry */
static void wsnap_ent_serialize(ffvec *buf, const struct ent *e)
{
	const struct entdata *d = &e->d;
	ffdatetime dt;
	fftime_split1(&dt, &d->mtime);

	char date_s[16];
	int n = fftime_tostr1(&dt, date_s, sizeof(date_s), FFTIME_DATE_YMD);
	ffstr date = FFSTR_INITN(date_s, n);

	char time_s[16];
	n = fftime_tostr1(&dt, time_s, sizeof(time_s), FFTIME_HMS_MSEC);
	ffstr time = FFSTR_INITN(time_s, n);

	// TODO escape
	// f|d "file" size unixattr/winattr uid:gid yyyy-mm-dd+hh:mm:ss.msc crc32
	ffvec_addfmt(buf, "\t%c \"%S\" %U %xu/%xu %u:%u %S+%S %u\r\n"
		, e->type, &e->name
		, d->size, d->unixattr, d->winattr, d->uid, d->gid, &date, &time, d->crc32);
}

/** Branch footer */
static ffstr wsnap_bftr()
{
	ffstr s = FFSTR_INITZ("}\r\n\r\n");
	return s;
}

static void wsnap_init(struct sync *s)
{
	ffsize cap = (s->cmd->buffer_size != 0) ? s->cmd->buffer_size : 64*1024;
	ffvec_alloc(&s->sw.buf, cap, 1);
}

static int wsnap_process(struct sync *s)
{
	for (;;) {
		ffstr data;
		if (!s->hdr) {
			s->hdr = 1;
			wsnap_init(s);
			data = wsnap_hdr();

			struct fcom_file_conf fc = {};
			fc.buffer_size = s->cmd->buffer_size;
			s->sw.snap = core->file->create(&fc);

			uint flags = fcom_file_cominfo_flags_o(s->cmd);
			flags |= FCOM_FILE_WRITE;
			int r = core->file->open(s->sw.snap, s->cmd->output.ptr, flags);
			if (r == FCOM_FILE_ERR) return 0xbad;

		} else if (s->sw.bftr) {
			s->sw.bftr = 0;
			data = wsnap_bftr();

		} else if (s->sw.bhdr) {
			s->sw.bhdr = 0;
			wsnap_bhdr(&s->sw.buf, s->dir);
			ffstr_setstr(&data, &s->sw.buf);
			s->sw.buf.len = 0;

		} else if (s->ent_ready) {
			s->ent_ready = 0;
			wsnap_ent_serialize(&s->sw.buf, &s->ent);
			ffstr_setstr(&data, &s->sw.buf);
			s->sw.buf.len = 0;

		} else {
			return 0;
		}

		int r = core->file->write(s->sw.snap, data, -1);
		if (r == FCOM_FILE_ERR) return 0xbad;
	}
}