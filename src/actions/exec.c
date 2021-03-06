#define _GNU_SOURCE
#include <stdlib.h>
#include <pthread.h>
#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <pwd.h>
#include <grp.h>
#include <ctype.h>

#include <libconfig.h>
#include <glib.h>

#include "common.h"
#include "list.h"
#include "config.h"
#include "action.h"
#include "subst.h"
#include "uevent.h"
#include "util.h"

struct env_var {
	const char *name;
	struct subst_vec *value;
};

struct exec_action {
	struct action base;
	const char *path;
	int argc;
	struct subst_vec **argv;
	int envc;
	struct env_var **envp;
	uid_t uid;
	gid_t gid;
};

static inline struct exec_action *
to_exec_action(const struct action *base_action)
{
	return container_of(base_action, struct exec_action, base);
}


static int
setup_argv(const struct exec_action *action, const config_setting_t *command,
	   struct subst_vec ***vecp, int *countp)
{
	int ret, i;
	gint argc;
	gchar **argv = NULL;
	GError *error = NULL;
	const char *args;
	struct subst_vec **vec;

	args = config_setting_require_string(command);
	if (!args)
		return -EINVAL;

	*vecp = NULL;
	*countp = 0;

	if (g_shell_parse_argv(args, &argc, &argv, &error) != TRUE) {
		g_assert(error->code != G_SHELL_ERROR_EMPTY_STRING);
		config_error(command, "%s", error->message);
		return -EINVAL;
	}

	vec = calloc(argc + 1, sizeof(*vec));
	if (!vec) {
		ret = -ENOMEM;
		goto free;
	}

	for (i = 0; i < argc; i++) {
		ret = subst_tokenize(argv[i], &vec[i]);
		if (ret)
			goto free;
	}
	*countp = argc;
	*vecp = vec;
	return 0;

free:
	for (i = 1; i <= argc; i++) {
		if (vec[i])
			free(vec[i]);
	}
	if (vec)
		free(vec);
	if (argv)
		g_strfreev(argv);
	return ret;
}

static int
setup_env(struct exec_action *action, const config_setting_t *env)
{
	int count, i;
	const char **envp = NULL;
	int ret = 0;

	if (!env)
		return 0;

	if (config_setting_type(env) == CONFIG_TYPE_STRING)
		count = 1;
	else if (config_setting_is_aggregate(env))
		count = config_setting_length(env);
	else {
		config_error(env, "`env' must be string or aggregate of strings, formatted as key=value (value may be empty).");
		return -EINVAL;
	}

	envp = calloc(count + 1, sizeof(char *));
	if (!envp) {
		log_err("Failed to allocate memory");
		return -ENOMEM;
	}

	ret = config_setting_fill_string_vector(envp, count, env);
	if (ret) {
		g_assert(ret != -ERANGE);
		goto free;
	}

	action->envp = calloc(count + 1, sizeof(struct env_var *));
	if (!action->envp) {
		ret = -ENOMEM;
		log_err("Failed to allocate memory.");
		goto free;
	}

	for (i = 0; envp[i]; i++) {
		const char *name, *value;
		name = value = envp[i];

		while (*value && *value != '=') {
			if (!isalnum(*value) && *value != '_') {
				ret = -EINVAL;
				config_error(env,
					     "`%s' is not a valid environment variable name.  Must be series of alphanumeric or underscore characters (or empty).",
					     envp[i]);
				break;
			}
		}

		action->envp[i]->name = strndup(name, value - name);
		if (*value++ != '=')
			continue;

		ret = subst_tokenize(value, &action->envp[i]->value);
		if (ret)
			break;
	}
	action->envp[count] = NULL;
free:
	free(envp);
	return ret;
}

static struct action *
setup(const struct action_type *type, const config_setting_t *setting)
{
	struct exec_action *action;
	const config_setting_t *command, *env;
	const config_setting_t *uid, *gid;
	int ret;

	action = zalloc(sizeof(*action));
	if (!action) {
		log_err("failed to alloc memory for exec action.");
		return NULL;
	}
	action_init(&action->base, setting, type);

	command = config_setting_get_member(setting, "command");
	env = config_setting_get_member(setting, "env");
	uid = config_setting_get_member(setting, "uid");
	gid = config_setting_get_member(setting, "gid");

	if (!command) {
		config_error(setting, "action type `exec' requires `command'.");
		goto free;
	}

	ret = setup_argv(action, command, &action->argv, &action->argc);
	if (ret)
		goto free;

	ret = setup_env(action, env);
	if (ret)
		goto free;

	if (uid) {
		if (config_setting_type(uid) == CONFIG_TYPE_STRING) {
			const char *user = config_setting_get_string(uid);
			ret = util_get_user(user, &action->uid, &action->gid);
			if (ret == -ENOENT) {
				config_error(uid,
				     "Failed to look up user %s: No such user.",
				      user);
				goto free;
			} else if (ret) {
				config_error(uid,
					     "Failed to look up user `%s'",
					     user);
				goto free;
			}
		} else if (!config_setting_require_int(uid)) {
			action->uid = config_setting_get_int(uid);
			/*
			 * We need to grab the primary group for this user
			 * in case it's not overridden in the config
			 */
			if (!gid) {
				ret = util_get_user_by_uid(action->uid, NULL,
							   &action->gid);
				if (ret == -ENOENT) {
					config_error(uid,
					     "Failed to look up uid %u: No such user.",
					      uid);
					goto free;
				} else if (ret) {
					config_error(uid,
					     "Failed to look up uid %u",
					      uid);
					goto free;
				}
			}
		} else
			goto free;
	}

	if (gid) {
		if (config_setting_type(gid) == CONFIG_TYPE_STRING) {
			const char *grname;
			int ret;

			grname = config_setting_get_string(gid);
			ret = util_get_group(grname, &action->gid);
			if (ret == -ENOENT) {
				config_error(gid,
					     "Failed to look up group `%s': No such group.",
					     grname);
				goto free;
			} else if (ret) {
				config_error(gid,
					     "Failed to look up group `%s': %s",
					     grname, strerror(errno));
				goto free;
			}
		} else if (!config_setting_require_int(gid))
			action->gid = config_setting_get_int(gid);
		else
			goto free;
	}

	return &action->base;

free:
	action_release(&action->base);
	return NULL;
}

static int
export_uevent_properties(struct udev_device *uevent)
{
	struct udev_list_entry *list;
	struct udev_list_entry *node;
	int ret = 0;

	list = udev_device_get_properties_list_entry(uevent);
	if (!list)
		return 0;

	udev_list_entry_foreach(node, list) {
		const char *name, *value;
		name = udev_list_entry_get_name(node);
		value = udev_list_entry_get_value(node);
		if (!global_state.dry_run) {
			ret = setenv(name, value, 1);
		} else if (global_state.debug) {
			log_info("Would set env %s=%s",
				 name, value);
		}
		if (ret)
			break;
	}
	return ret;
}

static int
export_properties(const struct exec_action *action,
		  struct udev_device *uevent)
{
	int i;
	int ret = 0;

	if (global_state.dry_run)
		return 0;

	export_uevent_properties(uevent);

	for (i = 0; i < action->envc; i++) {
		const char *name;
		const char *value;
		bool needs_free = false;

		name = action->envp[i]->name;
		if (action->envp[i]->value) {
			value = uevent_subst(action->envp[i]->value, uevent);
			needs_free = true;
		} else
			value = uevent_get_property(name, uevent, &needs_free);

		if (value) {
			if (!global_state.dry_run)
				ret = setenv(name, value, 1);
			else if (global_state.debug)
				log_info("Would set env %s=%s",
					 name, value);
			if (ret) {
				log_err("failed to set environment %s=%s",
					name, value);
				break;
			}
			if (needs_free)
				free((char *)value);
		}
	}

	return ret;
}

static int
execute(const struct action *base_action, struct udev_device *uevent)
{
	const struct exec_action *action = to_exec_action(base_action);
	int ret = 0, i;
	pid_t pid = 0;

	g_assert(action != NULL);
	g_assert(uevent != NULL);

	if (!global_state.dry_run)
		pid = fork();
	if (pid == 0) { /* child */
		const char **args;

		args = alloca(sizeof(char *) * (action->argc + 1));
		for (i = 0; i < action->argc; i++) {
			args[i] = uevent_subst(action->argv[i], uevent);
			if (!args[i]) {
				errno = EINVAL;
				goto no_exec;
			}
		}
		args[action->argc] = NULL;

		ret = export_properties(action, uevent);
		if (ret)
			goto no_exec;

		ret = util_set_cred(action->gid, action->uid);
		if (ret)
			goto no_exec;

		if (global_state.debug || global_state.dry_run) {
			char *cmdline = util_strjoin(args, " ");
			if (global_state.dry_run)
				log_info("Would start child for \"%s\"\n",
					 cmdline);
			else
				log_debug("Starting child %u: %s",
					  getpid(), cmdline);
			free(cmdline);
			if (global_state.dry_run)
				return 0;
		}

		ret = execvp(args[0], (char ** const)args);
		if (ret) {
			log_warn("failed to execute %s: %s",
				 args[0], strerror(errno));
no_exec:
			_exit(1);
		}

		/* Not reached */
	} else if (pid > 0) { /* parent */
		return util_wait_helper(pid);
	} else {
		ret = -errno;
		log_err("fork failed: %s", strerror(errno));
	}

	return ret;
}

static void
release(struct action *base_action)
{
	struct exec_action *action = to_exec_action(base_action);
	int i;

	if (action->argv) {
		for (i = 0; action->argv[i]; i++)
			subst_release(action->argv[i]);
		free(action->argv);
	}
	
	if (action->envp) {
		for (i = 0; action->envp[i]; i++) {
			free((char *)action->envp[i]->name);
			subst_release(action->envp[i]->value);
		}
		free(action->envp);
	}
	free(action);
}

struct action_type exec_action_type = {
	.name = "exec",
	.setup = setup,
	.execute = execute,
	.release = release,
};
