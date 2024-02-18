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
	switch (ce->status & 0x0f) {
	case FNTREE_CMP_LEFT:
		s->sc.stats.add++;
		break;
	case FNTREE_CMP_NEQ:
		s->sc.stats.overwritten++;
		break;
	case FNTREE_CMP_RIGHT:
		s->sc.stats.del++;
		break;
	}
	s->sc.cmp_idx++;
	sync_run(s);
}

/** Prepare target full file name */
static ffstr out_name(ffstr lname, ffstr lbase, ffstr rpath)
{
	// `in/d/f` -> `out/d/f`
	ffstr_shift(&lname, lbase.len);
	ffstr s = {};
	ffsize cap = 0;
	ffstr_growfmt(&s, &cap, "%S%c%S%Z", &rpath, FFPATH_SLASH, &lname);
	return s;
}

static void sync_copy(struct sync *s, fntree_cmp_ent *ce)
{
	fcom_cominfo *c = core->com->create();
	c->operation = ffsz_dup("copy");

	ffvec a = {};
	*ffvec_pushT(&a, char*) = ffsz_dup("--verify");
	ffvec_zpushT(&a, char*);
	c->argv = (char**)a.ptr;
	c->argc = a.len - 1;

	ffstr *p = ffvec_pushT(&c->input, ffstr);
	char *sz = ffsz_dupstr((ffstr*)&s->sc.lname);
	ffstr_setz(p, sz);

	ffstr lbase = fntree_path(_fntr_ent_first(s->src.root)->children);
	c->output = out_name(*(ffstr*)&s->sc.lname, lbase, s->cmd->output);

	c->recursive = 0xff; // disable auto-recursive mode
	c->test = s->cmd->test;
	c->buffer_size = s->cmd->buffer_size;
	c->directio = s->cmd->directio;
	c->overwrite = s->cmd->overwrite;

	uint st = ce->status & 0x0f;
	if (st == FNTREE_CMP_NEQ)
		c->overwrite = 1;

	c->on_complete = sync_on_op_complete;
	c->opaque = s;
	core->com->run(c);
	fcom_dbglog("sync: copy: %S -> %S", &s->sc.lname, &c->output);
}

static void sync_move(struct sync *s, fntree_cmp_ent *ce)
{
	// "right_root/left_subpath/left_name"
	ffstr lpath = fntree_path(ce->lb);
	ffstr_shift(&lpath, s->src.root_dir.len);
	s->sc.lname.len = 0;
	s->sc.lname.add_f("%S%S%c%S"
		, &s->dst.root_dir, &lpath, FFPATH_SLASH, &xxrval(fntree_name(ce->l)));

	core->file->move(*(ffstr*)&s->sc.rname, s->sc.lname.str(), FCOM_FILE_MOVE_SAFE);
}

static void sync_trash(struct sync *s)
{
	fcom_cominfo *c = core->com->create();
	c->operation = ffsz_dup("trash");

	ffstr *p = ffvec_pushT(&c->input, ffstr);
	char *sz = ffsz_dupstr((ffstr*)&s->sc.rname);
	ffstr_setz(p, sz);

	c->test = s->cmd->test;
	c->buffer_size = s->cmd->buffer_size;
	c->overwrite = s->cmd->overwrite;

	c->on_complete = sync_on_op_complete;
	c->opaque = s;
	core->com->run(c);
	fcom_dbglog("sync: trash: %S", &s->sc.rname);
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
	uint st = ce->status & 0x0f;

	if (ce->status & FNTREE_CMP_SKIP)
		goto next;

	full_name(&s->sc.lname, ce->l, ce->lb);
	full_name(&s->sc.rname, ce->r, ce->rb);

	switch (st) {
	case FNTREE_CMP_EQ:
		break;

	case FNTREE_CMP_LEFT:
		if (ce->status & FNTREE_CMP_MOVED) {
			if (s->sync_move) {
				sync_move(s, ce);
				goto next;
			}
			break;
		}

		if (s->sync_add) {
			sync_copy(s, ce);
			return 0x123;
		}
		break;

	case FNTREE_CMP_NEQ:
		if (s->sync_update) {
			sync_copy(s, ce);
			return 0x123;
		}
		break;

	case FNTREE_CMP_RIGHT:
		if (ce->status & FNTREE_CMP_MOVED) {
			if (s->sync_move) {
				sync_move(s, ce);
				goto next;
			}
			break;
		}

		if (s->sync_del) {
			sync_trash(s);
			return 0x123;
		}
		break;

	default:
		FCOM_ASSERT(0);
		return 1;
	}

next:
	s->sc.cmp_idx++;
	return 0;
}
