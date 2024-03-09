/** fcom: sync: write snapshot file
2022, Simon Zolin */

/** File header */
static ffstr wsnap_hdr()
{
	return FFSTR_Z("# fcom file tree snapshot\r\n\r\n");
}

/** Branch header */
static void wsnap_bhdr(xxvec& buf, ffstr dirname)
{
	buf.add_f("b \"%S\" {\r\n\tv 1\r\n", &dirname);
}

/** File entry */
static void wsnap_ent_serialize(xxvec& buf, const struct ent& e)
{
	const struct fcom_sync_entry *d = &e.d;
	ffdatetime dt;
	fftime_split1(&dt, &d->mtime);

	char date_s[16];
	uint n = fftime_tostr1(&dt, date_s, sizeof(date_s), FFTIME_DATE_YMD);
	ffstr date = FFSTR_INITN(date_s, n);

	char time_s[16];
	n = fftime_tostr1(&dt, time_s, sizeof(time_s), FFTIME_HMS_MSEC);
	ffstr time = FFSTR_INITN(time_s, n);

	// TODO escape
	// f|d "file" size unix_attr/win_attr uid:gid yyyy-mm-dd+hh:mm:ss.msc crc32
	buf.add_f("\t%c \"%S\"\t%U\t%xu/%xu\t%u:%u\t%S+%S\t%u\r\n"
		, e.type, &e.name
		, d->size, d->unix_attr, d->win_attr, d->uid, d->gid, &date, &time, d->crc32);
}

/** Branch footer */
static ffstr wsnap_bftr()
{
	ffstr s = FFSTR_INITZ("}\r\n\r\n");
	return s;
}

void wsnap::init(struct sync *s)
{
	ffsize cap = (s->cmd->buffer_size != 0) ? s->cmd->buffer_size : 64*1024;
	ffvec_alloc(&this->buf, cap, 1);
}

int wsnap::process(struct sync *s, const struct ent& e)
{
	for (;;) {
		ffstr data;
		if (!s->hdr) {
			s->hdr = 1;
			this->init(s);
			data = wsnap_hdr();

			struct fcom_file_conf fc = {};
			fc.buffer_size = s->cmd->buffer_size;
			this->snap.create(&fc);

			uint flags = fcom_file_cominfo_flags_o(s->cmd);
			flags |= FCOM_FILE_WRITE;
			int r = this->snap.open(s->cmd->output.ptr, flags);
			if (r == FCOM_FILE_ERR) return 0xbad;

		} else if (this->bftr) {
			this->bftr = 0;
			data = wsnap_bftr();

		} else if (this->bhdr) {
			this->bhdr = 0;
			wsnap_bhdr(this->buf, s->dir);
			ffstr_setstr(&data, &this->buf);
			this->buf.len = 0;

		} else if (s->ent_ready) {
			s->ent_ready = 0;
			wsnap_ent_serialize(this->buf, e);
			ffstr_setstr(&data, &this->buf);
			this->buf.len = 0;

		} else {
			return 0;
		}

		int r = this->snap.write(data, -1);
		if (r == FCOM_FILE_ERR) return 0xbad;
	}
}