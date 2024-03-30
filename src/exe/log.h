/** fcom: logger
2022, Simon Zolin */

// TIME :TID LEVEL MSG [: SYS-ERROR]
static void stdlogv(uint flags, const char *fmt, va_list args)
{
	uint level = flags & 0xf;
	char buf[4096];
	ffsize cap = sizeof(buf) - 1;
	ffstr d = FFSTR_INITN(buf, 0);

	static const char err_str[][8] = {
		"ERROR",
		"ERROR",
		"WARNING",
		"INFO",
		"VERBOSE",
		"DEBUG",
	};
	if (level == FCOM_LOG_FATAL) {
		ffstr_addfmt(&d, cap - d.len, "%s\t", err_str[level]);

	} else if (level == FCOM_LOG_INFO) {

	} else if (level != FCOM_LOG_VERBOSE) {
		fftime t;
		fftime_now(&t);
		ffdatetime dt;
		fftime_split1(&dt, &t);
		d.len = fftime_tostr1(&dt, d.ptr, cap - d.len, FFTIME_HMS_MSEC);

		uint64 tid = ffthread_curid();
		ffstr_addfmt(&d, cap - d.len, " :%U %s\t", tid, err_str[level]);
	}
	ffstr_addfmtv(&d, cap - d.len, fmt, args);

	if (flags & FCOM_LOG_SYSERR) {
		ffstr_addfmt(&d, cap - d.len, ": (%d) %s", fferr_last(), fferr_strptr(fferr_last()));
	}

	d.ptr[d.len++] = '\n';
	ffstderr_write(d.ptr, d.len);
}

void stdlog(uint flags, const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	stdlogv(flags, fmt, args);
	va_end(args);
}
