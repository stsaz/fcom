/** Archives.
Copyright (c) 2019 Simon Zolin
*/

#include <fcom.h>

extern const fcom_core *core;
extern const fcom_command *com;

int fn_out(fcom_cmd *cmd, const ffstr *input, ffarr *buf);
int out_hlink(fcom_cmd *cmd, ffstr target, const char *linkname);
int out_slink(fcom_cmd *cmd, ffstr target, const char *linkname);
ffbool arc_members_wildcard(const ffarr2 *members);
ffbool arc_need_member(const ffarr2 *members, ffbool member_wildcard, const ffstr *fn);
ffbool arc_need_file(fcom_cmd *cmd, const ffstr *fn);
