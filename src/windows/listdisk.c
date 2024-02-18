/** fcom: List logical volumes (Windows)
2023, Simon Zolin */

static const char* listdisk_help()
{
	return "\
List logical volumes.\n\
Usage:\n\
  fcom listdisk\n\
";
}

#include <fcom.h>
#include <ffsys/volume.h>

static const fcom_core *core;

struct listdisk {
	fcom_cominfo cominfo;

	uint st;
	fcom_cominfo *cmd;
	uint stop;
};

static int args_parse(struct listdisk *l, fcom_cominfo *cmd)
{
	static const struct ffarg args[] = {
		{}
	};
	return core->com->args_parse(cmd, args, l, FCOM_COM_AP_INOUT);
}

static void listdisk_close(fcom_op *op)
{
	struct listdisk *l = op;
	ffmem_free(l);
}

static fcom_op* listdisk_create(fcom_cominfo *cmd)
{
	struct listdisk *l = ffmem_new(struct listdisk);
	l->cmd = cmd;

	if (0 != args_parse(l, cmd))
		goto end;

	return l;

end:
	listdisk_close(l);
	return NULL;
}

static int listdisk_print()
{
	int rc = 1;
	ffvec buf = {}, out = {};
	fffd hvol = FFFILE_NULL;
	if (NULL == ffvec_allocT(&buf, MAX_PATH + 1, wchar_t))
		goto end;
	struct ffvol_info vi = {};

	if (FFFILE_NULL == (hvol = ffvol_open((void*)buf.ptr, buf.cap))) {
		fcom_syserrlog("FindFirstVolume");
		goto end;
	}
	for (;;) {
		const wchar_t *volname = (void*)buf.ptr;
		ffvec_addfmt(&out, "%q:  ", volname);

		ffvol_info_destroy(&vi);
		ffmem_zero_obj(&vi);
		ffvol_info(volname, &vi, 0);

		static const char*const types[] = {
			"", "", "removable", "fixed", "remote", "cdrom", "ramdisk"
		};
		ffvec_addfmt(&out, "type:%s  "
			, (vi.type < FF_COUNT(types)) ? types[vi.type] : "");

		if (vi.fs[0] != 0)
			ffvec_addfmt(&out, "fs:%q  ", vi.fs);

		if (vi.sectors_cluster != 0) {
			uint bytes_cluster = vi.sectors_cluster * vi.bytes_sector;
			double free_percent = FFINT_DIVSAFE((double)vi.clusters_free * 100, vi.clusters_total);
			ffvec_addfmt(&out, "(cluster:%u  total:%,U  free:%,U (%.02F%%)) "
				, bytes_cluster
				, (uint64)vi.clusters_total * bytes_cluster
				, (uint64)vi.clusters_free * bytes_cluster, free_percent);
		}

		if (((wchar_t*)vi.paths.ptr)[0] != '\0') {
			for (wchar_t *ws = (void*)vi.paths.ptr;  ws[0] != '\0';  ws += ffwsz_len(ws) + 1) {
				ffvec_addfmt(&out, "%q, ", ws);
			}
		}
		ffvec_addfmt(&out, "\n");

		if (0 != ffvol_next(hvol, (void*)buf.ptr, buf.cap)) {
			if (fferr_last() == FFERR_NOMOREVOLS)
				break;
			fcom_syserrlog("FindNextVolume");
			goto end;
		}
	}

	fcom_infolog("%S", &out);
	rc = 0;

end:
	ffvol_info_destroy(&vi);
	ffvec_free(&buf);
	ffvec_free(&out);
	ffvol_close(hvol);
	return rc;
}

static void listdisk_run(fcom_op *op)
{
	struct listdisk *l = op;
	int r, rc = 1;
	enum { I_PRINT, };

	while (!FFINT_READONCE(l->stop)) {
		switch (l->st) {
		case I_PRINT:
			r = listdisk_print();
			if (r != 0) goto end;
			rc = 0;
			goto end;
		}
	}

end:
	{
	fcom_cominfo *cmd = l->cmd;
	listdisk_close(l);
	core->com->complete(cmd, rc);
	}
}

static void listdisk_signal(fcom_op *op, uint signal)
{
	struct listdisk *l = op;
	FFINT_WRITEONCE(l->stop, 1);
}

static const fcom_operation fcom_op_listdisk = {
	listdisk_create, listdisk_close,
	listdisk_run, listdisk_signal,
	listdisk_help,
};

FCOM_MOD_DEFINE(listdisk, fcom_op_listdisk, core)
