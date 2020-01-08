/** Archives.
Copyright (c) 2019 Simon Zolin
*/

#include <fcom.h>

const fcom_core *core;
const fcom_command *com;
int fn_out(fcom_cmd *cmd, const ffstr *input, ffarr *buf);
int out_hlink(fcom_cmd *cmd, const char *target, const char *linkname);
int out_slink(fcom_cmd *cmd, const char *target, const char *linkname);
ffbool arc_members_wildcard(const ffarr2 *members);
ffbool arc_need_member(const ffarr2 *members, ffbool member_wildcard, const ffstr *fn);
