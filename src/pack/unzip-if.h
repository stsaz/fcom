/** fcom: unzip interface
2024, Simon Zolin */

struct unzip_ctx {
	ffzipread rzip;
	fffd	f;
	ffvec	buf;
	ffstr	in;
};

static fcom_unpack_obj* unzip_if_open_file(fffd f)
{
	struct unzip_ctx *ux = ffmem_new(struct unzip_ctx);
	if (ffzipread_open(&ux->rzip, fffile_size(f))) {
		ffmem_free(ux);
		return NULL;
	}
	ffvec_alloc(&ux->buf, 128*1024, 1);
	ux->f = f;
	return ux;
}

static void unzip_if_close(fcom_unpack_obj *o)
{
	struct unzip_ctx *ux = o;
	ffzipread_close(&ux->rzip);
	ffvec_free(&ux->buf);
	ffmem_free(ux);
}

static ffstr unzip_if_next(fcom_unpack_obj *o, void *unused)
{
	struct unzip_ctx *ux = o;
	ffstr out;
	for (;;) {

		int r = ffzipread_process(&ux->rzip, &ux->in, &out);
		fcom_dbglog("ffzipread_process: %d", r);

		switch ((enum FFZIPREAD_R)r) {
		case FFZIPREAD_MORE:
			r = fffile_read(ux->f, ux->buf.ptr, ux->buf.cap);
			if (r < 0) {
				fcom_syserrlog("file read");
				goto end;
			} else if (r == 0) {
				fcom_dbglog("zip file read: finished");
				goto end;
			}
			fcom_dbglog("zip file read: %u", r);
			ffstr_set(&ux->in, ux->buf.ptr, r);
			break;

		case FFZIPREAD_SEEK:
			ux->in.len = 0;
			fffile_seek(ux->f, ffzipread_offset(&ux->rzip), FFFILE_SEEK_BEGIN);
			break;

		case FFZIPREAD_FILEINFO: {
			const ffzipread_fileinfo_t *zi = ffzipread_fileinfo(&ux->rzip);
			fcom_dbglog("zip file read: %S", &zi->name);
			return zi->name;
		}

		case FFZIPREAD_FILEDONE:
		case FFZIPREAD_FILEHEADER:
		case FFZIPREAD_DATA:
			FF_ASSERT(0);
			break;

		case FFZIPREAD_DONE:
			goto end;

		case FFZIPREAD_WARNING:
			fcom_warnlog("ffzipread_process: %s @%xU"
				, ffzipread_error(&ux->rzip), ffzipread_offset(&ux->rzip));
			break;

		case FFZIPREAD_ERROR:
			fcom_errlog("ffzipread_process: %s @%xU"
				, ffzipread_error(&ux->rzip), ffzipread_offset(&ux->rzip));
			goto end;
		}
	}
end:
	return FFSTR_Z("");
}

FF_EXPORT const fcom_unpack_if fcom_unzip = {
	unzip_if_open_file,
	unzip_if_close,
	unzip_if_next,
};
