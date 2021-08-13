#include <fcom.h>
#include <FFOS/test.h>

#define x  FFTEST_BOOL
const fcom_core *core;

void test_file_matches()
{
	comm *c = ffmem_new(comm);

	ffstr *dst, s, wc;
	ffarr aincl = {};
	ffstr_setz(&s, "/path");
	while (s.len != 0) {
		ffstr_nextval3(&s, &wc, ';');
		dst = ffarr_pushgrowT(&aincl, 4, ffstr);
		*dst = wc;
	}
	ffarr_set(&c->cmd.include_files, aincl.ptr, aincl.len);

	ffarr aexcl = {};
	ffstr_setz(&s, "*/.git;/path/bin;/path/dir/_bin;*.zip");
	while (s.len != 0) {
		ffstr_nextval3(&s, &wc, ';');
		dst = ffarr_pushgrowT(&aexcl, 4, ffstr);
		*dst = wc;
	}
	ffarr_set(&c->cmd.exclude_files, aexcl.ptr, aexcl.len);

	x(!file_matches(c, "/path2", 0));

	x(file_matches(c, "/path/file", 0));
	x(file_matches(c, "/path/dir", 1));

	x(!file_matches(c, "/path/.git", 1));
	x(file_matches(c, "/path/.git2", 1));
	x(file_matches(c, "/path/1.git", 1));
	x(!file_matches(c, "/path/dir/.git", 1));

	x(!file_matches(c, "/path/bin", 1));
	x(file_matches(c, "/path/binn", 1));
	x(file_matches(c, "/path/dir/bin", 1));

	x(!file_matches(c, "/path/dir/_bin", 1));
	x(file_matches(c, "/path/ddir/_bin", 1));

	x(!file_matches(c, "/path/dir/1.zip", 1));
	x(file_matches(c, "/path/dir/1.zip2", 1));
}

int main(int argc, char const *argv[])
{
	ffmem_init();
	test_file_matches();
	fffile_writecz(ffstdout, "DONE");
	return 0;
}
