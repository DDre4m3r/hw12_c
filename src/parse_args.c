#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "parse_args.h"

static int parse_port(const char* s) {
	char* end = NULL;
	long  val = strtol(s, &end, 10);
	if (!s[0] || *end || val <= 0 || val > 65535) { return -1; }
	return (int) val;
}

void print_help(const char* progname) {
	printf("Usage: %s --path=<dir> --addr=<address> --port=<port>\n", progname);
	printf("       %s --help\n", progname);
}

int parse_args(int argc, char** argv, struct server_args* args) {
	args->path      = NULL;
	args->addr      = NULL;
	args->port      = 0;
	args->show_help = 0;

	for (int i = 1; i < argc; ++i) {
		if (strncmp(argv[i], "--path=", 7) == 0) {
			args->path = argv[i] + 7;
		} else if (strncmp(argv[i], "--addr=", 7) == 0) {
			args->addr = argv[i] + 7;
		} else if (strncmp(argv[i], "--port=", 7) == 0) {
			int p = parse_port(argv[i] + 7);
			if (p < 0) {
				args->show_help = 1;
				return -1;
			}
			args->port = p;
		} else if (strcmp(argv[i], "--help") == 0) {
			args->show_help = 1;
			return 0;
		} else {
			args->show_help = 1;
			return -1;
		}
	}

	if (!args->path || !args->addr || args->port == 0) {
		args->show_help = 1;
		return -1;
	}

	return 0;
}
