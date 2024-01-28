/** fcom: command-line arguments: public interface
2022, Simon Zolin */

#pragma once

struct args {
	int argc;
	char **argv;
	byte verbose;
	byte debug;
	ffvec args; // const char*[]
};

int args_read(struct args *a, uint argc, char **argv, char *cmd_line);
void args_destroy(struct args *conf);
