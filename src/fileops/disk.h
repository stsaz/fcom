/** fcom: list disks
2021, Simon Zolin
*/

#define FILT_NAME  "disk"

static void* disk_open(fcom_cmd *cmd)
{
	return FCOM_OPEN_DUMMY;
}

static void disk_close(void *p, fcom_cmd *cmd)
{
}

static int disk_process(void *p, fcom_cmd *cmd)
{
#ifdef FF_WIN
	ffbool ok = 0;
	fffd hvol = FF_BADFD;
	ffarr buf = {};
	ffarr names = {};
	ffarr out = {};
	if (NULL == ffarr_allocT(&buf, MAX_PATH, ffsyschar))
		goto end;
	ffarr_allocT(&names, MAX_PATH + 1, ffsyschar);
	ffarr_alloc(&out, 1024);

	if (FF_BADFD == (hvol = FindFirstVolume((void*)buf.ptr, buf.cap))) {
		syserrlog("FindFirstVolume", 0);
		goto end;
	}
	for (;;) {
		const ffsyschar *volname = (void*)buf.ptr;
		ffstr_catfmt(&out, "%q:  ", volname);

		uint type = GetDriveType(volname);
		static const char*const types[] = {
			"", "", "removable", "fixed", "remote", "cdrom", "ramdisk"
		};
		ffstr_catfmt(&out, "type:%s  "
			, (type < FFCNT(types)) ? types[type] : "");

		ffsyschar fs[64];
		if (GetVolumeInformation(volname, NULL, 0, NULL, NULL, NULL, fs, FFCNT(fs)))
			ffstr_catfmt(&out, "fs:%q  ", fs);

		DWORD sectors_cluster, bytes_sector, clusters_free, clusters_total;
		if (GetDiskFreeSpace(volname, &sectors_cluster, &bytes_sector, &clusters_free, &clusters_total)) {
			uint bytes_cluster = sectors_cluster * bytes_sector;
			double free_percent = FFINT_DIVSAFE((double)clusters_free * 100, clusters_total);
			ffstr_catfmt(&out, "(cluster:%u: total:%U  free:%U (%.02F%%)) "
				, bytes_cluster
				, (uint64)clusters_total * bytes_cluster
				, (uint64)clusters_free * bytes_cluster, free_percent);
		}

		for (;;) {
			DWORD size;
			if (!GetVolumePathNamesForVolumeName(volname, (void*)names.ptr, names.cap, &size)) {
				if (fferr_last() == ERROR_MORE_DATA) {
					ffarr_free(&names);
					if (NULL == ffarr_allocT(&names, size, ffsyschar))
						goto end;
					continue;
				}
			} else {
				for (ffsyschar *ws = (void*)names.ptr;  ws[0] != '\0';  ws += ffq_len(ws) + 1) {
					ffstr_catfmt(&out, "%q, ", ws);
				}
				ffstr_catfmt(&out, "\n");
			}
			break;
		}

		if (!FindNextVolume(hvol, (void*)buf.ptr, buf.cap)) {
			if (fferr_last() == ERROR_NO_MORE_FILES)
				break;
			syserrlog("FindNextVolume", 0);
			goto end;
		}
	}

	fcom_userlog("%S", &out);
	ok = 1;

end:
	ffarr_free(&buf);
	ffarr_free(&names);
	ffarr_free(&out);
	if (hvol != FF_BADFD)
		FindVolumeClose(hvol);
	return (ok) ? FCOM_DONE : FCOM_ERR;
#endif
	return FCOM_DONE;
}

#undef FILT_NAME

static const fcom_filter disk_filt = { disk_open, disk_close, disk_process };
