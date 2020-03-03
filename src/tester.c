#include <fcom.h>

extern void test_file_matches();

const fcom_core *core;

int main(int argc, char const *argv[])
{
	ffmem_init();
	test_file_matches();
	fffile_writecz(ffstdout, "DONE");
	return 0;
}
