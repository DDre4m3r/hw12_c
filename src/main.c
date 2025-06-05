#include <stdio.h>
#include <string.h>

#include "parse_args.h"

int main(int argc, char** argv) {
	const char* progname = argv[0];
	const char* slash    = strrchr(progname, '/');
	if (slash != NULL) {
		progname = slash + 1;    // после последнего '/'
	}

	struct server_args args;

	int res = parse_args(argc, argv, &args);
	if (res != 0 || args.show_help) {
		print_help(progname);
		return res == 0 ? 0 : 1;
	}

	printf("Path: %s\n", args.path);
	printf("Address: %s\n", args.addr);
	printf("Port: %d\n", args.port);
	return 0;
}
