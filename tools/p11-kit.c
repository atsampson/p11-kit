/*
 * Copyright (c) 2011, Collabora Ltd.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *     * Redistributions of source code must retain the above
 *       copyright notice, this list of conditions and the
 *       following disclaimer.
 *     * Redistributions in binary form must reproduce the
 *       above copyright notice, this list of conditions and
 *       the following disclaimer in the documentation and/or
 *       other materials provided with the distribution.
 *     * The names of contributors to this software may not be
 *       used to endorse or promote products derived from this
 *       software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH
 * DAMAGE.
 *
 * Author: Stef Walter <stefw@collabora.co.uk>
 */

#include "config.h"

#include <assert.h>
#include <err.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "p11-kit.h"

typedef int (*operation) (int argc, char *argv[]);
int verbose = 0;

static void
usage (void)
{
	fprintf (stderr, "usage: p11-kit [-v] -l\n");
	fprintf (stderr, "       p11-kit -h\n");
	exit (2);
}

static int
list_modules (int argc, char *argv[])
{
	CK_FUNCTION_LIST_PTR *module_list;
	char *name;
	char *path;
	CK_RV rv;
	int i;

	if (argc != 0)
		usage ();

	rv = p11_kit_initialize_registered ();
	if (rv != CKR_OK)
		errx (1, "couldn't initialize registered modules: %s",
		      p11_kit_strerror (rv));

	module_list = p11_kit_registered_modules ();
	for (i = 0; module_list[i]; i++) {
		name = p11_kit_registered_module_to_name (module_list[i]);
		path = p11_kit_registered_option (module_list[i], "module");

		printf ("%s: %s\n",
		        name ? name : "(null)",
		        path ? path : "(null)");

		free (name);
		free (path);
	}

	return 0;
}

int
main (int argc, char *argv[])
{
	operation oper = NULL;
	int opt;

	while ((opt = getopt (argc, argv, "lv")) != -1) {
		switch (opt) {
		case 'l':
			if (oper != NULL)
				usage ();
			oper = list_modules;
			break;
		case 'v':
			verbose = 1;
			setenv ("P11_KIT_DEBUG", "all", 1);
			break;
		case 'h':
		case '?':
			usage();
			break;
		}
	}

	if (!oper)
		usage ();

	argc -= optind;
	argv += optind;

	return (oper) (argc, argv);
}