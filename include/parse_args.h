#ifndef HW12_PARSE_ARGS_H
#define HW12_PARSE_ARGS_H

struct server_args {
	const char *path;
	const char *addr;
	int port;
	int show_help;
};

int parse_args(int argc, char **argv, struct server_args *args);
void print_help(const char *progname);

#endif    // HW12_PARSE_ARGS_H
