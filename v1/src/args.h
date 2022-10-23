/** fcom: command-line arguments: public interface
2022, Simon Zolin */

#pragma once

struct args {
	int argc;
	char **argv;
	byte verbose;
	byte debug;
};

int args_read(struct args *a, uint argc, char **argv);
void args_destroy(struct args *conf);

char* path(const char *fn);
