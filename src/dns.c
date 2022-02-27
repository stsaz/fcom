/** DNS client.
Copyright (c) 2020 Simon Zolin */

#include <fcom.h>

#include <FF/net/dns-client.h>
#include <FF/net/dns.h>
#include <FF/net/proto.h>
#include <FFOS/netconf.h>


#define errlog(mod, fmt, ...)  fcom_errlog(mod, fmt, __VA_ARGS__)


static const fcom_core *core;
static const fcom_command *com;

// MODULE
static int dns_sig(uint signo);
static const void* dns_iface(const char *name);
static const fcom_mod dns_mod = {
	.sig = &dns_sig, .iface = &dns_iface,
};

// DNS
static void* dnscl_open(fcom_cmd *cmd);
static void dnscl_close(void *p, fcom_cmd *cmd);
static int dnscl_process(void *p, fcom_cmd *cmd);
static const fcom_filter net_dnscl_filt = { &dnscl_open, &dnscl_close, &dnscl_process };


FF_EXP const fcom_mod* fcom_getmod(const fcom_core *_core)
{
	core = _core;
	return &dns_mod;
}

struct oper {
	const char *name;
	const char *mod;
	const void *iface;
};

static const struct oper cmds[] = {
	{ "dns", "net.dnscl", &net_dnscl_filt },
};

static const void* dns_iface(const char *name)
{
	const struct oper *op;
	FFARR_WALKNT(cmds, FFCNT(cmds), op, struct oper) {
		if (ffsz_eq(name, op->mod + FFSLEN("net.")))
			return op->iface;
	}
	return NULL;
}

static int dns_sig(uint signo)
{
	switch (signo) {
	case FCOM_SIGINIT: {
		if (0 != ffskt_init(FFSKT_WSA))
			return -1;
		com = core->iface("core.com");

		const struct oper *op;
		FFARR_WALKNT(cmds, FFCNT(cmds), op, struct oper) {
			if (op->name != NULL
				&& 0 != com->reg(op->name, op->mod))
				return -1;
		}
		break;
	}
	case FCOM_SIGSTART:
		break;
	case FCOM_SIGFREE:
		break;
	}
	return 0;
}


typedef struct {
	ffdnsclient *cl;
	fcom_cmd *cmd;
	uint njobs;
} dnscl;

static void dnscl_log(uint level, const char *fmt, ...)
{
	va_list va;
	va_start(va, fmt);
	ffarr a = {};
	ffstr_catfmtv(&a, fmt, va);
	switch (level & 0x0f) {
	case FFDNSCL_LOG_ERR:
	case FFDNSCL_LOG_WARN:
		fcom_warnlog("dnscl", "%S", &a);
		break;
	default:
		fcom_dbglog(0, "dnscl", "%S", &a);
		break;
	}
	va_end(va);
	ffarr_free(&a);
}

static void dnscl_timer(fftimerqueue_node *tmr, uint value_ms)
{
	core->timer(tmr, value_ms, 0);
}

static void* dnscl_open(fcom_cmd *cmd)
{
	dnscl *c = ffmem_new(dnscl);
	ffdnscl_conf conf;
	ffdnscl_conf_init(&conf);
	conf.kq = core->kq;
	conf.max_tries = 3;
	conf.debug_log = 1;
	conf.timer = &dnscl_timer;
	conf.log = &dnscl_log;
	if (NULL == (c->cl = ffdnscl_new(&conf))) {
		ffmem_free(c);
		return NULL;
	}

	if (cmd->servers.len == 0) {
		ffbool added = 0;
		ffnetconf nc = {};
		if (0 == ffnetconf_get(&nc, FFNETCONF_DNS_ADDR)) {
			for (uint i = 0;  i != nc.dns_addrs_num;  i++) {
				ffstr s;
				ffstr_setz(&s, nc.dns_addrs[i]);
				if (0 != ffdnscl_serv_add(c->cl, &s))
					errlog("dnscl", "can't use server name %S", &s);
				else
					added = 1;
			}
		}
		ffnetconf_destroy(&nc);

		if (!added) {
			ffstr s;
			ffstr_setz(&s, "9.9.9.9");
			if (0 != ffdnscl_serv_add(c->cl, &s)) {
				errlog("dnscl", "can't use server name %S", &s);
				dnscl_close(c, cmd);
				return NULL;
			}
		}
	}

	ffstr *srv;
	FFARR_WALKT(&cmd->servers, srv, ffstr) {
		if (0 != ffdnscl_serv_add(c->cl, srv)) {
			errlog("dnscl", "can't use server name %S", srv);
			dnscl_close(c, cmd);
			return NULL;
		}
	}

	c->cmd = cmd;
	return c;
}

static void dnscl_close(void *p, fcom_cmd *cmd)
{
	dnscl *c = p;
	ffdnscl_free(c->cl);
	ffmem_free(c);
}

/** Called by ffdnscl after DNS response is processed. */
static void dnscl_complete(void *udata, const ffdnscl_result *res)
{
	dnscl *c = udata;
	if (res->status != 0) {
		errlog("dnscl", "result: %d (%s)", res->status, ffdns_rcode_str(res->status));
		return;
	}

	ffarr data = {};
	const ffip6 *ip;
	FFARR_WALKT(&res->ip, ip, ffip6) {
		char buf[FFIP6_STRLEN];
		size_t n = ffip46_tostr(ip, buf, sizeof(buf));
		ffstr_catfmt(&data, "%*s  ", n, buf);
	}

	// server addr
	fcom_userlog("%S [@%S]: %S", &res->name, &res->server_addr, &data);
	ffarr_free(&data);

	if (--c->njobs == 0)
		com->ctrl(c->cmd, FCOM_CMD_RUNASYNC);
}

/** Send DNS requests */
static int dnscl_process(void *p, fcom_cmd *cmd)
{
	dnscl *c = p;
	ffstr name;
	for (;;) {
		const char *s = com->arg_next(cmd, 0);
		if (s == NULL) {
			if (c->njobs == 0)
				return FCOM_DONE;
			break;
		}
		ffstr_setz(&name, s);
		int r = ffdnscl_resolve(c->cl, name, &dnscl_complete, c, 0);
		if (r != 0) {
			errlog("dnscl", "ffdnscl_resolve(): %d", r);
			continue;
		}
		c->njobs++;
	}
	return FCOM_ASYNC;
}
