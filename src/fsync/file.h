/** fcom fsync
2022, Simon Zolin
*/

static void file_destroy(struct file *f)
{
	ffmem_safefree(f->name);
}

void file_info_set(struct file *f, fffileinfo *fi)
{
#ifdef FF_UNIX
	f->attr = fffile_infoattr(fi);
#else
	f->attr = fffile_isdir(fffile_infoattr(fi)) ? FFUNIX_FILE_DIR : FFUNIX_FILE_REG;
	f->attr |= 0755;
#endif
	f->size = fffile_infosize(fi);
	f->mtime = fffile_infomtime(fi);
}

#define ffint_cmp(a, b) \
	(((a) == (b)) ? 0 : ((a) < (b)) ? -1 : 1)

/** Compare attributes of 2 files.
Return enum FSYNC_ST. */
static int file_cmp(const struct file *f1, const struct file *f2, uint flags)
{
	if (isdir(f1->attr) != isdir(f2->attr))
		return FSYNC_ST_NEQ;

	int r;
	uint m = 0;
	if ((flags & FSYNC_CMP_SIZE)
		&& !isdir(f1->attr)
		&& f1->size != f2->size) {
		m |= (f1->size < f2->size) ? FSYNC_ST_SMALLER : FSYNC_ST_LARGER;
	}

	r = 0;
	if (flags & FSYNC_CMP_MTIME) {
		if (flags & FSYNC_CMP_MTIME_SEC)
			r = ffint_cmp(f1->mtime.sec, f2->mtime.sec);
		else
			r = fftime_cmp(&f1->mtime, &f2->mtime);
	}
	if (r != 0)
		m |= (r < 0) ? FSYNC_ST_OLDER : FSYNC_ST_NEWER;

	if ((flags & FSYNC_CMP_ATTR) && f1->attr != f2->attr)
		m |= FSYNC_ST_ATTR;

	return (m != 0) ? FSYNC_ST_NEQ | m : FSYNC_ST_EQ;
}
