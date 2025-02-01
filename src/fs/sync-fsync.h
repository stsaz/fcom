/** fcom: sync: perform directory synchronization
2022, Simon Zolin */

static void sync_run(fcom_op *op);

static void sync_on_op_complete(void *param, int result)
{
	struct sync *s = (struct sync*)param;

	if (result != 0) {
		fcom_cominfo *cmd = s->cmd;
		sync_close(s);
		core->com->complete(cmd, result);
		return;
	}

	fntree_cmp_ent *ce = ffslice_itemT(&s->cmp.ents, s->sc.cmp_idx, fntree_cmp_ent);
	switch (ce->status & FCOM_SYNC_MASK) {
	case FCOM_SYNC_LEFT:
		s->sc.stats.add++;
		break;
	case FCOM_SYNC_NEQ:
		s->sc.stats.overwritten++;
		break;
	case FCOM_SYNC_RIGHT:
		s->sc.stats.del++;
		break;
	}
	s->sc.cmp_idx++;
	sync_run(s);
}

/** Prepare target full file name */
static ffstr out_name(ffstr lname, ffstr lbase, ffstr rbase)
{
	// `in/d/f` -> `out/d/f`
	ffstr_shift(&lname, lbase.len);
	if (lname.len && ffpath_slash(lname.ptr[0]))
		ffstr_shift(&lname, 1);
	ffstr s = {};
	ffsize cap = 0;
	ffstr_growfmt(&s, &cap, "%S%c%S%Z", &rbase, FFPATH_SLASH, &lname);
	return s;
}

static void sync_copy_async(struct sync *s, ffstr src, ffstr dst, uint status)
{
	fcom_cominfo *c = core->com->create();
	c->operation = ffsz_dup("copy");

	ffvec a = {};
	*ffvec_pushT(&a, char*) = ffsz_dup("--verify");
	if (s->replace_date) {
		*ffvec_pushT(&a, char*) = ffsz_dup("--update");
		*ffvec_pushT(&a, char*) = ffsz_dup("--replace-date");
	}

	if (s->write_into)
		*ffvec_pushT(&a, char*) = ffsz_dup("--write-into");

	ffvec_zpushT(&a, char*);
	c->argv = (char**)a.ptr;
	c->argc = a.len - 1;

	ffstr *p = ffvec_pushT(&c->input, ffstr);
	char *sz = ffsz_dupstr(&src);
	ffstr_setz(p, sz);

	c->output = dst;

	c->recursive = 0xff; // disable auto-recursive mode
	c->test = s->cmd->test;
	c->buffer_size = s->cmd->buffer_size;
	c->directio = s->cmd->directio;

	c->overwrite = s->cmd->overwrite;
	if (status == FCOM_SYNC_NEQ)
		c->overwrite = 1;

	c->on_complete = sync_on_op_complete;
	c->opaque = s;
	core->com->run(c);
	fcom_dbglog("sync: copy: %S -> %S", &s->sc.lname, &c->output);
}

static void sync_move(struct sync *s, fntree_cmp_ent *ce, ffstr lname, ffstr rname)
{
	// "right_root/left_subpath/left_name"
	ffstr lpath = fntree_path(ce->lb);
	ffstr_shift(&lpath, s->src->root_dir.len);
	s->sc.lname.len = 0;
	s->sc.lname.add_f("%S%S%c%S"
		, &s->dst->root_dir, &lpath, FFPATH_SLASH, &xxrval(fntree_name(ce->l)));

	core->file->move(rname, s->sc.lname.str(), FCOM_FILE_MOVE_SAFE);
}

static void sync_trash_async(struct sync *s, ffstr rname)
{
	fcom_cominfo *c = core->com->create();
	c->operation = ffsz_dup("trash");

	ffstr *p = ffvec_pushT(&c->input, ffstr);
	char *sz = ffsz_dupstr(&rname);
	ffstr_setz(p, sz);

	c->test = s->cmd->test;
	c->buffer_size = s->cmd->buffer_size;
	c->overwrite = s->cmd->overwrite;

	c->on_complete = sync_on_op_complete;
	c->opaque = s;
	core->com->run(c);
	fcom_dbglog("sync: trash: %S", &rname);
}

/** Perform a single sync operation */
static int sync1(struct sync *s)
{
	if (s->sc.cmp_idx == s->cmp.ents.len) {
		fcom_infolog("Result: added:%u  deleted:%u  overwritten:%u"
			, s->sc.stats.add, s->sc.stats.del, s->sc.stats.overwritten);
		return 1;
	}

	fntree_cmp_ent *ce = ffslice_itemT(&s->cmp.ents, s->sc.cmp_idx, fntree_cmp_ent);

	snapshot::full_name(&s->sc.lname, ce->l, ce->lb);
	snapshot::full_name(&s->sc.rname, ce->r, ce->rb);

	switch (ce->status & FCOM_SYNC_MASK) {
	case FCOM_SYNC_LEFT:
		if (s->sync_add) {
			ffstr dst = out_name(*(ffstr*)&s->sc.lname, s->cmp.left->root_dir, s->cmp.right->root_dir);
			sync_copy_async(s, *(ffstr*)&s->sc.lname, dst, 0);
			return 0x123;
		}
		break;

	case FCOM_SYNC_RIGHT:
		if (s->sync_del) {
			sync_trash_async(s, *(ffstr*)&s->sc.rname);
			return 0x123;
		}
		break;

	case FCOM_SYNC_EQ:
		break;

	case FCOM_SYNC_NEQ:
		if (s->sync_update) {
			ffstr dst = out_name(*(ffstr*)&s->sc.lname, s->cmp.left->root_dir, s->cmp.right->root_dir);
			sync_copy_async(s, *(ffstr*)&s->sc.lname, dst, FCOM_SYNC_NEQ);
			return 0x123;
		}
		break;

	case FCOM_SYNC_MOVE:
		if (s->sync_move) {
			sync_move(s, ce, *(ffstr*)&s->sc.lname, *(ffstr*)&s->sc.rname);
		}
		break;

	default:
		FCOM_ASSERT(0);
		return 1;
	}

	s->sc.cmp_idx++;
	return 0;
}

int sync_sync(fcom_sync_diff *sd, void *diff_entry_id, uint flags
	, void(*on_complete)(void*, int), void *param)
{
	int rc = -1;
	fcom_sync_diff_entry de_s, *de = &de_s;
	sync_info_id(sd, diff_entry_id, flags, de);
	fcom_sync_snapshot *l = sd->left, *r = sd->right;
	if (flags & FCOM_SYNC_SWAP) {
		FF_SWAP2(l, r);
	}

	switch (de->status & FCOM_SYNC_MASK) {
	case FCOM_SYNC_LEFT:
	case FCOM_SYNC_NEQ: {
		fcom_cominfo *c = core->com->create();
		c->operation = ffsz_dup("copy");

		ffvec a = {};
		if (flags & FCOM_SYNC_VERIFY)
			*ffvec_pushT(&a, char*) = ffsz_dup("--verify");
		if (flags & FCOM_SYNC_REPLACE_DATE) {
			*ffvec_pushT(&a, char*) = ffsz_dup("--update");
			*ffvec_pushT(&a, char*) = ffsz_dup("--replace-date");
		}
		*ffvec_pushT(&a, char*) = ffsz_dup("--md5");
		ffvec_zpushT(&a, char*);
		c->argv = (char**)a.ptr;
		c->argc = a.len - 1;

		ffstr *p = ffvec_pushT(&c->input, ffstr);
		char *sz = ffsz_dupstr(&de->lname);
		ffstr_setz(p, sz);

		c->output = out_name(de->lname, l->root_dir, r->root_dir);

		c->recursive = 0xff; // disable auto-recursive mode
		c->overwrite = !!(de->status & FCOM_SYNC_NEQ);

		c->on_complete = on_complete;
		c->opaque = param;
		fcom_dbglog("sync: copy: '%S' -> '%S'", &de->lname, &c->output);
		core->com->run(c);
		rc = 1;
		goto end;
	}

	case FCOM_SYNC_RIGHT: {
		fcom_cominfo *c = core->com->create();
		c->operation = ffsz_dup("trash");

		ffstr *p = ffvec_pushT(&c->input, ffstr);
		char *sz = ffsz_dupstr(&de->rname);
		ffstr_setz(p, sz);

		c->overwrite = 1; // if trash doesn't work: delete

		c->on_complete = on_complete;
		c->opaque = param;
		fcom_dbglog("sync: trash: '%S'", &de->rname);
		core->com->run(c);
		rc = 1;
		goto end;
	}

	case FCOM_SYNC_EQ:
		break;

	case FCOM_SYNC_MOVE: {
		// "right_root/left_subpath/left_name"
		ffstr lname = de->lname;
		if (lname.len && ffpath_slash(lname.ptr[0]))
			ffstr_shift(&lname, 1);
		ffstr_shift(&lname, l->root_dir.len);
		xxvec v;
		v.add_f("%S%c%S"
			, &r->root_dir, FFPATH_SLASH, &lname);
		if (core->file->move(de->rname, v.str(), FCOM_FILE_MOVE_SAFE))
			goto end;
		break;
	}

	default:
		FCOM_ASSERT(0);
		goto end;
	}

	rc = 0;

end:
	fcom_sync_diff_entry_destroy(de);
	return rc;
}
