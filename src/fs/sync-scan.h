/** fcom: sync: scan file tree
2022, Simon Zolin */

#include <util/util.h>

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
	ffstr		path, name_segment;
	uint64		total;
	uint		zip_block;
	uint		zip_expand :1;

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
	int next(ffstr *pname, fntree_entry **e)
	{
		fntree_block *b = this->root;
		fntree_entry *it = fntree_cur_next_r_ctx(&this->cur, &b);
		if (!it) {
			fcom_dbglog("no more input files");
			return 'done';
		}

		ffstr nm = fntree_name(it);
		this->path = fntree_path(b);
		this->name.len = 0;
		if (this->path.len)
			ffvec_addfmt(&this->name, "%S%c", &this->path, FFPATH_SLASH);
		ffvec_addfmt(&this->name, "%S%Z", &nm);
		this->name_segment = nm;

		ffstr_set(pname, this->name.ptr, this->name.len - 1);
		fcom_dbglog("file: '%S'", pname);
		*e = it;

		if (this->parent_blk != b) {
			this->parent_blk = b;

			if (this->zip_expand) {
				ffstr ext;
				ffpath_splitpath_str(this->path, NULL, &ext);
				ffpath_splitname_str(ext, NULL, &ext);
				this->zip_block = (ffstr_ieqz(&ext, "zip")
					|| ffstr_ieqz(&ext, "zipx"));
			}

			return 'nblk';
		}
		return 0;
	}

	/** Add tree branch */
	int add_dir(fffd fd)
	{
		ffdirscan ds = {};
		uint flags = 0;

		if (fd != FFFILE_NULL) {
#ifdef FF_LINUX
			ds.fd = fd;
			flags = FFDIRSCAN_USEFD;
#else
			fffile_close(fd);
#endif
		}

		if (ffdirscan_open(&ds, (char*)this->name.ptr, flags)) {
			fcom_syserrlog("ffdirscan_open: %s", this->name.ptr);
			return -1;
		}
		ffstr path = FFSTR_INITZ((char*)this->name.ptr);
		fntree_block *b = fntree_from_dirscan(path, &ds, sizeof(struct fcom_sync_entry));
		this->total += b->entries;
		fntree_attach((fntree_entry*)this->cur.cur, b);
		fcom_dbglog("added branch '%S' with %u files [%U]", &path, b->entries, this->total);
		ffdirscan_close(&ds);
		return 0;
	}

	/** Add .zip tree branch */
	int add_zip(fffd fd, ffstr name)
	{
		int rc = 'erro';
		fcom_unpack_obj *ux = NULL;
		fntree_block *b = fntree_create(name);
		const fcom_unpack_if *uif = (fcom_unpack_if*)core->com->provide("zip.fcom_unzip", 0);
		if (!uif)
			goto end;
		if (!(ux = uif->open_file(fd)))
			goto end;
		for (;;) {
			ffstr fn = uif->next(ux, NULL);
			if (!fn.len)
				break;
			if (*ffstr_last(&fn) == '/')
				fn.len--;
			if (NULL == fntree_add(&b, fn, sizeof(struct fcom_sync_entry))) {
				fntree_free_all(b);
				goto end;
			}
		}
		this->total += b->entries;
		fntree_attach((fntree_entry*)this->cur.cur, b);
		fcom_dbglog("added branch '%S' with %u files", &name, b->entries);
		rc = 0;

	end:
		if (ux)
			uif->close(ux);
		return rc;
	}

	/** Get file attributes */
	static int f_info(struct fcom_sync_entry *d, const xxfileinfo& fi)
	{
		int rc = 'file';
		if (fi.dir())
			rc = 'dir ';

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
		return rc;
	}

	int scan_next(struct ent *dst)
	{
		fntree_entry *e;
		ffstr name, ext;
		int r = this->next(&name, &e);
		if (r == 'done')
			return 'done';

		struct fcom_sync_entry *d = (struct fcom_sync_entry*)fntree_data(e);
		ffmem_zero_obj(d);

		if (this->zip_block) {
			if (dst) {
				dst->type = 'f';
				dst->name = name;
				dst->d = *d;
			}
			return r;
		}

		int r2 = 0;
		fffd fd = FFFILE_NULL;
		xxfileinfo fi;
		if (fffile_info_path(name.ptr, &fi.info)) {
			fcom_syswarnlog("fffile_info_path: %s", name.ptr);
			goto fin;
		}

		r2 = this->f_info(d, fi);
		switch (r2) {
		case 'dir ':
			if (this->add_dir(fd)) {
				fd = FFFILE_NULL;
				r = 'erro';
				goto end;
			}
			fd = FFFILE_NULL;
			break;

		case 'file':
			if (this->zip_expand) {
				ffpath_splitpath_str(name, NULL, &ext);
				ffpath_splitname_str(ext, NULL, &ext);
				if (ffstr_ieqz(&ext, "zip")
					|| ffstr_ieqz(&ext, "zipx")) {

					fd = fffile_open(name.ptr, FFFILE_READONLY | FFFILE_NOATIME);
					if (fd == FFFILE_NULL) {
						fcom_syswarnlog("fffile_open: %s", name.ptr);
						goto fin;
					}

					if (this->add_zip(fd, name)) {
						r = 'erro';
						goto end;
					}
				}
			}
			break;
		}

	fin:
		if (dst) {
			dst->type = (r2 == 'dir ') ? 'd' : 'f';
			dst->name = name;
			dst->d = *d;
		}

	end:
		fffile_close(fd);
		return r;
	}
};

static void sync_snapshot_free(fcom_sync_snapshot *ss)
{
	if (ss) ss->~snapshot();
	ffmem_free(ss);
}

// ""->["/"]
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

static void fntree_entry_set(fntree_entry *dst, const fntree_entry *src, size_t data_len)
{
	fntree_attach(dst, src->children);
	ffmem_copy(fntree_data(dst), fntree_data(src), data_len);
}

/** Combine two directories.
Example:
	1. ""->["/path/a"]      | "/path/b" => ""->["/path"->[a,b]]
	2. ""->["/path"->[a,b]] | "/path/c" => ""->["/path"->[a,b,c]]
*/
static void sync_combine(struct snapshot *s, fntree_entry *e2)
{
	xxstr p1 = s->root_dir, p2 = fntree_path(e2->children), parent, name1, name2;
	fcom_dbglog("sync: combine: %S + %S", &p1, &p2);
	if (ffpath_parent(p1, p2, &parent))
		return;
	ffpath_splitpath_str(p2, NULL, &name2);

	const uint data_len = sizeof(struct fcom_sync_entry);
	if (!parent.equals(p1)) {
		// 1.
		fntree_block *first = fntree_create(parent);

		ffpath_splitpath_str(p1, NULL, &name1);
		fntree_entry *e = fntree_add(&first, name1, data_len);
		fntree_entry_set(e, _fntr_ent_first(s->root), data_len);

		e = fntree_add(&first, name2, data_len);
		fntree_entry_set(e, e2, data_len);

		fntree_block *newroot = fntree_create(FFSTR_Z(""));
		e = fntree_add(&newroot, parent, data_len);
		fntree_attach(e, first);

		s->root = newroot;
		s->root_dir.len = parent.len;
		s->total += 3;

	} else {
		// 2.
		fntree_block **root = &_fntr_ent_first(s->root)->children;
		fntree_entry *e = fntree_add(root, name2, data_len);
		fntree_entry_set(e, e2, data_len);
		s->total++;
	}

	e2->children = NULL;
}

static struct snapshot* sync_scan_wc(ffstr path, uint flags)
{
	ffdirscan ds = {};
	const char *fn;
	char *fullname = NULL, *dirz = NULL;
	struct snapshot *r = NULL, *d = NULL, *parent = NULL;

	ffstr dir, name;
	ffpath_splitpath_str(path, &dir, &name);
	dirz = ffsz_dupstr(&dir);
	ds.wildcard = name.ptr;
	if (ffdirscan_open(&ds, dirz, FFDIRSCAN_USEWILDCARD)) {
		fcom_syserrlog("ffdirscan_open: '%s'  wc: '%s'", dirz, name.ptr);
		goto end;
	}

	while ((fn = ffdirscan_next(&ds))) {

		ffmem_free(fullname);
		fullname = ffsz_allocfmt("%s%c%s", dirz, FFPATH_SLASH, fn);

		xxfileinfo fi;
		if (!(!fffile_info_path(fullname, &fi.info) && fi.dir()))
			continue;

		if (!(d = sync_scan(FFSTR_Z(fullname), flags)))
			goto end;

		if (!parent) {
			parent = d;
		} else {
			sync_combine(parent, _fntr_ent_first(d->root));
			parent->total += d->total;
		}
		d = NULL;
	}

	r = parent;
	parent = NULL;

end:
	ffmem_free(dirz);
	ffmem_free(fullname);
	ffdirscan_close(&ds);
	sync_snapshot_free(d);
	sync_snapshot_free(parent);
	return r;
}
