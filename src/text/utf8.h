/** fcom: convert files to UTF-8
2021, Simon Zolin
*/

#define FILT_NAME  "utf8"

struct utf8 {
	ffarr fdata;
	ffarr out;
	const char *ofn;
	ffarr fn;
};

static void* utf8_open(fcom_cmd *cmd)
{
	struct utf8 *u = ffmem_new(struct utf8);
	return u;
}

static void utf8_close(void *p, fcom_cmd *cmd)
{
	struct utf8 *u = p;
	ffarr_free(&u->fdata);
	ffarr_free(&u->out);
	ffarr_free(&u->fn);
	ffmem_free(u);
}

static size_t ffutf8_encodedata(char *dst, size_t cap, const char *src, size_t *plen, uint flags)
{
	ffssize r;
	size_t len = *plen;

	switch (flags) {
	case FFUNICODE_UTF8:
		if (dst == NULL)
			return len;
		len = ffmin(cap, len);
		ffmemcpy(dst, src, len);
		*plen = len;
		return len;

	case FFUNICODE_UTF16LE:
		r = ffutf8_from_utf16(dst, cap, src, *plen, FFUNICODE_UTF16LE);
		break;

	case FFUNICODE_UTF16BE:
		r = ffutf8_from_utf16(dst, cap, src, *plen, FFUNICODE_UTF16BE);
		break;

	default:
		r = ffutf8_from_cp(dst, cap, src, *plen, flags);
		break;
	}

	if (r < 0)
		return 0;
	return r;
}

static size_t ffutf8_encodewhole(char *dst, size_t cap, const char *src, size_t len, uint flags)
{
	size_t ln = len;
	size_t r = ffutf8_encodedata(dst, cap, src, &ln, flags);
	if (ln != len)
		return 0; //not enough output space
	return r;
}

static size_t ffutf8_strencode(ffstr3 *dst, const char *src, size_t len, uint flags)
{
	size_t r = ffutf8_encodewhole(NULL, 0, src, len, flags);
	if (NULL == ffarr_realloc(dst, r))
		return 0;
	r = ffutf8_encodewhole(dst->ptr, dst->cap, src, len, flags);
	dst->len = r;
	return r;
}

// Note: uses much memory (reads the whole file, writes the whole file)
static int utf8_process(void *p, fcom_cmd *cmd)
{
	struct utf8 *u = p;
	const char *fn;
	int coding;
	ffstr data;

	if (u->ofn == NULL) {
		if (cmd->output.fn == NULL) {
			errlog("output file isn't set", 0);
			return FCOM_ERR;
		}
		u->ofn = cmd->output.fn;
	}

	for (;;) {
		if (NULL == (fn = com->arg_next(cmd, FCOM_CMD_ARG_FILE)))
			return FCOM_DONE;
		if (0 != fffile_readall(&u->fdata, fn, (uint64)-1))
			return FCOM_SYSERR;
		ffstr_set2(&data, &u->fdata);

		size_t n = data.len;
		coding = ffutf_bom(data.ptr, &n);
		if (coding != FFUNICODE_UTF16LE && coding != FFUNICODE_UTF16BE) {
			fcom_infolog(FILT_NAME, "skipping file %s", fn);
			continue;
		}
		ffstr_shift(&data, n);

		if (0 == ffutf8_strencode(&u->out, data.ptr, data.len, coding)) {
			errlog("ffutf8_strencode", 0);
			return FCOM_ERR;
		}

		ffstr idir, iname, iext;
		ffstr odir, oname, oext;
		ffstr_setz(&iname, fn);
		ffpath_split3(iname.ptr, iname.len, &idir, &iname, &iext);
		ffstr_setz(&oname, u->ofn);
		path_split3(oname.ptr, oname.len, &odir, &oname, &oext);

		cmd->output.fn = u->ofn;
		if (oname.len == 0) {
			if (0 != ffpath_makefn_out(&u->fn, &idir, &iname, &odir, &oext))
				return FCOM_SYSERR;
			cmd->output.fn = u->fn.ptr;
		}

		com->ctrl(cmd, FCOM_CMD_FILTADD_LAST, FCOM_CMD_FILT_OUT(cmd));
		ffstr_set2(&cmd->out, &u->out);
		return FCOM_NEXTDONE;
	}
}

#undef FILT_NAME

const fcom_filter utf8_filt = { &utf8_open, &utf8_close, &utf8_process, };
