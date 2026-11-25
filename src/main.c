/* main.c - entry point */
#include <stdlib.h>
#include <string.h>

#include <openssl/ssl.h>

#include "epis.h"

int main(int argc, char **argv)
{
	const char *conf = NULL;

	argc--;
	argv++;
	if (argc >= 2 && !strcmp(argv[0], "-c")) {
		conf = argv[1];
		argc -= 2;
		argv += 2;
	}
	OPENSSL_init_ssl(0, NULL);
	if (config_load(conf) && argc && strcmp(argv[0], "setup") &&
	    strcmp(argv[0], "help") && strcmp(argv[0], "autoconfig") &&
	    strcmp(argv[0], "version") && strcmp(argv[0], "-h") &&
	    strcmp(argv[0], "--help"))
		die("no configuration at %s, run 'epis setup'", cfg.path);
	if (argc)
		return cli_main(argc, argv);
#ifdef EPIS_GUI
	return gui_main();
#else
	if (!config_exists())
		return setup_wizard_tty();
	return cli_main(1, (char *[]){ "help", NULL });
#endif
}
