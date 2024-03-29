/** Archives.
Copyright (c) 2019 Simon Zolin
*/

#include <fcom.h>
#include <ffbase/map.h>

extern const fcom_core *core;
extern const fcom_command *com;

int fn_out(fcom_cmd *cmd, const ffstr *input, ffarr *buf);
int out_hlink(fcom_cmd *cmd, ffstr target, const char *linkname);
int out_slink(fcom_cmd *cmd, ffstr target, const char *linkname);


static inline void fcom_cmd_set(fcom_cmd *dst, const fcom_cmd *src)
{
	ffmem_copy(dst, src, sizeof(*dst));
	ffstr_null(&dst->in);
	ffstr_null(&dst->out);
}

static inline int task_create_run(fcom_cmd *parent, const char *prefilter_name, const char *filter_name, const char *in_filename
	, void *cb_func, void *cb_param)
{
	fcom_cmd ncmd = {};
	fcom_cmd_set(&ncmd, parent);
	ncmd.name = filter_name;
	ncmd.flags = FCOM_CMD_EMPTY | FCOM_CMD_INTENSE;
	ncmd.input.fn = in_filename;

	fcom_cmd *nc;
	if (NULL == (nc = com->create(&ncmd)))
		return FCOM_ERR;
	com->ctrl(nc, FCOM_CMD_FILTADD_LAST, FCOM_CMD_FILT_IN(nc));
	if (prefilter_name != NULL)
		com->ctrl(nc, FCOM_CMD_FILTADD_LAST, prefilter_name);
	com->ctrl(nc, FCOM_CMD_FILTADD_LAST, filter_name);
	com->fcom_cmd_monitor_func(nc, cb_func, cb_param);
	com->ctrl(nc, FCOM_CMD_RUNASYNC);
	return FCOM_DONE;
}

struct members {
	ffmap members;
	ffvec members_wildcard; // ffstr[]
};
/**
data: char*[] */
void members_optimize(struct members *m, ffslice data);
void members_destroy(struct members *m);
ffbool members_check(struct members *m, ffstr fn);
