/*****************************************************************************\
*  _  _       _          _              ___                                   *
* | || | ___ | |_  _ __ | | _  _  __ _ |_  )                                  *
* | __ |/ _ \|  _|| '_ \| || || |/ _` | / /                                   *
* |_||_|\___/ \__|| .__/|_| \_,_|\__, |/___|                                  *
*                 |_|            |___/                                        *
\*****************************************************************************/

#define _GNU_SOURCE

#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <linux/types.h>
#include <linux/netlink.h>
#include <poll.h>

#include "mem_utils.h"
#include "filemap_utils.h"
#include "hotplug2.h"
#include "hotplug2_utils.h"
#include "rules.h"
#include "childlist.h"

#define TERMCONDITION (persistent == 0 && \
					coldplug_p == FORK_FINISHED && \
					child == NULL && \
					highest_seqnum == get_kernel_seqnum())

/*
 * These variables are accessed from throughout the code.
 *
 * TODO: Move this into a hotplug2_t-like variable.
 */
event_seqnum_t highest_seqnum = 0;
pid_t coldplug_p;
int coldplug = 1;
int persistent = 0;
int override = 0;
int max_child_c = 20;
int dumb = 0;
int terminate = 0;
int child_c;
struct hotplug2_child_t *child;
int netlink_socket;

char *modprobe_command = NULL;

/**
 * Release all memory associated with an uevent read from kernel. The given
 * pointer is no longer valid, as it gets freed as well.
 *
 * @1 The event that is to be freed.
 *
 * Returns: void
 */
inline void free_hotplug2_event(struct hotplug2_event_t *event) {
	int i;
	
	for (i = 0; i < event->env_vars_c; i++) {
		free(event->env_vars[i].key);
		free(event->env_vars[i].value);
	}
	free(event->env_vars);
	free(event->plain);
	free(event);
}

/**
 * A trivial function determining the action that the uevent.
 *
 * @1 String containing the action name (null-terminated).
 *
 * Returns: Macro of the given action
 */
inline int get_hotplug2_event_action(char *action) {
	if (!strcmp(action, "add"))
		return ACTION_ADD;
	
	if (!strcmp(action, "remove"))
		return ACTION_REMOVE;
	
	return ACTION_UNKNOWN;
}

/**
 * Looks up a value according to the given key.
 *
 * @1 A hotplug event structure
 * @2 Key for lookup
 *
 * Returns: The value of the key or NULL if no such key found
 */
char *get_hotplug2_value_by_key(struct hotplug2_event_t *event, char *key) {
	int i;
	
	for (i = 0; i < event->env_vars_c; i++) {
		if (!strcmp(event->env_vars[i].key, key))
			return event->env_vars[i].value;
	}

	return NULL;
}

/**
 * Appends a key-value pair described by the second argument to the
 * hotplug event.
 *
 * @1 A hotplug event structure
 * @1 An item in format "key=value" to be appended
 *
 * Returns: 0 if success, -1 if the string is malformed
 */
int add_hotplug2_event_env(struct hotplug2_event_t *event, char *item) {
	char *ptr, *tmp;
	
	ptr = strchr(item, '=');
	if (ptr == NULL)
		return -1;
	
	*ptr='\0';
	
	event->env_vars_c++;
	event->env_vars = xrealloc(event->env_vars, sizeof(struct env_var_t) * event->env_vars_c);
	event->env_vars[event->env_vars_c - 1].key = strdup(item);
	event->env_vars[event->env_vars_c - 1].value = strdup(ptr + 1);
	
	/*
	 * Variables not generated by kernel but demanded nonetheless...
	 *
	 * TODO: Split this to a different function
	 */
	if (!strcmp(item, "DEVPATH")) {
		event->env_vars_c++;
		event->env_vars = xrealloc(event->env_vars, sizeof(struct env_var_t) * event->env_vars_c);
		event->env_vars[event->env_vars_c - 1].key = strdup("DEVICENAME");
		tmp = strdup(ptr + 1);
		event->env_vars[event->env_vars_c - 1].value = strdup(basename(tmp));
		free(tmp);
	}
	
	*ptr='=';
	
	return 0;
}

/**
 * Duplicates all allocated memory of a source hotplug event
 * and returns a new hotplug event, an identical copy of the
 * source event.
 *
 * @1 Source hotplug event structure
 *
 * Returns: A copy of the source event structure
 */
inline struct hotplug2_event_t *dup_hotplug2_event(struct hotplug2_event_t *src) {
	struct hotplug2_event_t *dest;
	int i;
	
	dest = xmalloc(sizeof(struct hotplug2_event_t));
	dest->action = src->action;
	dest->env_vars_c = src->env_vars_c;
	dest->env_vars = xmalloc(sizeof(struct env_var_t) * dest->env_vars_c);
	dest->plain_s = src->plain_s;
	dest->plain = xmalloc(dest->plain_s);
	memcpy(dest->plain, src->plain, dest->plain_s);
	
	for (i = 0; i < src->env_vars_c; i++) {
		dest->env_vars[i].key = strdup(src->env_vars[i].key);
		dest->env_vars[i].value = strdup(src->env_vars[i].value);
	}
	
	return dest;
}

/**
 * Parses a string into a hotplug event structurs.
 *
 * @1 The event string (not null terminated)
 * @2 The size of the event string
 *
 * Returns: A new event structure
 */
inline struct hotplug2_event_t *get_hotplug2_event(char *event_str, int size) {
	char *ptr;
	struct hotplug2_event_t *event;
	int skip;
	
	ptr = strchr(event_str, '@');
	if (ptr == NULL) {
		return NULL;
	}
	*ptr='\0';
	
	event = xmalloc(sizeof(struct hotplug2_event_t));
	event->action = get_hotplug2_event_action(event_str);
	event->env_vars_c = 0;
	event->env_vars = NULL;
	event->plain_s = size;
	event->plain = xmalloc(size);
	memcpy(event->plain, event_str, size);
	
	skip = ++ptr - event_str;
	size -= skip;
	
	while (size > 0) {
		add_hotplug2_event_env(event, ptr);
		skip = strlen(ptr);
		ptr += skip + 1;
		size -= skip + 1;
	}
	
	return event;
}

/**
 * Evaluates an argument into a true/false value.
 *
 * @1 argument
 * @2 argument flag
 * @3 pointer to output value
 *
 * Returns: 0 if success, -1 otherwise
 */
int get_bool_opt(char *argv, char *name, int *value) {
	int rv = -1;
	
	if (!strncmp(argv, "--no-", 5)) {
		rv = 0;
		argv+=5;
	}
	
	if (!strncmp(argv, "--", 2)) {
		rv = 1;
		argv+=2;
	}
	
	if (rv == -1)
		return -1;
	
	if (!strcmp(argv, name)) {
		*value = rv;
		return 0;
	} else {
		return -1;
	}
}

/**
 * Performs a cleanup; closes uevent socket, resets signal
 * handlers, waits for all the children.
 *
 * Returns: void
 */
void cleanup() {
	pid_t p;
	
	close(netlink_socket);
	
	signal(SIGUSR1, SIG_DFL);
	signal(SIGINT, SIG_DFL);
	signal(SIGCHLD, SIG_DFL);
	
	INFO("cleanup", "Waiting for children.");
	/* Wait for our possible children... */
	while ((p = wait(NULL)) != -1)
		DBG("cleanup", "pid: %d.", p);
	INFO("cleanup", "All children terminated.");
}

/**
 * Handles all signals.
 *
 * @1 Signal identifier
 *
 * Returns: void
 */
void sighandler(int sig) {
	pid_t p;
	
	switch (sig) {
		/*
		 * SIGINT simply tells that yes, we caught the signal, and 
		 * exits.
		 */
		case SIGINT:
			INFO("sighandler", "Obtained SIGINT, quitting.");
			cleanup();
			exit(0);
			break;
		
		/*
		 * SIGUSR1 is handled so that if we have turned off persistency
		 * and have processed all events, we quit.
		 */
		case SIGUSR1:
			persistent = !persistent;
			INFO("sighandler", "Changed persistency to: %s", persistent ? "yes" : "no");
			if (TERMCONDITION) {
				INFO("sighandler", "No more events to be processed, quitting.");
				cleanup();
				exit(0);
			}
			break;
		
		/*
		 * SIGCHLD helps us to figure out when a child died and
		 * what kind of child it was. It may also invoke termination.
		 */
		case SIGCHLD:
			while (1) {
				p = waitpid (WAIT_ANY, NULL, WNOHANG);
				if (p <= 0)
					break;
				
				DBG("sighandler", "caught pid: %d.", p);
				if (p == coldplug_p) {
					DBG("sighandler", "coldplug_p: %d.", coldplug_p);
					coldplug_p = FORK_FINISHED;
				} else {
					child = remove_child_by_pid(child, p, NULL, &child_c);
				}
				
				DBG("sighandler", "child_c: %d, child: %p, highest_seqnum: %lld, cur_seqnum: %lld, coldplug_p: %d.\n", child_c, child, highest_seqnum, get_kernel_seqnum(), coldplug_p);
			}
			
			if (TERMCONDITION) {
				INFO("sighandler", "No more events to be processed, quitting.");
				cleanup();
				exit(0);
			}
			break;
	}
}

#ifdef HAVE_RULES
/**
 * Execute all rules for this particular event.
 *
 * @1 Hotplug event structure
 * @2 Rules structure, containing array of rules
 *
 * Returns: void
 */
void perform_action(struct hotplug2_event_t *event, struct rules_t *rules) {
	int i, rv;
	
	for (i = 0; i < rules->rules_c; i++) {
		rv = rule_execute(event, &rules->rules[i]);
		if (rv == -1)
			break;
	}
	
	free_hotplug2_event(event);
}

/**
 * Iterates through all rules, and performs an AND between all flags that
 * would apply during execution (ie. all rules that have conditions matching
 * the hotplug event).
 *
 * @1 Hotplug event structure
 * @2 Rules structure, containing array of rules
 *
 * Returns: Flags that apply to all matching rules
 */
int flags_eval(struct hotplug2_event_t *event, struct rules_t *rules) {
	int flags = FLAG_ALL;
	int match = 0;
	int i, j;

	for (i = 0; i < rules->rules_c; i++) {
		match = 1;

		for (j = 0; j < rules->rules[i].conditions_c; j++) {
			if (rule_condition_eval(event, &rules->rules[i].conditions[j]) != EVAL_MATCH) {
				match = 0;
				break;
			}
		}

		/*
		 * Logical AND between flags we've got already and
		 * those we're adding.
		 */
		if (match) {
			rule_flags(&rules->rules[i]);
			flags &= rules->rules[i].flags;
		}
	}

	/*
	 * A little trick; if no rule matched, we return FLAG_ALL
	 * and have it skipped completely.
	 */

	return flags;
}
#else
#define perform_action(event, rules)
#endif

/**
 * Blindly modprobe the modalias, nothing more.
 *
 * @1 Hotplug event structure
 * @2 Modalias to be loaded
 *
 * Returns: void
 */
void perform_dumb_action(struct hotplug2_event_t *event, char *modalias) {
	free_hotplug2_event(event);
	execl(modprobe_command, modprobe_command, "-q", modalias, NULL);
}

/**
 * Attempt to figure out whether our modprobe command can handle modalias.
 * If not, use our own wrapper.
 *
 * Returns: 0 if success, -1 otherwise
 */
int get_modprobe_command() {
	pid_t p;
	int fds[2];
	char buf[18];
	FILE *fp;
	
	pipe(fds);
	
	p = fork();
	
	switch (p) {
		case -1:
			ERROR("modprobe_command","Unable to fork.");
			return -1;
			break;
		
		case 0:
			close(fds[0]);
			close(2);
			dup2(fds[1], 1);
		
			execlp("/sbin/modprobe", "/sbin/modprobe", "--version", NULL);
			exit(1);
			break;
	
		default:
			close(fds[1]);
			fp = fdopen(fds[0], "r");
			fread(buf, 1, 17, fp);
			buf[17]='\0';
		
			/* 
			 * module-init-tools can handle aliases.
			 * If we do not have a match, we use hotplug2-depwrap,
			 * even though our modprobe can do fnmatch aliases,
			 * which is the case of eg. busybox.
			 */
			if (!strcmp(buf, "module-init-tools")) {
				modprobe_command = "/sbin/modprobe";
			} else {
				modprobe_command = "/sbin/hotplug2-depwrap";
			}
			fclose(fp);
			waitpid(p, NULL, 0);
			break;
	}
	return 0;
}

int main(int argc, char *argv[]) {
	/*
	 * TODO, cleanup
	 */
	static char buffer[UEVENT_BUFFER_SIZE+512];
	struct hotplug2_event_t *tmpevent;
	char *modalias, *seqnum;
	event_seqnum_t cur_seqnum;
	pid_t p;
	int recv_errno;
	int size;
	int rv = 0;
	int i;
	unsigned int flags;
	char *coldplug_command = NULL;
	char *rules_file = HOTPLUG2_RULE_PATH;
	sigset_t block_mask;
	struct pollfd msg_poll;

	struct hotplug2_event_t *backlog = NULL;
	struct hotplug2_event_t *backlog_tail = NULL;
	int n_backlog = 0;
	
	struct rules_t *rules = NULL;
	struct filemap_t filemap;

	struct options_t bool_options[] = {
		{"persistent", &persistent},
		{"coldplug", &coldplug},
		{"udevtrigger", &coldplug},	/* compatibility */
		{"override", &override},
#ifdef HAVE_RULES
		{"dumb", &dumb},
#endif
		{NULL, NULL}
	};
	
	/*
	 * We parse all the options...
	 */
	for (argc--; argc > 0; argc--) {
		argv++;
		/*
		 * TODO, cleanup
		 */
		for (i = 0; bool_options[i].name != NULL; i++) {
			if (!get_bool_opt(*argv, bool_options[i].name, bool_options[i].value)) {
				/*
				 * Bool options are --option or --no-options. If we handled
				 * it, quit iterating.
				 */
				break;
			} else {
				if (!strcmp(*argv, "--max-children")) {
					argv++;
					argc--;
					if (argc <= 0)
						break;
					
					max_child_c = strtol(*argv, NULL, 0);
				} else if (!strcmp(*argv, "--set-coldplug-cmd")) {
					argv++;
					argc--;
					if (argc <= 0)
						break;
					
					coldplug_command = *argv;
				} else if (!strcmp(*argv, "--set-modprobe-cmd")) {
					argv++;
					argc--;
					if (argc <= 0)
						break;
					
					modprobe_command = *argv;
				} else if (!strcmp(*argv, "--set-rules-file")) {
					argv++;
					argc--;
					if (argc <= 0)
						break;
					
					rules_file = *argv;
				}
			}
		}
	}
	
#ifndef HAVE_RULES
	/*
	 * We don't use rules, so we use dumb mode only.
	 */
	dumb = 1;
#else
	/*
	 * We're not in dumb mode, parse the rules. If we fail,
	 * faillback to dumb mode.
	 */
	if (!dumb) {
		if (map_file(rules_file, &filemap)) {
			ERROR("rules parse","Unable to open/mmap rules file.");
			dumb = 1;
			goto end_rules;
		}
		
		rules = rules_from_config((char*)(filemap.map), NULL);
		if (rules == NULL) {
			ERROR("rules parse","Unable to parse rules file.");
			dumb = 1;
		}

		unmap_file(&filemap);
		
end_rules:	
		if (dumb == 1)
			ERROR("rules parse","Parsing rules failed, switching to dumb mode.");
	} else
#endif
	/*
	 * No modprobe command specified, let's autodetect it.
	 */
	if (!modprobe_command)
	{
		if (get_modprobe_command()) {
			ERROR("modprobe_command","Unable to autodetect modprobe command.");
			goto exit;
		}
		DBG("modprobe_command", "Using modprobe: `%s'.", modprobe_command);
	}
	
	/*
	 * Open netlink socket to read the uevents
	 */
	netlink_socket = init_netlink_socket(NETLINK_BIND);
	msg_poll.fd = netlink_socket;
	msg_poll.events = POLLIN;
	
	if (netlink_socket == -1) {
		ERROR("netlink init","Unable to open netlink socket.");
		goto exit;
	}

	child = NULL;
	child_c = 0;
	
	signal(SIGUSR1, sighandler);
	signal(SIGINT, sighandler);
	signal(SIGCHLD, sighandler);
	
	/*
	 * If we desire coldplugging, we initiate it right now.
	 */
	if (coldplug) {
		if (coldplug_command == NULL)
			coldplug_command = UDEVTRIGGER_COMMAND;
		coldplug_p = fork();
		switch (coldplug_p) {
			case FORK_ERROR:
				ERROR("coldplug","Coldplug fork failed: %s.", strerror(errno));
				perror("coldplug fork failed");
				goto exit;
				break;
			case 0:
				execlp(coldplug_command, coldplug_command, NULL);
				ERROR("coldplug","Coldplug exec ('%s') failed: %s.", coldplug_command, strerror(errno));
				goto exit;
				break;
		}
	} else {
		coldplug_p = FORK_FINISHED;
	}
	
	/*
	 * Main loop reading uevents
	 */
	while (!terminate) {
		if ((n_backlog > 0) && (child_c < max_child_c)) {
			/* dequeue backlog message */
			tmpevent = backlog;
			backlog = backlog->next;
			n_backlog--;
			if (backlog_tail == tmpevent)
				backlog_tail = NULL;
		} else {
			/*
			 * Read the uevent packet
			 */
			if (n_backlog >= HOTPLUG2_MSG_BACKLOG) {
				usleep(HOTPLUG2_THROTTLE_INTERVAL * 1000);
				continue;
			}

			if ((n_backlog > 0) && (child_c >= max_child_c)) {
				int fds;
				msg_poll.revents = 0;
				fds = poll(&msg_poll, 1, HOTPLUG2_THROTTLE_INTERVAL);
				if (fds < 0) {
					continue;
				}
				if (fds == 0)
					continue;
			}
			size = recv(netlink_socket, &buffer, sizeof(buffer), 0);
			recv_errno = errno;
			if (size < 0)
				continue;
	
			/*
			 * Parse the event into an event structure
			 */
			tmpevent = get_hotplug2_event(buffer, size);
		
			if (tmpevent == NULL) {
				ERROR("reading events", "Malformed event read (missing action prefix).");
				continue;
			}
		}
		
		/*
		 * Look up two important items of the event
		 */
		modalias = get_hotplug2_value_by_key(tmpevent, "MODALIAS");
		seqnum = get_hotplug2_value_by_key(tmpevent, "SEQNUM");

		/*
		 * Seqnum is necessary not to end up in a race with the kernel.
		 */
		if (seqnum == NULL) {
			free_hotplug2_event(tmpevent);
			ERROR("reading events", "Malformed event read (missing SEQNUM).");
			continue;
		}
		
		/*
		 * Maintain seqnum continuity
		 */
		cur_seqnum = strtoull(seqnum, NULL, 0);
		if (cur_seqnum > highest_seqnum)
			highest_seqnum = cur_seqnum;
		
		/*
		 * If we are in smart mode, we'll always pass. If we're in dumb mode,
		 * we only pass events that have 'add' action and have modalias set.
		 */
		if ((dumb && tmpevent->action == ACTION_ADD && modalias != NULL) || (!dumb)) {
			/*
			 * Pre-evaluation of the flags
			 */
			if (!dumb && override) {
				flags = flags_eval(tmpevent, rules);

				DBG("flags", "flag returned: %8x", flags);

				if (flags == FLAG_ALL)
					continue;
			} else {
				flags = FLAG_UNSET;
			}

			/* 
			 * We have more children than we want. Wait until SIGCHLD handler reduces
			 * their numbers.
			 *
			 * Unless, of course, we've specified otherwise and no rules that match
			 * need throttling.
			 */
			if (!(flags & FLAG_NOTHROTTLE) && (child_c >= max_child_c)) {
				/* log the packet and process it later */
				if (backlog_tail)
					backlog_tail->next = tmpevent;
				else
					backlog = tmpevent;
				tmpevent->next = NULL;
				backlog_tail = tmpevent;
				n_backlog++;
				continue;
			}
			
			sigemptyset(&block_mask);
			sigaddset(&block_mask, SIGCHLD);
			sigprocmask(SIG_BLOCK, &block_mask, 0);
			p = fork();
			switch (p) {
				case -1:
					ERROR("event", "fork failed: %s.", strerror(errno));
					break;
				case 0:
					/*
					 * TODO: We do not have to dup here, or do we?
					 */
					sigprocmask(SIG_UNBLOCK, &block_mask, 0);
					signal(SIGCHLD, SIG_DFL);
					signal(SIGUSR1, SIG_DFL);
					if (!dumb)
						perform_action(dup_hotplug2_event(tmpevent), rules);
					else
						perform_dumb_action(dup_hotplug2_event(tmpevent), modalias);
					exit(0);
					break;
				default:
					DBG("spawn", "spawning: %d.", p);
					child = add_child(child, p, cur_seqnum);
					child_c++;
					break;
			}
			sigprocmask(SIG_UNBLOCK, &block_mask, 0);
		}
		
		free_hotplug2_event(tmpevent);
	}

exit:
	signal(SIGUSR1, SIG_DFL);
	signal(SIGINT, SIG_DFL);
	signal(SIGCHLD, SIG_DFL);
	
	if (!dumb) {
		rules_free(rules);
		free(rules);
	}

	cleanup();
	
	return rv;
}
