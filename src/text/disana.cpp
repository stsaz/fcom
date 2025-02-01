/** fcom: Analyze disassembler listing
2024, Simon Zolin */

static const char* disana_help()
{
	return "\
Analyze disassembler listing\n\
Usage:\n\
  objdump -d ... | `fcom disana`\n\
";
}

#include <fcom.h>
#include <util/util.hpp>
#include <ffsys/std.h>
#include <ffsys/globals.h>

static const fcom_core *core;

struct item {
	ffstr name;
	uint off, size, calltree_size;
	xxvec call_names; // ffstr[]
};

struct disana {
	fcom_cominfo cominfo;
	uint stop;
	fcom_cominfo *cmd;

	fcom_filexx input;
	xxvec items; // struct item[]
	xxvec data;

	disana() : input(core)
	{}

	int read_input()
	{
		int r = this->input.open("", FCOM_FILE_READ | FCOM_FILE_STDIN);
		if (r != FCOM_FILE_OK) return -1;

		for (;;) {
			ffstr s;
			r = this->input.read(&s, -1);
			if (r == FCOM_FILE_EOF) break;
			if (r != FCOM_FILE_OK) return -1;
			this->data.add(s);
		}
		return 0;
	}

	void parse(xxstr d)
	{
		uint addr_prev = 0, addr;
		xxstr line, name_prev, name, op;
		xxvec calls;

		while (d.len) {
			d.split_by('\n', &line, &d);

			if (line.len && line.ptr[0] == '0') {
				int r = line.match_f("%xu <%S>:"
					, &addr, &name);
				if (r == 0) {
					fcom_dbglog("%S %xu", &name, addr);

					if (addr_prev) {
						struct item *e = this->items.push_z<struct item>();
						e->name = name_prev;
						e->off = addr_prev;
						e->calltree_size = ~0U;
						e->size = addr - addr_prev;
						e->call_names = calls;
						calls.reset();
					}

					addr_prev = addr;
					name_prev = name;
				}

			} else if (line.len && line.ptr[0] == ' ') {
				line.split_by('\t', NULL, &line); // skip address
				line.split_by('\t', NULL, &op); // skip binary
				int r = op.match_f("call   %xu <%S>"
					, &addr, &name);
				if (r == 0) {
					fcom_dbglog("call %xu %S", addr, &name);

					int found = 0;
					xxstr *c;
					FFSLICE_WALK(&calls, c) {
						if (c->equals(name)) {
							found = 1;
							break;
						}
					}
					if (!found && !name.equals(name_prev))
						*calls.push<ffstr>() = name;
				}
			}
		}
	}

	struct item* item_by_name(xxstr name)
	{
		struct item *it;
		FFSLICE_WALK(&this->items, it) {
			if (name.equals(it->name))
				return it;
		}
		return NULL;
	}

	uint item_calltree_size(struct item *it)
	{
		uint n = 0;
		xxstr *s;
		FFSLICE_WALK(&it->call_names, s) {
			struct item *si = this->item_by_name(*s);
			if (si) {
				if (si->calltree_size == ~0U)
					si->calltree_size = this->item_calltree_size(si);
				n += si->size + si->calltree_size;
			}
		}
		return n;
	}

	void calltree_size()
	{
		struct item *it;
		FFSLICE_WALK(&this->items, it) {
			if (it->calltree_size == ~0U)
				it->calltree_size = this->item_calltree_size(it);
		}
	}

	static int sort_cmp_func(const void *a, const void *b, void *udata)
	{
		const struct item *aa = *(struct item**)a, *bb = *(struct item**)b;
		uint as = aa->size + aa->calltree_size;
		uint bs = bb->size + bb->calltree_size;
		if (as == bs)
			return 0;
		else if (as < bs)
			return 1;
		return -1;
	}

	void display_results()
	{
		xxvec index;

		struct item *it;
		FFSLICE_WALK(&this->items, it) {
			*index.push<struct item*>() = it;
		}
		ffsort(index.ptr, index.len, sizeof(void*), sort_cmp_func, NULL);

		xxvec v;
		v.add_f("Subtree Size | Self Size | Offset | Name\n");

		struct item **p;
		FFSLICE_WALK(&index, p) {
			const struct item *it = *p;

			v.add_f("%8u  %8u  %8xu  %S\n"
				, it->calltree_size, it->size, it->off, &it->name);

			if (core->stdout_color)
				v.add_f("%s", FFSTD_CLR_I(FFSTD_BLUE));

			xxstr *s;
			FFSLICE_WALK(&it->call_names, s) {
				struct item *si = this->item_by_name(*s);
				if (si) {
					v.add_f("%8u  %8u  %8xu   \\_ %S\n"
						, si->calltree_size, si->size, si->off, &si->name);
				}
			}

			if (core->stdout_color)
				v.add_f("%s", FFSTD_CLR_RESET);
		}

		ffstdout_write(v.str().ptr, v.str().len);
	}
};

static void disana_close(fcom_op *op)
{
	struct disana *d = (struct disana*)op;
	d->~disana();
	ffmem_free(d);
}

static fcom_op* disana_create(fcom_cominfo *cmd)
{
	struct disana *d = new(ffmem_new(struct disana)) struct disana;
	d->cmd = cmd;

	static const struct ffarg args[] = {
		{}
	};
	if (core->com->args_parse(cmd, args, d, FCOM_COM_AP_INOUT)) {
		disana_close(d);
		return NULL;
	}

	struct fcom_file_conf fc = {};
	fc.n_buffers = 1;
	d->input.create(&fc);

	return d;
}

static void disana_run(fcom_op *op)
{
	struct disana *d = (struct disana*)op;
	while (!FFINT_READONCE(d->stop)) {

		if (d->read_input())
			goto end;
		d->parse(d->data.str());
		d->calltree_size();
		d->display_results();
		break;
	}

end:
	{
	fcom_cominfo *cmd = d->cmd;
	disana_close(d);
	core->com->complete(cmd, 0);
	}
}

static void disana_signal(fcom_op *op, uint signal)
{
	struct disana *d = (struct disana*)op;
	FFINT_WRITEONCE(d->stop, 1);
}

static const fcom_operation fcom_op_disana = {
	disana_create, disana_close,
	disana_run, disana_signal,
	disana_help,
};

FCOM_MOD_DEFINE(disana, fcom_op_disana, core)
