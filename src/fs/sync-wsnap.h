/** fcom: sync: write snapshot file
2022, Simon Zolin */

/** File header */
static ffstr wsnap_hdr()
{
	return FFSTR_Z("# fcom file tree snapshot\r\n\r\n");
}

/** Branch header */
static void wsnap_bhdr(ffvecxx& buf, ffstr dirname)
{
	buf.addf("b \"%S\" {\r\n\tv 1\r\n", &dirname);
}

/** File entry */
static void wsnap_ent_serialize(ffvecxx& buf, const struct ent *e)
{
	const struct entdata *d = &e->d;
	ffdatetime dt;
	fftime_split1(&dt, &d->mtime);

	char date_s[16];
	uint n = fftime_tostr1(&dt, date_s, sizeof(date_s), FFTIME_DATE_YMD);
	ffstr date = FFSTR_INITN(date_s, n);

	char time_s[16];
	n = fftime_tostr1(&dt, time_s, sizeof(time_s), FFTIME_HMS_MSEC);
	ffstr time = FFSTR_INITN(time_s, n);

	// TODO escape
	// f|d "file" size unixattr/winattr uid:gid yyyy-mm-dd+hh:mm:ss.msc crc32
	buf.addf("\t%c \"%S\"\t%U\t%xu/%xu\t%u:%u\t%S+%S\t%u\r\n"
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
			s->sw.snap.create(&fc);

			uint flags = fcom_file_cominfo_flags_o(s->cmd);
			flags |= FCOM_FILE_WRITE;
			int r = s->sw.snap.open(s->cmd->output.ptr, flags);
			if (r == FCOM_FILE_ERR) return 0xbad;

		} else if (s->sw.bftr) {
			s->sw.bftr = 0;
			data = wsnap_bftr();

		} else if (s->sw.bhdr) {
			s->sw.bhdr = 0;
			wsnap_bhdr(s->sw.buf, s->dir);
			ffstr_setstr(&data, &s->sw.buf);
			s->sw.buf.len = 0;

		} else if (s->ent_ready) {
			s->ent_ready = 0;
			wsnap_ent_serialize(s->sw.buf, &s->ent);
			ffstr_setstr(&data, &s->sw.buf);
			s->sw.buf.len = 0;

		} else {
			return 0;
		}

		int r = s->sw.snap.write(data, -1);
		if (r == FCOM_FILE_ERR) return 0xbad;
	}
}