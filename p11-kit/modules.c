/*
 * Copyright (C) 2008 Stefan Walter
 * Copyright (C) 2011 Collabora Ltd.
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

#include "conf.h"
#define DEBUG_FLAG DEBUG_LIB
#include "debug.h"
#include "hash.h"
#include "pkcs11.h"
#include "p11-kit.h"
#include "private.h"

#include <sys/stat.h>
#include <sys/types.h>

#include <assert.h>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <pwd.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/**
 * SECTION:p11-kit
 * @title: Modules
 * @short_description: Module loading and initializing
 *
 * PKCS\#11 modules are used by crypto libraries and applications to access
 * crypto objects (like keys and certificates) and to perform crypto operations.
 *
 * In order for applications to behave consistently with regard to the user's
 * installed PKCS\#11 modules, each module must be registered so that applications
 * or libraries know that they should load it.
 *
 * The functions here provide support for initializing registered modules. The
 * p11_kit_initialize_registered() function should be used to load and initialize
 * the registered modules. When done, the p11_kit_finalize_registered() function
 * should be used to release those modules and associated resources.
 *
 * In addition p11_kit_registered_option() can be used to access other parts
 * of the module configuration.
 *
 * When multiple consumers of a module (such as libraries or applications) are
 * in the same process, coordination of the initialization and finalization
 * of PKCS\#11 modules is required. The functions here automatically provide
 * initialization reference counting to make this work.
 *
 * If a consumer wishes to load an arbitrary PKCS\#11 module that's not
 * registered, that module should be initialized with p11_kit_initialize_module()
 * and finalized with p11_kit_finalize_module(). The module's own
 * <code>C_Initialize</code> and <code>C_Finalize</code> methods should not
 * be called directly.
 *
 * Modules are represented by a pointer to their <code>CK_FUNCTION_LIST</code>
 * entry points. This means that callers can load modules elsewhere, using
 * dlopen() for example, and then still use these methods on them.
 */

typedef struct _Module {
	CK_FUNCTION_LIST_PTR funcs;
	int ref_count;

	/* Registered modules */
	char *name;
	hash_t *config;

	/* Loaded modules */
	void *dl_module;
	int dlopen_count;

	/* Initialized modules */
	CK_C_INITIALIZE_ARGS init_args;
	int initialize_count;
} Module;

/*
 * This is the mutex that protects the global data of this library
 * and the pkcs11 proxy module. Note that we *never* call into our
 * underlying pkcs11 modules while holding this mutex. Therefore it
 * doesn't have to be recursive and we can keep things simple.
 */
pthread_mutex_t _p11_mutex = PTHREAD_MUTEX_INITIALIZER;

/*
 * Shared data between threads, protected by the mutex, a structure so
 * we can audit thread safety easier.
 */
static struct _Shared {
	hash_t *modules;
	hash_t *config;
} gl = { NULL, NULL };

/* -----------------------------------------------------------------------------
 * UTILITIES
 */

static void
warning (const char* msg, ...)
{
	char buffer[512];
	va_list va;

	va_start (va, msg);

	vsnprintf(buffer, sizeof (buffer) - 1, msg, va);
	buffer[sizeof (buffer) - 1] = 0;
	fprintf (stderr, "p11-kit: %s\n", buffer);

	va_end (va);
}

static void
conf_error (const char *buffer)
{
	/* called from conf.c */
	fprintf (stderr, "p11-kit: %s\n", buffer);
}

static char*
strconcat (const char *first, ...)
{
	size_t length = 0;
	const char *arg;
	char *result, *at;
	va_list va;

	va_start (va, first);

	for (arg = first; arg; arg = va_arg (va, const char*))
		length += strlen (arg);

	va_end (va);

	at = result = malloc (length);
	if (!result)
		return NULL;

	va_start (va, first);

	for (arg = first; arg; arg = va_arg (va, const char*)) {
		length = strlen (arg);
		memcpy (at, arg, length);
		at += length;
	}

	va_end (va);

	*at = 0;
	return result;
}

static int
strequal (const char *one, const char *two)
{
	return strcmp (one, two) == 0;
}

/* -----------------------------------------------------------------------------
 * P11-KIT FUNCTIONALITY
 */

static CK_RV
create_mutex (CK_VOID_PTR_PTR mut)
{
	pthread_mutex_t *pmutex;
	int err;

	if (mut == NULL)
		return CKR_ARGUMENTS_BAD;

	pmutex = malloc (sizeof (pthread_mutex_t));
	if (!pmutex)
		return CKR_HOST_MEMORY;
	err = pthread_mutex_init (pmutex, NULL);
	if (err == ENOMEM)
		return CKR_HOST_MEMORY;
	else if (err != 0)
		return CKR_GENERAL_ERROR;
	*mut = pmutex;
	return CKR_OK;
}

static CK_RV
destroy_mutex (CK_VOID_PTR mut)
{
	pthread_mutex_t *pmutex = mut;
	int err;

	if (mut == NULL)
		return CKR_MUTEX_BAD;

	err = pthread_mutex_destroy (pmutex);
	if (err == EINVAL)
		return CKR_MUTEX_BAD;
	else if (err != 0)
		return CKR_GENERAL_ERROR;
	free (pmutex);
	return CKR_OK;
}

static CK_RV
lock_mutex (CK_VOID_PTR mut)
{
	pthread_mutex_t *pmutex = mut;
	int err;

	if (mut == NULL)
		return CKR_MUTEX_BAD;

	err = pthread_mutex_lock (pmutex);
	if (err == EINVAL)
		return CKR_MUTEX_BAD;
	else if (err != 0)
		return CKR_GENERAL_ERROR;
	return CKR_OK;
}

static CK_RV
unlock_mutex (CK_VOID_PTR mut)
{
	pthread_mutex_t *pmutex = mut;
	int err;

	if (mut == NULL)
		return CKR_MUTEX_BAD;

	err = pthread_mutex_unlock (pmutex);
	if (err == EINVAL)
		return CKR_MUTEX_BAD;
	else if (err == EPERM)
		return CKR_MUTEX_NOT_LOCKED;
	else if (err != 0)
		return CKR_GENERAL_ERROR;
	return CKR_OK;
}

static void
free_module_unlocked (void *data)
{
	Module *mod = data;

	assert (mod);

	/* Module must be finalized */
	assert (mod->initialize_count == 0);

	/* Module must have no outstanding references */
	assert (mod->ref_count == 0);

	if (mod->dl_module)
		dlclose (mod->dl_module);
	hash_free (mod->config);
	free (mod->name);
	free (mod);
}

static Module*
alloc_module_unlocked (void)
{
	Module *mod;

	mod = calloc (1, sizeof (Module));
	if (!mod)
		return NULL;

	mod->init_args.CreateMutex = create_mutex;
	mod->init_args.DestroyMutex = destroy_mutex;
	mod->init_args.LockMutex = lock_mutex;
	mod->init_args.UnlockMutex = unlock_mutex;
	mod->init_args.flags = CKF_OS_LOCKING_OK;

	return mod;
}

static CK_RV
dlopen_and_get_function_list (Module *mod, const char *path)
{
	CK_C_GetFunctionList gfl;
	CK_RV rv;

	assert (mod);
	assert (path);

	mod->dl_module = dlopen (path, RTLD_LOCAL | RTLD_NOW);
	if (mod->dl_module == NULL) {
		warning ("couldn't load module: %s: %s", path, dlerror ());
		return CKR_GENERAL_ERROR;
	}

	mod->dlopen_count++;

	gfl = dlsym (mod->dl_module, "C_GetFunctionList");
	if (!gfl) {
		warning ("couldn't find C_GetFunctionList entry point in module: %s: %s",
		         path, dlerror ());
		return CKR_GENERAL_ERROR;
	}

	rv = gfl (&mod->funcs);
	if (rv != CKR_OK) {
		warning ("call to C_GetFunctiontList failed in module: %s: %s",
		         path, p11_kit_strerror (rv));
		return rv;
	}

	return CKR_OK;
}

static CK_RV
load_module_from_file_unlocked (const char *path, Module **result)
{
	Module *mod;
	Module *prev;
	CK_RV rv;

	mod = alloc_module_unlocked ();
	if (!mod)
		return CKR_HOST_MEMORY;

	rv = dlopen_and_get_function_list (mod, path);
	if (rv != CKR_OK) {
		free_module_unlocked (mod);
		return rv;
	}

	/* Do we have a previous one like this, if so ignore load */
	prev = hash_get (gl.modules, mod->funcs);

	if (prev != NULL) {
		free_module_unlocked (mod);
		mod = prev;

	} else if (!hash_set (gl.modules, mod->funcs, mod)) {
		free_module_unlocked (mod);
		return CKR_HOST_MEMORY;
	}

	if (result)
		*result= mod;
	return CKR_OK;
}

static CK_RV
load_module_from_config_unlocked (const char *configfile, const char *name,
                                  Module **result)
{
	Module *mod, *prev;
	const char *path;
	CK_RV rv;

	assert (configfile);

	mod = alloc_module_unlocked ();
	if (!mod)
		return CKR_HOST_MEMORY;

	mod->config = conf_parse_file (configfile, 0, conf_error);
	if (!mod->config) {
		free_module_unlocked (mod);
		if (errno == ENOMEM)
			return CKR_HOST_MEMORY;
		return CKR_GENERAL_ERROR;
	}

	mod->name = strdup (name);
	if (!mod->name) {
		free_module_unlocked (mod);
		return CKR_HOST_MEMORY;
	}

	path = hash_get (mod->config, "module");
	if (path == NULL) {
		free_module_unlocked (mod);
		warning ("no module path specified in config: %s", configfile);
		return CKR_GENERAL_ERROR;
	}

	rv = dlopen_and_get_function_list (mod, path);
	if (rv != CKR_OK) {
		free_module_unlocked (mod);
		return rv;
	}

	prev = hash_get (gl.modules, mod->funcs);

	/* Replace previous module that was loaded explicitly? */
	if (prev && !prev->name) {
		mod->ref_count = prev->ref_count;
		mod->initialize_count = prev->initialize_count;
		prev->ref_count = 0;
		prev->initialize_count = 0;
		prev = NULL; /* freed by hash_set below */
	}

	/* Refuse to load duplicate module */
	if (prev) {
		warning ("duplicate configured module: %s: %s", mod->name, path);
		free_module_unlocked (mod);
		return CKR_GENERAL_ERROR;
	}

	/*
	 * We support setting of CK_C_INITIALIZE_ARGS.pReserved from
	 * 'x-init-reserved' setting in the config. This only works with specific
	 * PKCS#11 modules, and is non-standard use of that field.
	 */
	mod->init_args.pReserved = hash_get (mod->config, "x-init-reserved");

	if (!hash_set (gl.modules, mod->funcs, mod)) {
		free_module_unlocked (mod);
		return CKR_HOST_MEMORY;
	}

	if (result)
		*result = mod;
	return CKR_OK;
}

static CK_RV
load_modules_from_config_unlocked (const char *directory)
{
	struct dirent *dp;
	struct stat st;
	CK_RV rv = CKR_OK;
	DIR *dir;
	int is_dir;
	char *path;

	debug ("loading module configs in: %s", directory);

	/* First we load all the modules */
	dir = opendir (directory);
	if (!dir) {
		if (errno == ENOENT || errno == ENOTDIR)
			warning ("couldn't list directory: %s", directory);
		return CKR_GENERAL_ERROR;
	}

	/* We're within a global mutex, so readdir is safe */
	while ((dp = readdir(dir)) != NULL) {
		path = strconcat (directory, "/", dp->d_name, NULL);
		if (!path) {
			rv = CKR_HOST_MEMORY;
			break;
		}

		is_dir = 0;
#ifdef HAVE_STRUCT_DIRENT_D_TYPE
		if(dp->d_type != DT_UNKNOWN) {
			is_dir = (dp->d_type == DT_DIR);
		} else
#endif
		{
			if (stat (path, &st) < 0) {
				warning ("couldn't stat path: %s", path);
				free (path);
				rv = CKR_GENERAL_ERROR;
				break;
			}
			is_dir = S_ISDIR (st.st_mode);
		}

		if (is_dir)
			rv = CKR_OK;
		else
			rv = load_module_from_config_unlocked (path, dp->d_name, NULL);

		free (path);

		if (rv != CKR_OK)
			break;
	}

	closedir (dir);

	return rv;
}

static char*
expand_user_path (const char *path)
{
	const char *env;
	struct passwd *pwd;

	if (path[0] == '~' && path[1] == '/') {
		env = getenv ("HOME");
		if (env && env[0]) {
			return strconcat (env, path + 1, NULL);
		} else {
			pwd = getpwuid (getuid ());
			if (!pwd)
				return NULL;
			return strconcat (pwd->pw_dir, path + 1, NULL);
		}
	}

	return strdup (path);
}

enum {
	USER_CONFIG_INVALID = 0,
	USER_CONFIG_NONE = 1,
	USER_CONFIG_MERGE,
	USER_CONFIG_OVERRIDE
};

static int
user_config_mode (hash_t *config, int defmode)
{
	const char *mode;

	/* Whether we should use or override from user directory */
	mode = hash_get (config, "user-config");
	if (mode == NULL) {
		return defmode;
	} else if (strequal (mode, "none")) {
		return USER_CONFIG_NONE;
	} else if (strequal (mode, "merge")) {
		return USER_CONFIG_MERGE;
	} else if (strequal (mode, "override")) {
		return USER_CONFIG_OVERRIDE;
	} else {
		warning ("invalid mode for 'user-config': %s", mode);
		return USER_CONFIG_INVALID;
	}
}

static CK_RV
load_config_files_unlocked (int *user_mode)
{
	hash_t *config = NULL;
	hash_t *uconfig = NULL;
	void *key = NULL;
	void *value = NULL;
	char *path;
	int mode;
	CK_RV rv = CKR_GENERAL_ERROR;
	hash_iter_t hi;

	/* Should only be called after everything has been unloaded */
	assert (!gl.config);

	/* Load the main configuration */
	config = conf_parse_file (P11_SYSTEM_CONF, CONF_IGNORE_MISSING, conf_error);
	if (!config) {
		rv = (errno == ENOMEM) ? CKR_HOST_MEMORY : CKR_GENERAL_ERROR;
		goto finished;
	}

	/* Whether we should use or override from user directory */
	mode = user_config_mode (config, USER_CONFIG_NONE);
	if (mode == USER_CONFIG_INVALID)
		goto finished;

	if (mode != USER_CONFIG_NONE) {
		path = expand_user_path (P11_USER_CONF);
		if (!path)
			goto finished;

		/* Load up the user configuration */
		uconfig = conf_parse_file (path, CONF_IGNORE_MISSING, conf_error);
		free (path);

		if (!uconfig) {
			rv = (errno == ENOMEM) ? CKR_HOST_MEMORY : CKR_GENERAL_ERROR;
			goto finished;
		}

		/* Figure out what the user mode is */
		mode = user_config_mode (uconfig, mode);
		if (mode == USER_CONFIG_INVALID)
			goto finished;

		/* Merge everything into the system config */
		if (mode == USER_CONFIG_MERGE) {
			hash_iterate (uconfig, &hi);
			while (hash_next (&hi, &key, &value)) {
				key = strdup (key);
				if (key == NULL)
					goto finished;
				value = strdup (value);
				if (value == NULL)
					goto finished;
				if (!hash_set (config, key, value))
					goto finished;
				key = NULL;
				value = NULL;
			}

		/* Override the system config */
		} else if (mode == USER_CONFIG_OVERRIDE) {
			hash_free (config);
			config = uconfig;
			uconfig = NULL;
		}
	}

	gl.config = config;
	config = NULL;
	rv = CKR_OK;

	if (user_mode)
		*user_mode = mode;

finished:
	hash_free (config);
	hash_free (uconfig);
	free (key);
	free (value);
	return rv;
}

static CK_RV
load_registered_modules_unlocked (void)
{
	char *path;
	int mode;
	CK_RV rv;

	rv = load_config_files_unlocked (&mode);
	if (rv != CKR_OK)
		return rv;

	assert (gl.config);
	assert (mode != USER_CONFIG_INVALID);

	/* Load each module from the main list */
	if (mode != USER_CONFIG_OVERRIDE) {
		rv = load_modules_from_config_unlocked (P11_SYSTEM_MODULES);
		if (rv != CKR_OK);
			return rv;
	}

	/* Load each module from the user list */
	if (mode != USER_CONFIG_NONE) {
		path = expand_user_path (P11_USER_MODULES);
		if (!path)
			rv = CKR_GENERAL_ERROR;
		else
			rv = load_modules_from_config_unlocked (path);
		free (path);
		if (rv != CKR_OK);
			return rv;
	}

	return CKR_OK;
}

static CK_RV
initialize_module_unlocked_reentrant (Module *mod)
{
	CK_RV rv = CKR_OK;

	assert (mod);

	/*
	 * Initialize first, so module doesn't get freed out from
	 * underneath us when the mutex is unlocked below.
	 */
	++mod->ref_count;

	if (!mod->initialize_count) {

		_p11_unlock ();

			assert (mod->funcs);
			rv = mod->funcs->C_Initialize (&mod->init_args);

		_p11_lock ();

		/*
		 * Because we have the mutex unlocked above, two initializes could
		 * race. Therefore we need to take CKR_CRYPTOKI_ALREADY_INITIALIZED
		 * into account.
		 *
		 * We also need to take into account where in a race both calls return
		 * CKR_OK (which is not according to the spec but may happen, I mean we
		 * do it in this module, so it's not unimaginable).
		 */

		if (rv == CKR_OK)
			++mod->initialize_count;
		else if (rv == CKR_CRYPTOKI_ALREADY_INITIALIZED)
			rv = CKR_OK;
		else
			--mod->ref_count;
	}

	return rv;
}

static void
reinitialize_after_fork (void)
{
	hash_iter_t it;
	Module *mod;

	/* WARNING: This function must be reentrant */
	debug ("forked");

	_p11_lock ();

		if (gl.modules) {
			hash_iterate (gl.modules, &it);
			while (hash_next (&it, NULL, (void**)&mod)) {
				mod->initialize_count = 0;

				/* WARNING: Reentrancy can occur here */
				initialize_module_unlocked_reentrant (mod);
			}
		}

	_p11_unlock ();

	_p11_kit_proxy_after_fork ();
}

static CK_RV
init_globals_unlocked (void)
{
	static int once = 0;

	if (!gl.modules)
		gl.modules = hash_create (hash_direct_hash, hash_direct_equal,
		                          NULL, free_module_unlocked);
	if (!gl.modules)
		return CKR_HOST_MEMORY;

	if (once)
		return CKR_OK;

	pthread_atfork (NULL, NULL, reinitialize_after_fork);
	once = 1;

	return CKR_OK;
}

static void
free_modules_when_no_refs_unlocked (void)
{
	Module *mod;
	hash_iter_t it;

	/* Check if any modules have a ref count */
	hash_iterate (gl.modules, &it);
	while (hash_next (&it, NULL, (void**)&mod)) {
		if (mod->ref_count)
			return;
	}

	hash_free (gl.modules);
	gl.modules = NULL;
	hash_free (gl.config);
	gl.config = NULL;
}

static CK_RV
finalize_module_unlocked_reentrant (Module *mod)
{
	assert (mod);

	/*
	 * We leave module info around until all are finalized
	 * so we can encounter these zombie Module structures.
	 */
	if (mod->ref_count == 0)
		return CKR_ARGUMENTS_BAD;

	if (--mod->ref_count > 0)
		return CKR_OK;

	/*
	 * Becuase of the mutex unlock below, we temporarily increase
	 * the ref count. This prevents module from being freed out
	 * from ounder us.
	 */
	++mod->ref_count;

	while (mod->initialize_count > 0) {

		_p11_unlock ();

			assert (mod->funcs);
			mod->funcs->C_Finalize (NULL);

		_p11_lock ();

		if (mod->initialize_count > 0)
			--mod->initialize_count;
	}

	/* Match the increment above */
	--mod->ref_count;

	free_modules_when_no_refs_unlocked ();
	return CKR_OK;
}

static Module*
find_module_for_name_unlocked (const char *name)
{
	Module *mod;
	hash_iter_t it;

	assert (name);

	hash_iterate (gl.modules, &it);
	while (hash_next (&it, NULL, (void**)&mod))
		if (mod->ref_count && mod->name && strcmp (name, mod->name) == 0)
			return mod;
	return NULL;
}

CK_RV
_p11_kit_initialize_registered_unlocked_reentrant (void)
{
	Module *mod;
	hash_iter_t it;
	CK_RV rv;

	rv = init_globals_unlocked ();
	if (rv == CKR_OK)
		rv = load_registered_modules_unlocked ();
	if (rv == CKR_OK) {
		hash_iterate (gl.modules, &it);
		while (hash_next (&it, NULL, (void**)&mod)) {

			/* Skip all modules that aren't registered */
			if (!mod->name)
				continue;

			rv = initialize_module_unlocked_reentrant (mod);

			if (rv != CKR_OK) {
				debug ("failed to initialize module: %s: %s",
				       mod->name, p11_kit_strerror (rv));
				break;
			}
		}
	}

	return rv;
}

/**
 * p11_kit_initialize_registered:
 *
 * Initialize all the registered PKCS\#11 modules.
 *
 * If this is the first time this function is called multiple times
 * consecutively within a single process, then it merely increments an
 * initialization reference count for each of these modules.
 *
 * Use p11_kit_finalize_registered() to finalize these registered modules once
 * the caller is done with them.
 *
 * Returns: CKR_OK if the initialization succeeded, or an error code.
 */
CK_RV
p11_kit_initialize_registered (void)
{
	CK_RV rv;

	/* WARNING: This function must be reentrant */
	debug ("in");

	_p11_lock ();

		/* WARNING: Reentrancy can occur here */
		rv = _p11_kit_initialize_registered_unlocked_reentrant ();

	_p11_unlock ();

	/* Cleanup any partial initialization */
	if (rv != CKR_OK)
		p11_kit_finalize_registered ();

	debug ("out: %lu");
	return rv;
}

CK_RV
_p11_kit_finalize_registered_unlocked_reentrant (void)
{
	Module *mod;
	hash_iter_t it;
	Module **to_finalize;
	int i, count;

	if (!gl.modules)
		return CKR_CRYPTOKI_NOT_INITIALIZED;

	/* WARNING: This function must be reentrant */

	to_finalize = calloc (hash_count (gl.modules), sizeof (Module*));
	if (!to_finalize)
		return CKR_HOST_MEMORY;

	count = 0;
	hash_iterate (gl.modules, &it);
	while (hash_next (&it, NULL, (void**)&mod)) {

		/* Skip all modules that aren't registered */
		if (mod->name)
			to_finalize[count++] = mod;
	}

	debug ("finalizing %d modules", count);

	for (i = 0; i < count; ++i) {
		/* WARNING: Reentrant calls can occur here */
		finalize_module_unlocked_reentrant (to_finalize[i]);
	}

	free (to_finalize);
	return CKR_OK;
}

/**
 * p11_kit_finalize_registered:
 *
 * Finalize all the registered PKCS\#11 modules. These should have been
 * initialized with p11_kit_initialize_registered().
 *
 * If p11_kit_initialize_registered() has been called more than once in this
 * process, then this function must be called the same number of times before
 * actual finalization will occur.
 *
 * Returns: CKR_OK if the finalization succeeded, or an error code.
 */

CK_RV
p11_kit_finalize_registered (void)
{
	CK_RV rv;

	/* WARNING: This function must be reentrant */
	debug ("in");

	_p11_lock ();

		/* WARNING: Reentrant calls can occur here */
		rv = _p11_kit_finalize_registered_unlocked_reentrant ();

	_p11_unlock ();

	debug ("out: %lu", rv);
	return rv;
}

CK_FUNCTION_LIST_PTR_PTR
_p11_kit_registered_modules_unlocked (void)
{
	CK_FUNCTION_LIST_PTR_PTR result;
	Module *mod;
	hash_iter_t it;
	int i = 0;

	result = calloc (hash_count (gl.modules) + 1, sizeof (CK_FUNCTION_LIST_PTR));
	if (result) {
		hash_iterate (gl.modules, &it);
		while (hash_next (&it, NULL, (void**)&mod)) {
			if (mod->ref_count && mod->name)
				result[i++] = mod->funcs;
		}
	}

	return result;
}

/**
 * p11_kit_registered_modules:
 *
 * Get a list of all the registered PKCS\#11 modules. This list will be valid
 * once the p11_kit_initialize_registered() function has been called.
 *
 * The returned value is a <code>NULL</code> terminated array of
 * <code>CK_FUNCTION_LIST_PTR</code> pointers.
 *
 * Returns: A list of all the registered modules. Use the free() function to
 * free the list.
 */
CK_FUNCTION_LIST_PTR_PTR
p11_kit_registered_modules (void)
{
	CK_FUNCTION_LIST_PTR_PTR result;

	_p11_lock ();

		result = _p11_kit_registered_modules_unlocked ();

	_p11_unlock ();

	return result;
}

/**
 * p11_kit_registered_module_to_name:
 * @module: pointer to a registered module
 *
 * Get the name of a registered PKCS\#11 module.
 *
 * You can use p11_kit_registered_modules() to get a list of all the registered
 * modules. This name is specified by the registered module configuration.
 *
 * Returns: A newly allocated string containing the module name, or
 *     <code>NULL</code> if no such registered module exists. Use free() to
 *     free this string.
 */
char*
p11_kit_registered_module_to_name (CK_FUNCTION_LIST_PTR module)
{
	Module *mod;
	char *name = NULL;

	if (!module)
		return NULL;

	_p11_lock ();

		mod = gl.modules ? hash_get (gl.modules, module) : NULL;
		if (mod && mod->name)
			name = strdup (mod->name);

	_p11_unlock ();

	return name;
}

/**
 * p11_kit_registered_name_to_module:
 * @name: name of a registered module
 *
 * Lookup a registered PKCS\#11 module by its name. This name is specified by
 * the registered module configuration.
 *
 * Returns: a pointer to a PKCS\#11 module, or <code>NULL</code> if this name was
 *     not found.
 */
CK_FUNCTION_LIST_PTR
p11_kit_registered_name_to_module (const char *name)
{
	CK_FUNCTION_LIST_PTR module = NULL;
	Module *mod;

	_p11_lock ();

		if (gl.modules) {
			mod = find_module_for_name_unlocked (name);
			if (mod)
				module = mod->funcs;
		}

	_p11_unlock ();

	return module;
}

/**
 * p11_kit_registered_option:
 * @module: a pointer to a registered module
 * @field: the name of the option to lookup.
 *
 * Lookup a configured option for a registered PKCS\#11 module. If a
 * <code>NULL</code> module argument is specified, then this will lookup
 * the configuration option in the global config file.
 *
 * Returns: A newly allocated string containing the option value, or
 *     <code>NULL</code> if the registered module or the option were not found.
 *     Use free() to free the returned string.
 */
char*
p11_kit_registered_option (CK_FUNCTION_LIST_PTR module, const char *field)
{
	Module *mod = NULL;
	char *option = NULL;
	hash_t *config = NULL;

	if (!field)
		return NULL;

	_p11_lock ();

		if (module == NULL) {
			config = gl.config;

		} else {
			mod = gl.modules ? hash_get (gl.modules, module) : NULL;
			if (mod)
				config = mod->config;
		}

		if (config) {
			option = hash_get (config, field);
			if (option)
				option = strdup (option);
		}

	_p11_unlock ();

	return option;
}

/**
 * p11_kit_initialize_module:
 * @module: loaded module to initialize.
 *
 * Initialize an arbitrary PKCS\#11 module. Normally using the
 * p11_kit_initialize_registered() is preferred.
 *
 * Using this function to initialize modules allows coordination between
 * multiple users of the same module in a single process. It should be called
 * on modules that have been loaded (with dlopen() for example) but not yet
 * initialized. The caller should not yet have called the module's
 * <code>C_Initialize</code> method. This function will call
 * <code>C_Initialize</code> as necessary.
 *
 * Subsequent calls to this function for the same module will result in an
 * initialization count being incremented for the module. It is safe (although
 * usually unnecessary) to use this function on registered modules.
 *
 * The module must be finalized with p11_kit_finalize_module() instead of
 * calling its <code>C_Finalize</code> method directly.
 *
 * This function does not accept a <code>CK_C_INITIALIZE_ARGS</code> argument.
 * Custom initialization arguments cannot be supported when multiple consumers
 * load the same module.
 *
 * Returns: CKR_OK if the initialization was successful.
 */
CK_RV
p11_kit_initialize_module (CK_FUNCTION_LIST_PTR module)
{
	Module *mod;
	Module *allocated = NULL;
	CK_RV rv = CKR_OK;

	/* WARNING: This function must be reentrant for the same arguments */
	debug ("in");

	_p11_lock ();

		rv = init_globals_unlocked ();
		if (rv == CKR_OK) {

			mod = hash_get (gl.modules, module);
			if (mod == NULL) {
				debug ("allocating new module");
				allocated = mod = alloc_module_unlocked ();
				mod->funcs = module;
			}

			/* WARNING: Reentrancy can occur here */
			rv = initialize_module_unlocked_reentrant (mod);

			/* If this was newly allocated, add it to the list */
			if (rv == CKR_OK && allocated) {
				hash_set (gl.modules, allocated->funcs, allocated);
				allocated = NULL;
			}

			free (allocated);
		}

	_p11_unlock ();

	debug ("out: %lu", rv);
	return rv;
}

/**
 * p11_kit_finalize_module:
 * @module: loaded module to finalize.
 *
 * Finalize an arbitrary PKCS\#11 module. The module must have been initialized
 * using p11_kit_initialize_module(). In most cases callers will want to use
 * p11_kit_finalize_registered() instead of this function.
 *
 * Using this function to finalize modules allows coordination between
 * multiple users of the same module in a single process. The caller should
 * call the module's <code>C_Finalize</code> method. This function will call
 * <code>C_Finalize</code> as necessary.
 *
 * If the module was initialized more than once, then this function will
 * decrement an initialization count for the module. When the count reaches zero
 * the module will be truly finalized. It is safe (although usually unnecessary)
 * to use this function on registered modules if (and only if) they were
 * initialized using p11_kit_initialize_module() for some reason.
 *
 * Returns: CKR_OK if the finalization was successful.
 */
CK_RV
p11_kit_finalize_module (CK_FUNCTION_LIST_PTR module)
{
	Module *mod;
	CK_RV rv = CKR_OK;

	/* WARNING: This function must be reentrant for the same arguments */
	debug ("in");

	_p11_lock ();

		mod = gl.modules ? hash_get (gl.modules, module) : NULL;
		if (mod == NULL) {
			debug ("module not found");
			rv = CKR_ARGUMENTS_BAD;
		} else {
			/* WARNING: Rentrancy can occur here */
			rv = finalize_module_unlocked_reentrant (mod);
		}

	_p11_unlock ();

	debug ("out: %lu", rv);
	return rv;
}

/**
 * p11_kit_load_initialize_module:
 * @module_path: full file path of module library
 * @module: location to place loaded module pointer
 *
 * Load an arbitrary PKCS\#11 module from a dynamic library file, and
 * initialize it. Normally using the p11_kit_initialize_registered() function
 * is preferred.
 *
 * Using this function to load and initialize modules allows coordination between
 * multiple users of the same module in a single process. The caller should not
 * call the module's <code>C_Initialize</code> method. This function will call
 * <code>C_Initialize</code> as necessary.
 *
 * If a module has already been loaded, then use of this function is unnecesasry.
 * Instead use the p11_kit_initialize_module() function to initialize it.
 *
 * Subsequent calls to this function for the same module will result in an
 * initialization count being incremented for the module. It is safe (although
 * usually unnecessary) to use this function on registered modules.
 *
 * The module must be finalized with p11_kit_finalize_module() instead of
 * calling its <code>C_Finalize</code> method directly.
 *
 * This function does not accept a <code>CK_C_INITIALIZE_ARGS</code> argument.
 * Custom initialization arguments cannot be supported when multiple consumers
 * load the same module.
 *
 * Returns: CKR_OK if the initialization was successful.
 */
CK_RV
p11_kit_load_initialize_module (const char *module_path,
                                CK_FUNCTION_LIST_PTR_PTR module)
{
	Module *mod;
	CK_RV rv = CKR_OK;

	/* WARNING: This function must be reentrant for the same arguments */
	debug ("in");

	_p11_lock ();

		rv = init_globals_unlocked ();
		if (rv == CKR_OK) {

			rv = load_module_from_file_unlocked (module_path, &mod);
			if (rv == CKR_OK) {

				/* WARNING: Reentrancy can occur here */
				rv = initialize_module_unlocked_reentrant (mod);
			}
		}

	_p11_unlock ();

	debug ("out: %lu", rv);
	return rv;
}