/** zstd write
2021, Simon Zolin
*/

struct zstd1 {
	zstd_encoder *z;
	uint flags;
	ffstr in;
	uint64 in_total, out_total;
	ffvec buf;
};

static void* zstd1_open(fcom_cmd *cmd)
{
	struct zstd1 *z = ffmem_new(struct zstd1);
	zstd_enc_conf zc = {};
	zc.level = cmd->zstd_level;
	zc.workers = cmd->zstd_workers;
	zstd_encode_init(&z->z, &zc);
	ffvec_alloc(&z->buf, zc.max_block_size, 1);
	return z;
}

static void zstd1_close(void *p, fcom_cmd *cmd)
{
	struct zstd1 *z = p;
	zstd_encode_free(z->z);
	ffvec_free(&z->buf);
	ffmem_free(z);
}

static int zstd1_process(void *p, fcom_cmd *cmd)
{
	struct zstd1 *z = p;

	if (cmd->flags & FCOM_CMD_FWD) {
		if (cmd->flags & FCOM_CMD_FIRST)
			z->flags = ZSTD_FFINISH;
		z->in = cmd->in;
		cmd->in.len = 0;
	}

	zstd_buf in, out;
	zstd_buf_set(&in, z->in.ptr, z->in.len);
	zstd_buf_set(&out, z->buf.ptr, z->buf.cap);
	int r = zstd_encode(z->z, &in, &out, z->flags);
	ffstr_shift(&z->in, in.pos);
	ffstr_set(&cmd->out, z->buf.ptr, out.pos);
	z->in_total += in.pos;
	z->out_total += out.pos;

	fcom_dbglog(0, "zstd1", "zstd_encode: %L (%U) -> %L (%U)"
		, in.pos, z->in_total, out.pos, z->out_total);

	if (r < 0) {
		fcom_errlog("zstd1", "zstd_encode: %s", zstd_error(r));
		return FCOM_ERR;
	} else if (out.pos == 0) {
		if (cmd->flags & FCOM_CMD_FIRST)
			return FCOM_OUTPUTDONE;
		return FCOM_MORE;
	}

	return FCOM_DATA;
}

const fcom_filter zstd1_filt = { zstd1_open, zstd1_close, zstd1_process };
