/** fcom: logger
2022, Simon Zolin */

static void exe_logv(uint flags, const char *fmt, va_list args)
{
	if (m->core) {
		fftime tm;
		fftime_now(&tm);
		tm.sec += m->core->tz.real_offset;
		if (fftime_cmp(&tm, &m->time_last)) {
			m->time_last = tm;
			ffdatetime dt;
			fftime_split1(&dt, &tm);
			fftime_tostr1(&dt, m->log_date, sizeof(m->log_date), FFTIME_HMS_MSEC);
		}
	}

	uint64 tid = 0;
	zzlog_printv(&m->log, flags, m->log_date, tid, "", NULL, fmt, args);
}

void exe_log(uint flags, const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	exe_logv(flags, fmt, args);
	va_end(args);
}

static void log_init(struct zzlog *l)
{
	if (!l->fd) {
		static const char levels[][8] = {
			"ERROR",
			"ERROR",
			"WARN ",
			"INFO ",
			"VERB ",
			"DEBUG",
		};
		ffmem_copy(l->levels, levels, sizeof(levels));

		static const char colors[][8] = {
			/*FCOM_LOG_FATAL*/		FFSTD_CLR(FFSTD_RED),
			/*FCOM_LOG_ERR*/		FFSTD_CLR(FFSTD_RED),
			/*FCOM_LOG_WARN*/		FFSTD_CLR(FFSTD_YELLOW),
			/*FCOM_LOG_INFO*/		FFSTD_CLR(FFSTD_GREEN),
			/*FCOM_LOG_VERBOSE*/	FFSTD_CLR(FFSTD_GREEN),
			/*FCOM_LOG_DEBUG*/		"",
		};
		ffmem_copy(l->colors, colors, sizeof(colors));
	}

	l->fd = (!m->core || !m->core->stdout_busy) ? ffstdout : ffstderr;
	int r = ffstd_attr(l->fd, FFSTD_VTERM, FFSTD_VTERM);
	l->use_color = !r;
	l->fd_file = (r < 0);
}
