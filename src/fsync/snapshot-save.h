/** fcom fsync
2022, Simon Zolin
*/

#undef FILT_NAME
#define FILT_NAME  "syncss"

struct fsyncss {
	uint state;
	struct dir *tree;
	struct cursor cur;
	ffconfw cw;
	void *curdir;
};

static void* fsyncss_open(fcom_cmd *cmd)
{
	struct fsyncss *f;
	if (NULL == (f = ffmem_new(struct fsyncss)))
		return FCOM_OPEN_SYSERR;
	ffconfw_init(&f->cw, 0);
	return f;
}

static void fsyncss_close(void *p, fcom_cmd *cmd)
{
	struct fsyncss *f = p;
	tree_free(f->tree);
	ffconfw_close(&f->cw);
	ffmem_free(f);
}

static int fsyncss_process(void *p, fcom_cmd *cmd)
{
	struct fsyncss *f = p;

	for (;;) {
	switch (f->state) {
	case 0: {
		char *fn;
		if (NULL == (fn = com->arg_next(cmd, 0))) {
			if (f->tree == NULL) {
				errlog("no input files");
				return FCOM_ERR;
			}
			goto done;
		}

		if (f->tree == NULL) {
			if (cmd->output.fn == NULL) {
				errlog("output file isn't specified", 0);
				return FCOM_ERR;
			}

			com->ctrl(cmd, FCOM_CMD_FILTADD_LAST, FCOM_CMD_FILT_OUT(cmd));

			ffconfw_addlinez(&f->cw, "# fcom file tree snapshot");
		}

		tree_free(f->tree);
		if (NULL == (f->tree = scan_tree(fn, 0)))
			return FCOM_ERR;

		cur_init(&f->cur, f->tree);

		f->curdir = NULL;
	}
	// fall through

	case 1:
		for (;;) {
			const struct file *fl = cur_get(&f->cur);
			if (fl == NULL)
				break;

			cur_next(&f->cur);

			if (fl->parent != f->curdir) {
				// got a file from another directory
				snapshot_writedir(&f->cw, fl->parent, (f->curdir != NULL));
				f->curdir = fl->parent;
			}

			snapshot_writefile(&f->cw, fl);

			ffstr s;
			ffconfw_output(&f->cw, &s);
			if (s.len >= 64 * 1024) {
				cmd->out = s;
				f->state = 2;
				return FCOM_DATA;
			}
		}
		f->state = 0;
		break;

	case 2:
		ffconfw_clear(&f->cw);
		f->state = 1;
		continue;
	}
	}

done:
	ffconfw_addobj(&f->cw, 0);
	if (0 != ffconfw_fin(&f->cw))
		errlog("config write", 0);
	ffconfw_output(&f->cw, &cmd->out);
	return FCOM_DONE;
}

static const fcom_filter fsyncss_filt = { fsyncss_open, fsyncss_close, fsyncss_process };

#undef FILT_NAME
