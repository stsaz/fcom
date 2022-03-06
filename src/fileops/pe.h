/** fcom: PE info
2021, Simon Zolin
*/

#define FILT_NAME  "peinfo"

struct peinfo {
	uint state;
	uint dd_idx;
	ffpe pe;
};

static void* f_pe_open(fcom_cmd *cmd)
{
	struct peinfo *p = ffmem_new(struct peinfo);
	if (p == NULL)
		return NULL;
	ffpe_open(&p->pe);
	return p;
}

static void f_pe_close(void *_p, fcom_cmd *cmd)
{
	struct peinfo *p = _p;
	ffpe_close(&p->pe);
	ffmem_free(p);
}

static int f_pe_process(void *_p, fcom_cmd *cmd)
{
	struct peinfo *p = _p;
	int r;

	enum { I_NEXTFILE, I_DATA, };

again:
	switch (p->state) {
	case I_NEXTFILE:
		if (NULL == (cmd->input.fn = com->arg_next(cmd, FCOM_CMD_ARG_FILE)))
			return FCOM_FIN;
		com->ctrl(cmd, FCOM_CMD_FILTADD_PREV, FCOM_CMD_FILT_IN(cmd));
		p->state = I_DATA;
		return FCOM_MORE;

	case I_DATA:
		break;
	}

	if (cmd->flags & FCOM_CMD_FWD)
		ffpe_input(&p->pe, cmd->in.ptr, cmd->in.len);

	for (;;) {
	r = ffpe_read(&p->pe);

	switch (r) {
	case FFPE_MORE:
		return FCOM_MORE;

	case FFPE_SEEK:
		fcom_cmd_seek(cmd, ffpe_offset(&p->pe));
		return FCOM_MORE;

	case FFPE_HDR: {
		const struct ffpe_info *i = &p->pe.info;
		core->log(FCOM_LOGINFO | FCOM_LOGNOPFX, "header: machine:%xu  sections:%u  tm_created:%u  linker_ver:%u.%u  code_size:%xu  "
			"init_data_size:%xu  uninit_data_size:%xu  "
			"stack_size_res:%xU  stack_size_commit:%xU  "
			"entry_addr:%xu  pe32+:%u"
			, i->machine
			, i->sections
			, i->tm_created
			, i->linker_ver[0], i->linker_ver[1]
			, i->code_size
			, i->init_data_size, i->uninit_data_size
			, i->stack_size_res, i->stack_size_commit
			, i->entry_addr, i->pe32plus);
		break;
	}

	case FFPE_DD: {
		const struct ffpe_data_dir *dd = &p->pe.data_dir;
		core->log(FCOM_LOGINFO | FCOM_LOGNOPFX, "dd[%u]:  vaddr:%xu  vsize:%xu"
			, ++p->dd_idx, dd->vaddr, dd->vsize);
		break;
	}

	case FFPE_SECT: {
		const struct ffpe_sect *s = &p->pe.section;
		core->log(FCOM_LOGINFO | FCOM_LOGNOPFX, "section: %s  vaddr:%xu  vsize:%xu  raw_off:%xu  raw_size:%xu  flags:%xu"
			, s->name, s->vaddr, s->vsize, s->raw_off, s->raw_size, s->flags);
		break;
	}

	case FFPE_IMPDIR: {
		uint n = p->pe.imp_dir.len / sizeof(struct coff_imp_dir);
		const struct coff_imp_dir *id = (void*)p->pe.imp_dir.ptr;
		for (uint i = 0;  i != n;  i++) {
			core->log(FCOM_LOGINFO | FCOM_LOGNOPFX, "import dir: "
				"lookups_rva:%xu  "
				"unused1:%xu  "
				"forwarder:%xu  "
				"name_rva:%xu  "
				"addrs_rva:%xu"
				, ffint_le_cpu32_ptr(id[i].lookups_rva)
				, ffint_le_cpu32_ptr(id[i].unused1)
				, ffint_le_cpu32_ptr(id[i].forwarder)
				, ffint_le_cpu32_ptr(id[i].name_rva)
				, ffint_le_cpu32_ptr(id[i].addrs_rva));
		}
		break;
	}

	case FFPE_IMPORT: {
		const struct ffpe_imp_ent *i = &p->pe.import;
		core->log(FCOM_LOGINFO | FCOM_LOGNOPFX, "import: dll_name:%s  sym_name:%s  sym_ordinal:%U"
			, i->dll_name, i->sym_name, i->sym_ordinal);
		break;
	}

	case FFPE_DONE:
		p->state = I_NEXTFILE;
		ffpe_close(&p->pe);
		ffmem_tzero(&p->pe);
		ffpe_open(&p->pe);
		goto again;

	case FFPE_ERR:
	default:
		errlog("ffpe_read(): %s", ffpe_errstr(&p->pe));
		return FCOM_ERR;
	}
	}
}

#undef FILT_NAME

static const fcom_filter f_peinfo = { f_pe_open, f_pe_close, f_pe_process };
