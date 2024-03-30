/** fcom: sync: scan file tree
2022, Simon Zolin */

struct ent {
	char type; // 'f', 'd'
	ffstr name;
	struct fcom_sync_entry d;
};

struct snapshot {
	ffstr		root_dir;
	fntree_block *root, *parent_blk;
	fntree_cursor cur;
	xxvec		name;
	uint64		total;

	~snapshot() {
		fntree_free_all(this->root);
		ffstr_free(&this->root_dir);
	}

	/** Prepare full file name */
	static void full_name(ffvec *buf, const fntree_entry *e, const fntree_block *b)
	{
		buf->len = 0;
		if (e == NULL)
			return;
		ffstr name = fntree_name(e);
		ffstr path = fntree_path(b);
		if (path.len != 0)
			ffvec_addfmt(buf, "%S%c", &path, FFPATH_SLASH);
		ffvec_addfmt(buf, "%S%Z", &name);
		buf->len--;
	}

	/** Get next file from tree */
	int next(ffstr *name, fntree_entry **e)
	{
		fntree_block *b = this->root;
		fntree_entry *it = fntree_cur_next_r_ctx(&this->cur, &b);
		if (!it) {
			fcom_dbglog("no more input files");
			return 'done';
		}

		ffstr nm = fntree_name(it);
		ffstr path = fntree_path(b);
		this->name.len = 0;
		if (path.len)
			ffvec_addfmt(&this->name, "%S%c", &path, FFPATH_SLASH);
		ffvec_addfmt(&this->name, "%S%Z", &nm);

		ffstr_set(name, this->name.ptr, this->name.len - 1);
		fcom_dbglog("file: '%S'", name);
		*e = it;

		if (this->parent_blk != b) {
			this->parent_blk = b;
			return 'dir ';
		}
		return 0;
	}

	/** Add tree branch */
	int add_dir(fffd fd)
	{
		ffdirscan ds = {};
		uint flags = 0;

#ifdef FF_LINUX
		ds.fd = fd;
		flags = FFDIRSCAN_USEFD;
#endif

		if (ffdirscan_open(&ds, (char*)this->name.ptr, flags)) {
			fcom_syserrlog("ffdirscan_open");
			return -1;
		}
		ffstr path = FFSTR_INITZ((char*)this->name.ptr);
		fntree_block *b = fntree_from_dirscan(path, &ds, sizeof(struct fcom_sync_entry));
		this->total += b->entries;
		fntree_attach((fntree_entry*)this->cur.cur, b);
		fcom_dbglog("added branch '%S' [%u]", &path, b->entries);
		ffdirscan_close(&ds);
		return 0;
	}

	/** Get file attributes
	fd: [output] directory descriptor */
	static int f_info(const char *name, struct fcom_sync_entry *d, fffd *fd)
	{
		int rc = -1;
		fffd f = fffile_open(name, FFFILE_READONLY | FFFILE_NOATIME);
		if (f == FFFILE_NULL) {
			fcom_syserrlog("fffile_open: %s", name);
			return -1;
		}

		xxfileinfo fi;
		if (fffile_info(f, &fi.info)) {
			fcom_syserrlog("fffile_info: %s", name);
			goto end;
		}

		if (fi.dir()) {
			*fd = f;
			f = FFFILE_NULL;
		}

		d->size = fi.size();
		d->mtime = fi.mtime1();
		d->mtime.nsec = (d->mtime.nsec / 1000000) * 1000000;

#ifdef FF_UNIX
		d->unix_attr = fi.attr();
		d->win_attr = (fi.dir()) ? FFFILE_WIN_DIR : 0;
		d->uid = fi.info.st_uid;
		d->gid = fi.info.st_gid;
#else
		d->win_attr = fi.attr();
		d->unix_attr = (fi.dir()) ? FFFILE_UNIX_DIR : 0;
#endif

		// d->crc32 = ;
		rc = 0;

	end:
		if (f != FFFILE_NULL)
			fffile_close(f);
		return rc;
	}

	int scan_next(struct ent *dst)
	{
		fntree_entry *e;
		ffstr name;
		int r = this->next(&name, &e);
		if (r == 'done')
			return r;

		struct fcom_sync_entry *d = (struct fcom_sync_entry*)fntree_data(e);
		ffmem_zero_obj(d);

		fffd fd = FFFILE_NULL;
		if (f_info(name.ptr, d, &fd))
			return 'erro';

		if (dst) {
			dst->type = (fd != FFFILE_NULL) ? 'd' : 'f';
			dst->name = name;
			dst->d = *d;
		}

		if (fd != FFFILE_NULL
			&& this->add_dir(fd))
			return 'erro';

		return r;
	}
};

static void sync_snapshot_free(fcom_sync_snapshot *ss)
{
	if (ss) ss->~snapshot();
	ffmem_free(ss);
}

static fcom_sync_snapshot* sync_scan(ffstr path, uint flags)
{
	fcom_sync_snapshot *ss = ffmem_new(fcom_sync_snapshot);
	ss->root = fntree_create(FFSTR_Z(""));
	if (!fntree_add(&ss->root, path, sizeof(struct fcom_sync_entry)))
		goto end;

	ffstr_dupstr(&ss->root_dir, &path);

	for (;;) {
		int r = ss->scan_next(NULL);
		if (r == 'done')
			break;
		else if (r == 'erro')
			continue;
	}

	return ss;

end:
	sync_snapshot_free(ss);
	return NULL;
}
