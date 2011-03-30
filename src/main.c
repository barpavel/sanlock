/*
 * Copyright (C) 2010-2011 Red Hat, Inc.  All rights reserved.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU General Public License v.2.
 */

#include <inttypes.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stddef.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <time.h>
#include <syslog.h>
#include <pthread.h>
#include <poll.h>
#include <sched.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <sys/mman.h>

#define EXTERN
#include "sanlock_internal.h"
#include "diskio.h"
#include "log.h"
#include "paxos_lease.h"
#include "delta_lease.h"
#include "host_id.h"
#include "token_manager.h"
#include "lockfile.h"
#include "watchdog.h"
#include "client_msg.h"
#include "sanlock_resource.h"
#include "sanlock_admin.h"
#include "sanlock_direct.h"

/* priorities are LOG_* from syslog.h */
int log_logfile_priority = LOG_ERR;
int log_syslog_priority = LOG_ERR;
int log_stderr_priority = LOG_ERR;

struct client {
	int used;
	int fd;
	int pid;
	int cmd_active;
	int acquire_done;
	int pid_dead;
	int killing;
	char owner_name[SANLK_NAME_LEN+1];
	pthread_mutex_t mutex;
	void *workfn;
	void *deadfn;
	struct token *tokens[SANLK_MAX_RESOURCES];
};

#define CLIENT_NALLOC 32 /* TODO: test using a small value here */
static int client_maxi;
static int client_size = 0;
static struct client *client = NULL;
static struct pollfd *pollfd = NULL;

static char command[COMMAND_MAX];
static int cmd_argc;
static char **cmd_argv;
static int external_shutdown;
static int token_id_counter = 1;
static int space_id_counter = 1;

struct cmd_args {
	int ci_in;
	int ci_target;
	struct sm_header header;
};

extern struct list_head spaces;
extern struct list_head spaces_remove;
extern pthread_mutex_t spaces_mutex;

static void client_alloc(void)
{
	int i;

	if (!client) {
		client = malloc(CLIENT_NALLOC * sizeof(struct client));
		pollfd = malloc(CLIENT_NALLOC * sizeof(struct pollfd));
	} else {
		client = realloc(client, (client_size + CLIENT_NALLOC) *
					 sizeof(struct client));
		pollfd = realloc(pollfd, (client_size + CLIENT_NALLOC) *
					 sizeof(struct pollfd));
		if (!pollfd)
			log_error("can't alloc for pollfd");
	}
	if (!client || !pollfd)
		log_error("can't alloc for client array");

	for (i = client_size; i < client_size + CLIENT_NALLOC; i++) {
		memset(&client[i], 0, sizeof(struct client));
		pthread_mutex_init(&client[i].mutex, NULL);
		client[i].fd = -1;
		pollfd[i].fd = -1;
		pollfd[i].revents = 0;
	}
	client_size += CLIENT_NALLOC;
}

static void client_ignore(int ci)
{
	pollfd[ci].fd = -1;
	pollfd[ci].events = 0;
}

static void client_back(int ci, int fd)
{
	pollfd[ci].fd = fd;
	pollfd[ci].events = POLLIN;
}

static void client_dead(int ci)
{
	close(client[ci].fd);
	client[ci].used = 0;
	memset(&client[ci], 0, sizeof(struct client));
	client[ci].fd = -1;
	pollfd[ci].fd = -1;
	pollfd[ci].events = 0;
}

static int client_add(int fd, void (*workfn)(int ci), void (*deadfn)(int ci))
{
	int i;

	if (!client)
		client_alloc();
 again:
	for (i = 0; i < client_size; i++) {
		if (!client[i].used) {
			client[i].used = 1;
			client[i].workfn = workfn;
			client[i].deadfn = deadfn ? deadfn : client_dead;
			client[i].fd = fd;
			pollfd[i].fd = fd;
			pollfd[i].events = POLLIN;
			if (i > client_maxi)
				client_maxi = i;
			return i;
		}
	}

	client_alloc();
	goto again;
}

static int find_client_pid(int pid)
{
	int i;

	for (i = 0; i < client_size; i++) {
		if (client[i].used && client[i].pid == pid)
			return i;
	}
	return -1;
}

static int get_peer_pid(int fd, int *pid)
{
	struct ucred cred;
	unsigned int cl = sizeof(cred);

	if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &cl) != 0)
		return -1;

	*pid = cred.pid;
	return 0;
}

static void client_pid_dead(int ci)
{
	struct client *cl = &client[ci];
	int delay_release = 0;
	int i, pid;

	log_debug("client_pid_dead ci %d fd %d pid %d", ci, cl->fd, cl->pid);

	/* cmd_acquire_thread may still be waiting for the tokens
	   to be acquired.  if it is, tell it to release them when
	   finished */

	pthread_mutex_lock(&cl->mutex);
	pid = cl->pid;
	cl->pid = -1;
	cl->pid_dead = 1;

	/* TODO: handle other cmds in progress */
	if ((cl->cmd_active == SM_CMD_ACQUIRE) && !cl->acquire_done) {
		delay_release = 1;
		/* client_dead() also delayed */
	}
	pthread_mutex_unlock(&cl->mutex);

	if (pid > 0)
		kill(pid, SIGKILL);

	if (delay_release) {
		log_debug("client_pid_dead delay release");
		return;
	}

	/* cmd_acquire_thread is done so we can release tokens here */

	for (i = 0; i < SANLK_MAX_RESOURCES; i++) {
		if (cl->tokens[i])
			release_token_async(cl->tokens[i]);
	}

	client_dead(ci);
}

static int client_using_space(struct client *cl, struct space *sp)
{
	struct token *token;
	int i, rv = 0;

	pthread_mutex_lock(&cl->mutex);
	for (i = 0; i < SANLK_MAX_RESOURCES; i++) {
		token = cl->tokens[i];
		if (!token)
			continue;
		if (strncmp(token->space_name, sp->space_name, NAME_ID_SIZE))
			continue;
		rv = 1;
		log_spoke(sp, token, "client_using_space pid %d", cl->pid);
		break;
	}
	pthread_mutex_unlock(&cl->mutex);
	return rv;
}

static void kill_pids(struct space *sp)
{
	struct client *cl;
	int ci, found = 0;

	log_space(sp, "kill_pids %d", sp->killing_pids);

	/* TODO: try killscript first if one is provided */

	if (sp->killing_pids > 11)
		return;

	if (sp->killing_pids > 10)
		goto do_dump;

	if (sp->killing_pids > 1)
		goto do_sigkill;

	for (ci = 0; ci <= client_maxi; ci++) {
		cl = &client[ci];

		if (!cl->used)
			continue;
		if (!cl->pid)
			continue;
		if (!client_using_space(cl, sp))
			continue;
		if (cl->killing > 1)
			continue;

		kill(cl->pid, SIGTERM);
		cl->killing++;
		found++;
	}

	if (found) {
		log_space(sp, "kill_pids SIGTERM found %d pids", found);
		usleep(500000);
	}

	sp->killing_pids++;
	return;

 do_sigkill:

	for (ci = 0; ci <= client_maxi; ci++) {
		cl = &client[ci];

		if (!cl->used)
			continue;
		if (!cl->pid)
			continue;
		if (!client_using_space(cl, sp))
			continue;
		if (cl->killing > 2)
			continue;

		kill(cl->pid, SIGKILL);
		cl->killing++;
		found++;
	}

	if (found) {
		log_space(sp, "kill_pids SIGKILL found %d pids", found);
		usleep(500000);
	}

	sp->killing_pids++;
	return;

 do_dump:
	for (ci = 0; ci <= client_maxi; ci++) {
		if (client[ci].pid && client[ci].killing) {
			log_error("kill_pids %d stuck", client[ci].pid);
			found++;
		}
	}

	sp->killing_pids++;
}

static int all_pids_dead(struct space *sp)
{
	struct client *cl;
	int ci;

	for (ci = 0; ci <= client_maxi; ci++) {
		cl = &client[ci];

		if (!cl->used)
			continue;
		if (!cl->pid)
			continue;
		if (!client_using_space(cl, sp))
			continue;

		log_space(sp, "used by pid %d killing %d",
			  cl->pid, cl->killing);
		return 0;
	}
	log_space(sp, "used by no pids");
	return 1;
}

#define MAIN_POLL_MS 2000

static int main_loop(void)
{
	void (*workfn) (int ci);
	void (*deadfn) (int ci);
	struct space *sp, *safe;
	int poll_timeout = MAIN_POLL_MS;
	int i, rv, empty;

	while (1) {
		rv = poll(pollfd, client_maxi + 1, poll_timeout);
		if (rv == -1 && errno == EINTR)
			continue;
		if (rv < 0) {
			/* not sure */
		}
		for (i = 0; i <= client_maxi; i++) {
			if (client[i].fd < 0)
				continue;
			if (pollfd[i].revents & POLLIN) {
				workfn = client[i].workfn;
				if (workfn)
					workfn(i);
			}
			if (pollfd[i].revents & (POLLERR | POLLHUP | POLLNVAL)) {
				deadfn = client[i].deadfn;
				if (deadfn)
					deadfn(i);
			}
		}

		pthread_mutex_lock(&spaces_mutex);
		list_for_each_entry_safe(sp, safe, &spaces, list) {
			if (sp->killing_pids) {
				if (all_pids_dead(sp)) {
					log_space(sp, "set thread_stop");
					pthread_mutex_lock(&sp->mutex);
					sp->thread_stop = 1;
					unlink_watchdog_file(sp);
					pthread_cond_broadcast(&sp->cond);
					pthread_mutex_unlock(&sp->mutex);
					list_move(&sp->list, &spaces_remove);
				} else {
					kill_pids(sp);
				}
			} else {
				if (external_shutdown || sp->external_remove ||
				    !host_id_renewed(sp)) {
					log_space(sp, "set killing_pids");
					sp->killing_pids = 1;
					kill_pids(sp);
				}
			}
		}
		empty = list_empty(&spaces);
		pthread_mutex_unlock(&spaces_mutex);

		if (empty && external_shutdown)
			break;

		clear_spaces(0);
	}

	clear_spaces(1);

	return 0;
}

static int set_cmd_active(int ci_target, int cmd)
{
	struct client *cl = &client[ci_target];
	int cmd_active = 0;

	pthread_mutex_lock(&cl->mutex);

	cmd_active = cl->cmd_active;

	if (!cmd) {
		/* active to inactive */
		cl->cmd_active = 0;
	} else {
		/* inactive to active */
		if (!cl->cmd_active)
			cl->cmd_active = cmd;
	}
	pthread_mutex_unlock(&cl->mutex);

	if (cmd && cmd_active) {
		log_error("set_cmd_active ci %d cmd %d busy %d",
			  ci_target, cmd, cmd_active);
		return -EBUSY;
	}

	if (!cmd && !cmd_active) {
		log_error("set_cmd_active ci %d already zero",
			  ci_target);
	}

	return 0;
}

/* clear the unreceived portion of an aborted command */

static void client_recv_all(int ci, struct sm_header *h_recv, int pos)
{
	char trash[64];
	int rem = h_recv->length - sizeof(struct sm_header) - pos;
	int rv, total = 0;

	if (!rem)
		return;

	while (1) {
		rv = recv(client[ci].fd, trash, sizeof(trash), MSG_DONTWAIT);
		if (rv <= 0)
			break;
		total += rv;

		if (total > MAX_CLIENT_MSG)
			break;
	}

	log_debug("recv_all ci %d rem %d total %d", ci, rem, total);
}

static void *cmd_acquire_thread(void *args_in)
{
	struct cmd_args *ca = args_in;
	struct sm_header h;
	struct client *cl;
	struct sync_disk *disks = NULL;
	struct token *token = NULL;
	struct token *new_tokens[SANLK_MAX_RESOURCES];
	struct sanlk_resource res;
	struct sanlk_options opt;
	struct space space;
	char *opt_str;
	uint64_t acquire_lver = 0;
	uint32_t new_num_hosts = 0;
	int fd, rv, i, j, disks_len, num_disks, empty_slots, opened;
	int alloc_count = 0, add_count = 0, open_count = 0, acquire_count = 0;
	int pos = 0, pid_dead = 0;
	int new_tokens_count;

	cl = &client[ca->ci_target];
	fd = client[ca->ci_in].fd;

	log_debug("cmd_acquire ci_in %d ci_target %d pid %d",
		  ca->ci_in, ca->ci_target, cl->pid);

	/*
	 * check if we can we add this many new leases
	 */

	new_tokens_count = ca->header.data;
	if (new_tokens_count > SANLK_MAX_RESOURCES) {
		log_error("cmd_acquire new_tokens_count %d max %d",
			  new_tokens_count, SANLK_MAX_RESOURCES);
		rv = -E2BIG;
		goto fail_reply;
	}

	pthread_mutex_lock(&cl->mutex);
	empty_slots = 0;
	for (i = 0; i < SANLK_MAX_RESOURCES; i++) {
		if (!cl->tokens[i])
			empty_slots++;
	}
	pthread_mutex_unlock(&cl->mutex);

	if (empty_slots < new_tokens_count) {
		log_error("cmd_acquire new_tokens_count %d empty %d",
			  new_tokens_count, empty_slots);
		rv = -ENOSPC;
		goto fail_reply;
	}

	/*
	 * read resource input and allocate tokens for each
	 */

	for (i = 0; i < new_tokens_count; i++) {
		token = malloc(sizeof(struct token));
		if (!token) {
			rv = -ENOMEM;
			goto fail_free;
		}
		memset(token, 0, sizeof(struct token));


		/*
		 * receive sanlk_resource, copy into token
		 */

		rv = recv(fd, &res, sizeof(struct sanlk_resource), MSG_WAITALL);
		if (rv > 0)
			pos += rv;
		if (rv != sizeof(struct sanlk_resource)) {
			log_error("cmd_acquire recv %d %d", rv, errno);
			free(token);
			rv = -EIO;
			goto fail_free;
		}

		strncpy(token->space_name, res.lockspace_name, SANLK_NAME_LEN);
		strncpy(token->resource_name, res.name, SANLK_NAME_LEN);
		token->acquire_lver = res.lver;
		token->acquire_data64 = res.data64;
		token->acquire_data32 = res.data32;
		token->acquire_flags = res.flags;
		token->num_disks = res.num_disks;

		/*
		 * receive sanlk_disk's / sync_disk's
		 *
		 * WARNING: as a shortcut, this requires that sync_disk and
		 * sanlk_disk match; this is the reason for the pad fields
		 * in sanlk_disk (TODO: let these differ)
		 */

		num_disks = token->num_disks;
		if (num_disks > MAX_DISKS) {
			free(token);
			rv = -ERANGE;
			goto fail_free;
		}

		disks = malloc(num_disks * sizeof(struct sync_disk));
		if (!disks) {
			free(token);
			rv = -ENOMEM;
			goto fail_free;
		}

		disks_len = num_disks * sizeof(struct sync_disk);
		memset(disks, 0, disks_len);

		rv = recv(fd, disks, disks_len, MSG_WAITALL);
		if (rv > 0)
			pos += rv;
		if (rv != disks_len) {
			log_error("cmd_acquire recv %d %d", rv, errno);
			free(disks);
			free(token);
			rv = -EIO;
			goto fail_free;
		}

		/* zero out pad1 and pad2, see WARNING above */
		for (j = 0; j < num_disks; j++) {
			disks[j].sector_size = 0;
			disks[j].fd = 0;
		}

		token->token_id = token_id_counter++;
		token->disks = disks;
		new_tokens[i] = token;
		alloc_count++;

		/* We use the token_id in log messages because the combination
		 * of full length space_name+resource_name in each log message
		 * would make excessively long lines.  Use an error message
		 * here to make a more permanent record of what the token_id
		 * represents for reference from later log messages. */

		log_errot(token, "lockspace %.48s resource %.48s has token_id %u for pid %u",
			  token->space_name, token->resource_name, token->token_id, cl->pid);
	}

	/*
	 * receive per-command sanlk_options and opt string (if any)
	 */

	rv = recv(fd, &opt, sizeof(struct sanlk_options), MSG_WAITALL);
	if (rv > 0)
		pos += rv;
	if (rv != sizeof(struct sanlk_options)) {
		log_error("cmd_acquire recv %d %d", rv, errno);
		rv = -EIO;
		goto fail_free;
	}

	log_debug("cmd_acquire recv opt %d %x %u", rv, opt.flags, opt.len);

	strcpy(cl->owner_name, opt.owner_name);

	if (!opt.len)
		goto skip_opt_str;

	opt_str = malloc(opt.len);
	if (!opt_str) {
		rv = -ENOMEM;
		goto fail_free;
	}

	rv = recv(fd, opt_str, opt.len, MSG_WAITALL);
	if (rv > 0)
		pos += rv;
	if (rv != opt.len) {
		log_error("cmd_acquire recv %d %d", rv, errno);
		free(opt_str);
		rv = -EIO;
		goto fail_free;
	}

	log_debug("cmd_acquire recv opt str %d", rv);


 skip_opt_str:
	/* TODO: warn if header.length != sizeof(header) + pos ? */

	/*
	 * all command input has been received, start doing the acquire
	 */

	for (i = 0; i < new_tokens_count; i++) {
		token = new_tokens[i];
		rv = get_space_info(token->space_name, &space);
		if (rv < 0 || space.killing_pids) {
			log_errot(token, "cmd_acquire bad space %.48s",
				  token->space_name);
			goto fail_free;
		}
		token->host_id = space.host_id;
		token->host_generation = space.host_generation;
	}

	for (i = 0; i < new_tokens_count; i++) {
		token = new_tokens[i];
		rv = add_resource(token, cl->pid);
		if (rv < 0) {
			log_errot(token, "cmd_acquire add_resource %d", rv);
			goto fail_del;
		}
		add_count++;
	}

	for (i = 0; i < new_tokens_count; i++) {
		token = new_tokens[i];
		opened = open_disks(token->disks, token->num_disks);
		if (!majority_disks(token, opened)) {
			log_errot(token, "cmd_acquire open_disks %d", opened);
			rv = -ENODEV;
			goto fail_close;
		}
		open_count++;
	}

	for (i = 0; i < new_tokens_count; i++) {
		token = new_tokens[i];

		if (token->acquire_flags & SANLK_RES_LVER)
			acquire_lver = token->acquire_lver;
		if (token->acquire_flags & SANLK_RES_NUM_HOSTS)
			new_num_hosts = token->acquire_data32;

		rv = acquire_token(token, acquire_lver, new_num_hosts);
		if (rv < 0) {
			log_errot(token, "cmd_acquire acquire error %d", rv);
			goto fail_release;
		}
		acquire_count++;
	}

	/* 
	 * if pid dead and success|fail, release all
	 * if pid not dead and success, reply
	 * if pid not dead and fail, release new, reply
	 *
	 * transfer all new_tokens into cl->tokens; client_pid_dead
	 * is reponsible from here to release old and new from cl->tokens
	 */

	pthread_mutex_lock(&cl->mutex);

	if (cl->pid_dead) {
		pthread_mutex_unlock(&cl->mutex);
		log_error("cmd_acquire pid %d dead", cl->pid);
		pid_dead = 1;
		rv = -ENOTTY;
		goto fail_dead;
	}

	empty_slots = 0;
	for (i = 0; i < SANLK_MAX_RESOURCES; i++) {
		if (!cl->tokens[i])
			empty_slots++;
	}
	if (empty_slots < new_tokens_count) {
		pthread_mutex_unlock(&cl->mutex);
		log_error("cmd_acquire new_tokens_count %d slots %d",
			  new_tokens_count, empty_slots);
		rv = -ENOSPC;
		goto fail_release;
	}

	/* space may have failed while new tokens were being acquired */
	for (i = 0; i < new_tokens_count; i++) {
		token = new_tokens[i];
		rv = get_space_info(token->space_name, &space);
		if (!rv && !space.killing_pids && space.host_id == token->host_id)
			continue;
		pthread_mutex_unlock(&cl->mutex);
		log_errot(token, "cmd_acquire bad space %.48s", token->space_name);
		rv = -EINVAL;
		goto fail_release;
	}

	for (i = 0; i < new_tokens_count; i++) {
		for (j = 0; j < SANLK_MAX_RESOURCES; j++) {
			if (!cl->tokens[j]) {
				cl->tokens[j] = new_tokens[i];
				break;
			}
		}
	}

	cl->acquire_done = 1;
	cl->cmd_active = 0;   /* instead of set_cmd_active(0) */
	pthread_mutex_unlock(&cl->mutex);

	log_debug("cmd_acquire done %d", new_tokens_count);

	memcpy(&h, &ca->header, sizeof(struct sm_header));
	h.length = sizeof(h);
	h.data = 0;
	h.data2 = 0;
	send(fd, &h, sizeof(h), MSG_NOSIGNAL);

	client_back(ca->ci_in, fd);
	free(ca);
	return NULL;

 fail_dead:
	/* clear out all the old tokens */
	for (i = 0; i < SANLK_MAX_RESOURCES; i++) {
		token = cl->tokens[i];
		if (!token)
			continue;
		release_token(token);
		close_disks(token->disks, token->num_disks);
		del_resource(token);
		free_token(token);
	}

 fail_release:
	for (i = 0; i < acquire_count; i++)
		release_token(new_tokens[i]);

 fail_close:
	for (i = 0; i < open_count; i++)
		close_disks(new_tokens[i]->disks, new_tokens[i]->num_disks);

 fail_del:
	for (i = 0; i < add_count; i++)
		del_resource(new_tokens[i]);

 fail_free:
	for (i = 0; i < alloc_count; i++)
		free_token(new_tokens[i]);

 fail_reply:
	set_cmd_active(ca->ci_target, 0);

	client_recv_all(ca->ci_in, &ca->header, pos);

	memcpy(&h, &ca->header, sizeof(struct sm_header));
	h.length = sizeof(h);
	h.data = -1;
	h.data2 = 0;
	send(fd, &h, sizeof(h), MSG_NOSIGNAL);

	if (pid_dead)
		client_dead(ca->ci_target);

	client_back(ca->ci_in, fd);
	free(ca);
	return NULL;
}

static void *cmd_release_thread(void *args_in)
{
	struct cmd_args *ca = args_in;
	struct sm_header h;
	struct token *token;
	struct sanlk_resource res;
	struct client *cl;
	int fd, rv, i, j, found, result = 0;

	cl = &client[ca->ci_target];
	fd = client[ca->ci_in].fd;

	log_debug("cmd_release ci_in %d ci_target %d pid %d",
		  ca->ci_in, ca->ci_target, cl->pid);

	/* caller wants to release all resources */

	if (ca->header.cmd_flags & SANLK_REL_ALL) {
		for (j = 0; j < SANLK_MAX_RESOURCES; j++) {
			token = cl->tokens[j];
			if (!token)
				continue;

			rv = release_token(token);
			if (rv < 0)
				result = -1;
			free_token(token);
			cl->tokens[j] = NULL;
		}
		goto reply;
	}

	/* caller is specifying specific resources to release */

	for (i = 0; i < ca->header.data; i++) {
		rv = recv(fd, &res, sizeof(struct sanlk_resource), MSG_WAITALL);
		if (rv != sizeof(struct sanlk_resource)) {
			log_error("cmd_release recv fd %d %d %d", fd, rv, errno);
			result = -1;
			goto reply;
		}

		found = 0;

		for (j = 0; j < SANLK_MAX_RESOURCES; j++) {
			token = cl->tokens[j];
			if (!token)
				continue;

			if (memcmp(token->space_name, res.lockspace_name, NAME_ID_SIZE))
				continue;
			if (memcmp(token->resource_name, res.name, NAME_ID_SIZE))
				continue;

			rv = release_token(token);
			if (rv < 0)
				result = -1;
			free_token(token);
			cl->tokens[j] = NULL;
			found = 1;
			break;
		}

		if (!found) {
			log_error("cmd_release pid %d no resource %s",
				  cl->pid, res.name);
			result = -1;
		}
	}

 reply:
	set_cmd_active(ca->ci_target, 0);

	log_debug("cmd_release done");

	memcpy(&h, &ca->header, sizeof(struct sm_header));
	h.length = sizeof(h);
	h.data = result;
	h.data2 = 0;
	send(fd, &h, sizeof(h), MSG_NOSIGNAL);

	client_back(ca->ci_in, fd);
	free(ca);
	return NULL;
}

static void *cmd_inquire_thread(void *args_in)
{
	struct cmd_args *ca = args_in;
	struct sm_header h;
	struct token *token;
	struct sanlk_resource *res;
	char *reply_str;
	struct client *cl;
	int fd, i, d, reply_len, result = 0, total = 0, ret, pos, reply_str_len = 0;

	cl = &client[ca->ci_target];
	fd = client[ca->ci_in].fd;

	log_debug("cmd_inquire ci_in %d ci_target %d pid %d",
		  ca->ci_in, ca->ci_target, cl->pid);

	for (i = 0; i < SANLK_MAX_RESOURCES; i++) {
		if (cl->tokens[i])
			total++;
	}

	/* TODO: use sanlock_res_to_str() */

	reply_len = total * SANLK_MAX_RES_STR;
	reply_str = malloc(reply_len);
	if (!reply_str) {
		result = -ENOMEM;
		goto reply;
	}
	memset(reply_str, 0, reply_len);
	res = (struct sanlk_resource *)reply_str;
	pos = 0;
	ret = 0;

	for (i = 0; i < SANLK_MAX_RESOURCES; i++) {
		token = cl->tokens[i];
		if (!token)
			continue;

		ret = snprintf(reply_str + pos, reply_len - pos, "%s:%s",
			       token->space_name, token->resource_name);

		if (ret >= reply_len - pos)
			goto out;
		pos += ret;

		for (d = 0; d < token->num_disks; d++) {
			ret = snprintf(reply_str + pos, reply_len - pos, ":%s:%llu",
				       token->disks[d].path,
				       (unsigned long long)token->disks[d].offset);

			if (ret >= reply_len - pos)
				goto out;
			pos += ret;
		}

		ret = snprintf(reply_str + pos, reply_len - pos, ":%llu ",
			       (unsigned long long)token->leader.lver);

		if (ret >= reply_len - pos)
			goto out;
		pos += ret;
	}

	/* remove trailing space */
	pos--;
	reply_str[pos++] = '\0';
	reply_str_len = strlen(reply_str);

	ret = 0;
 out:
	if (ret)
		result = -ENOSPC;

 reply:
	set_cmd_active(ca->ci_target, 0);

	log_debug("cmd_inquire done result %d total %d pos %d reply_str_len %d",
		  result, total, pos, reply_str_len);

	memcpy(&h, &ca->header, sizeof(struct sm_header));
	h.data = result;
	h.data2 = total;

	if (reply_str) {
		h.length = sizeof(h) + pos;
		send(fd, &h, sizeof(h), MSG_NOSIGNAL);
		send(fd, reply_str, pos, MSG_NOSIGNAL);
		free(reply_str);
	} else {
		h.length = sizeof(h);
		send(fd, &h, sizeof(h), MSG_NOSIGNAL);
	}

	client_back(ca->ci_in, fd);
	free(ca);
	return NULL;
}

static void *cmd_add_lockspace_thread(void *args_in)
{
	struct cmd_args *ca = args_in;
	struct sm_header h;
	struct space *sp;
	struct sanlk_lockspace lockspace;
	int fd, rv, result;

	fd = client[ca->ci_in].fd;

	log_debug("cmd_add_lockspace ci_in %d", ca->ci_in);

	sp = malloc(sizeof(struct space));
	if (!sp) {
		result = -ENOMEM;
		goto reply;
	}

	rv = recv(fd, &lockspace, sizeof(struct sanlk_lockspace), MSG_WAITALL);
	if (rv != sizeof(struct sanlk_lockspace)) {
		result = -EIO;
		goto reply;
	}

	memset(sp, 0, sizeof(struct space));
	memcpy(sp->space_name, lockspace.name, NAME_ID_SIZE);
	sp->host_id = lockspace.host_id;
	memcpy(&sp->host_id_disk, &lockspace.host_id_disk,
	       sizeof(struct sanlk_disk));
	pthread_mutex_init(&sp->mutex, NULL);
	pthread_cond_init(&sp->cond, NULL);

	pthread_mutex_lock(&spaces_mutex);
	sp->space_id = space_id_counter++;
	pthread_mutex_unlock(&spaces_mutex);

	/* We use the space_id in log messages because the full length
	 * space_name in each log message woul dmake excessively long lines.
	 * Use an error message here to make a more permanent record of what
	 * the space_id represents for reference from later log messages. */

	log_erros(sp, "lockspace %.48s host_id %llu has space_id %u",
		  sp->space_name, (unsigned long long)sp->host_id,
		  sp->space_id);

	/* add_space returns once the host_id has been acquired and
	   sp space has been added to the spaces list */

	result = add_space(sp);

	if (result)
		free(sp);
 reply:
	log_debug("cmd_add_lockspace done %d", result);

	memcpy(&h, &ca->header, sizeof(struct sm_header));
	h.length = sizeof(h);
	h.data = result;
	h.data2 = 0;
	send(fd, &h, sizeof(h), MSG_NOSIGNAL);

	client_back(ca->ci_in, fd);
	free(ca);
	return NULL;
}

static void *cmd_rem_lockspace_thread(void *args_in)
{
	struct cmd_args *ca = args_in;
	struct sm_header h;
	struct sanlk_lockspace lockspace;
	int fd, rv, result;

	fd = client[ca->ci_in].fd;

	log_debug("cmd_rem_lockspace ci_in %d", ca->ci_in);

	rv = recv(fd, &lockspace, sizeof(struct sanlk_lockspace), MSG_WAITALL);
	if (rv != sizeof(struct sanlk_lockspace)) {
		result = -EIO;
		goto reply;
	}

	/* rem_space flags the sp as wanting to be removed, so follow with a
	   wait loop until it's actually gone */

	/* TODO: we should probably prevent add_lockspace during an
	   outstanding rem_lockspace and v.v. */

	result = rem_space(lockspace.name);

	if (result < 0)
		goto reply;

	while (1) {
		if (!space_exists(lockspace.name))
			break;
		sleep(1);
	}

 reply:
	log_debug("cmd_rem_lockspace done %d", result);

	memcpy(&h, &ca->header, sizeof(struct sm_header));
	h.length = sizeof(h);
	h.data = result;
	h.data2 = 0;
	send(fd, &h, sizeof(h), MSG_NOSIGNAL);

	client_back(ca->ci_in, fd);
	free(ca);
	return NULL;
}

static int print_daemon_state(char *str)
{
	memset(str, 0, SANLK_STATE_MAXSTR);

	snprintf(str, SANLK_STATE_MAXSTR-1,
		 "io_timeout=%d host_id_timeout=%d "
		 "host_id_renewal=%d host_id_renewal_fail=%d",
		 to.io_timeout_seconds,
		 to.host_id_timeout_seconds,
		 to.host_id_renewal_seconds,
		 to.host_id_renewal_fail_seconds);

	return strlen(str) + 1;
}

static int print_client_state(struct client *cl, char *str)
{
	memset(str, 0, SANLK_STATE_MAXSTR);

	snprintf(str, SANLK_STATE_MAXSTR-1,
		 "cmd_active=%d acquire_done=%d pid_dead=%d",
		 cl->cmd_active,
		 cl->acquire_done,
		 cl->pid_dead);

	return strlen(str) + 1;
}

static int print_token_state(struct token *t, char *str)
{
	memset(str, 0, SANLK_STATE_MAXSTR);

	snprintf(str, SANLK_STATE_MAXSTR-1,
		 "token_id=%u "
		 "acquire_result=%d "
		 "release_result=%d "
		 "leader.lver=%llu "
		 "leader.timestamp=%llu "
		 "leader.owner_id=%llu "
		 "leader.owner_generation=%llu",
		 t->token_id,
		 t->acquire_result,
		 t->release_result,
		 (unsigned long long)t->leader.lver,
		 (unsigned long long)t->leader.timestamp,
		 (unsigned long long)t->leader.owner_id,
		 (unsigned long long)t->leader.owner_generation);

	return strlen(str) + 1;
}

/*
 *  0. header
 *  1. dst (sanlk_state DAEMON)
 *  2. dst.str (dst.len)
 *  3. lst (sanlk_state LOCKSPACE)
 *  4. lst.str (lst.len)			print_space_state()
 *  5. lockspace (sanlk_lockspace)
 *  6. [repeat 3-5 for each space]
 *  7. cst (sanlk_state CLIENT)
 *  8. cst.str (cst.len)			print_client_state()
 *  9. rst (sanlk_state RESOURCE)
 * 10. rst.str (rst.len)			print_token_state()
 * 11. resource (sanlk_resource)
 * 12. disks (sanlk_disk * resource.num_disks)
 * 13. [repeat 9-12 for each token]
 * 14. [repeat 7-13 for each client]
 */

static void cmd_status(int fd, struct sm_header *h_recv)
{
	struct sm_header h;
	struct sanlk_state dst;
	struct sanlk_state lst;
	struct sanlk_state cst;
	struct sanlk_state rst;
	struct sanlk_lockspace lockspace;
	struct sanlk_resource resource;
	char str[SANLK_STATE_MAXSTR];
	struct token *token;
	struct space *sp;
	struct client *cl;
	int ci, i, j, str_len;

	/*
	 * send header: h
	 */

	memset(&h, 0, sizeof(h));
	memcpy(&h, h_recv, sizeof(struct sm_header));
	h.length = sizeof(h);
	h.data = 0;

	send(fd, &h, sizeof(h), MSG_NOSIGNAL);

	/*
	 * send daemon state: dst, dst.str
	 */

	str_len = print_daemon_state(str);
	memset(&dst, 0, sizeof(dst));
	dst.type = SANLK_STATE_DAEMON;
	dst.str_len = str_len;

	send(fd, &dst, sizeof(dst), MSG_NOSIGNAL);
	if (str_len)
		send(fd, str, str_len, MSG_NOSIGNAL);

	if (h_recv->data == SANLK_STATE_DAEMON)
		return;

	/*
	 * send lockspace state: lst, lst.str, sanlk_lockspace
	 */

	pthread_mutex_lock(&spaces_mutex);
	list_for_each_entry(sp, &spaces, list) {
		str_len = print_space_state(sp, str);
		memset(&lst, 0, sizeof(lst));
		lst.type = SANLK_STATE_LOCKSPACE;
		lst.data64 = sp->host_id;
		strncpy(lst.name, sp->space_name, NAME_ID_SIZE);
		lst.str_len = str_len;

		send(fd, &lst, sizeof(lst), MSG_NOSIGNAL);
		if (str_len)
			send(fd, str, str_len, MSG_NOSIGNAL);

		memset(&lockspace, 0, sizeof(struct sanlk_lockspace));
		strncpy(lockspace.name, sp->space_name, NAME_ID_SIZE);
		lockspace.host_id = sp->host_id;
		memcpy(&lockspace.host_id_disk, &sp->host_id_disk, sizeof(struct sanlk_disk));

		send(fd, &lockspace, sizeof(lockspace), MSG_NOSIGNAL);
	}
	pthread_mutex_unlock(&spaces_mutex);

	if (h_recv->data == SANLK_STATE_LOCKSPACE)
		return;

	/*
	 * send client and resource state:
	 * cst, cst.str, (rst, rst.str, resource, disk*N)*M
	 */

	for (ci = 0; ci <= client_maxi; ci++) {
		cl = &client[ci];

		if (!cl->used || !cl->pid)
			continue;

		str_len = print_client_state(cl, str);
		memset(&cst, 0, sizeof(cst));
		cst.type = SANLK_STATE_CLIENT;
		cst.data32 = cl->pid;
		strncpy(cst.name, cl->owner_name, NAME_ID_SIZE);
		cst.str_len = str_len;

		send(fd, &cst, sizeof(cst), MSG_NOSIGNAL);
		if (str_len)
			send(fd, str, str_len, MSG_NOSIGNAL);

		for (i = 0; i < SANLK_MAX_RESOURCES; i++) {
			token = cl->tokens[i];
			if (!token)
				continue;

			str_len = print_token_state(token, str);
			memset(&rst, 0, sizeof(rst));
			rst.type = SANLK_STATE_RESOURCE;
			strncpy(rst.name, token->resource_name, NAME_ID_SIZE);
			rst.str_len = str_len;

			send(fd, &rst, sizeof(rst), MSG_NOSIGNAL);
			if (str_len)
				send(fd, str, str_len, MSG_NOSIGNAL);

			memset(&resource, 0, sizeof(resource));
			strncpy(resource.lockspace_name, token->space_name, NAME_ID_SIZE);
			strncpy(resource.name, token->resource_name, NAME_ID_SIZE);
			resource.num_disks = token->num_disks;

			send(fd, &resource, sizeof(resource), MSG_NOSIGNAL);

			for (j = 0; j < token->num_disks; j++) {
				send(fd, &token->disks[j], sizeof(struct sanlk_disk), MSG_NOSIGNAL);
			}
		}
	}
}

static void cmd_log_dump(int fd, struct sm_header *h_recv)
{
	struct sm_header h;

	memcpy(&h, h_recv, sizeof(struct sm_header));

	/* can't send header until taking log_mutex to find the length */

	write_log_dump(fd, &h);
}

static void process_cmd_thread_lockspace(int ci_in, struct sm_header *h_recv)
{
	pthread_t cmd_thread;
	pthread_attr_t attr;
	struct cmd_args *ca;
	struct sm_header h;
	int rv;

	ca = malloc(sizeof(struct cmd_args));
	if (!ca) {
		rv = -ENOMEM;
		goto fail;
	}
	ca->ci_in = ci_in;
	memcpy(&ca->header, h_recv, sizeof(struct sm_header));

	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

	switch (h_recv->cmd) {
	case SM_CMD_ADD_LOCKSPACE:
		rv = pthread_create(&cmd_thread, &attr, cmd_add_lockspace_thread, ca);
		break;
	case SM_CMD_REM_LOCKSPACE:
		rv = pthread_create(&cmd_thread, &attr, cmd_rem_lockspace_thread, ca);
		break;
	};

	pthread_attr_destroy(&attr);
	if (rv < 0) {
		log_error("create cmd thread failed");
		goto fail_free;
	}

	return;

 fail_free:
	free(ca);
 fail:
	memcpy(&h, h_recv, sizeof(struct sm_header));
	h.length = sizeof(h);
	h.data = rv;
	h.data2 = 0;
	send(client[ci_in].fd, &h, sizeof(h), MSG_NOSIGNAL);
	close(client[ci_in].fd);
}

static void process_cmd_thread_resource(int ci_in, struct sm_header *h_recv)
{
	pthread_t cmd_thread;
	pthread_attr_t attr;
	struct cmd_args *ca;
	struct sm_header h;
	int rv, ci_target;

	if (h_recv->data2 != -1) {
		/* lease for another registered client with pid specified by data2 */
		ci_target = find_client_pid(h_recv->data2);
		if (ci_target < 0) {
			rv = -ENOENT;
			goto fail;
		}
	} else {
		/* lease for this registered client */
		ci_target = ci_in;
	}

	/* the target client must be registered */

	if (client[ci_target].pid <= 0) {
		rv = -EPERM;
		goto fail;
	}

	rv = set_cmd_active(ci_target, h_recv->cmd);
	if (rv < 0)
		goto fail;

	ca = malloc(sizeof(struct cmd_args));
	if (!ca) {
		rv = -ENOMEM;
		goto fail_active;
	}
	ca->ci_in = ci_in;
	ca->ci_target = ci_target;
	memcpy(&ca->header, h_recv, sizeof(struct sm_header));

	/* TODO: use a thread pool? */

	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

	switch (h_recv->cmd) {
	case SM_CMD_ACQUIRE:
		rv = pthread_create(&cmd_thread, &attr, cmd_acquire_thread, ca);
		break;
	case SM_CMD_RELEASE:
		rv = pthread_create(&cmd_thread, &attr, cmd_release_thread, ca);
		break;
	case SM_CMD_INQUIRE:
		rv = pthread_create(&cmd_thread, &attr, cmd_inquire_thread, ca);
		break;
	};

	pthread_attr_destroy(&attr);
	if (rv < 0) {
		log_error("create cmd thread failed");
		goto fail_free;
	}

	return;

 fail_free:
	free(ca);
 fail_active:
	set_cmd_active(ci_target, 0);
 fail:
	client_recv_all(ci_in, h_recv, 0);

	memcpy(&h, h_recv, sizeof(struct sm_header));
	h.length = sizeof(h);
	h.data = rv;
	h.data2 = 0;
	send(client[ci_in].fd, &h, sizeof(h), MSG_NOSIGNAL);
	client_back(ci_in, client[ci_in].fd);
}

static void process_cmd_daemon(int ci, struct sm_header *h_recv)
{
	int rv, pid, auto_close = 1;
	int fd = client[ci].fd;

	switch (h_recv->cmd) {
	case SM_CMD_REGISTER:
		rv = get_peer_pid(fd, &pid);
		if (rv < 0) {
			log_error("cmd_register ci %d fd %d get pid failed", ci, fd);
			break;
		}
		log_debug("cmd_register ci %d fd %d pid %d", ci, fd, pid);
		client[ci].pid = pid;
		client[ci].deadfn = client_pid_dead;
		auto_close = 0;
		break;
	case SM_CMD_SHUTDOWN:
		external_shutdown = 1;
		break;
	case SM_CMD_STATUS:
		cmd_status(fd, h_recv);
		break;
	case SM_CMD_LOG_DUMP:
		cmd_log_dump(fd, h_recv);
		break;
	};

	if (auto_close)
		close(fd);
}

static void process_connection(int ci)
{
	struct sm_header h;
	void (*deadfn)(int ci);
	int rv;

	memset(&h, 0, sizeof(h));

	rv = recv(client[ci].fd, &h, sizeof(h), MSG_WAITALL);
	if (!rv)
		return;
	if (rv < 0) {
		log_error("ci %d recv error %d", ci, errno);
		return;
	}
	if (rv != sizeof(h)) {
		log_error("ci %d recv size %d", ci, rv);
		goto dead;
	}
	if (h.magic != SM_MAGIC) {
		log_error("ci %d recv %d magic %x vs %x",
			  ci, rv, h.magic, SM_MAGIC);
		goto dead;
	}

	switch (h.cmd) {
	case SM_CMD_REGISTER:
	case SM_CMD_SHUTDOWN:
	case SM_CMD_STATUS:
	case SM_CMD_LOG_DUMP:
		process_cmd_daemon(ci, &h);
		break;
	case SM_CMD_ADD_LOCKSPACE:
	case SM_CMD_REM_LOCKSPACE:
		client_ignore(ci);
		process_cmd_thread_lockspace(ci, &h);
		break;
	case SM_CMD_ACQUIRE:
	case SM_CMD_RELEASE:
	case SM_CMD_INQUIRE:
		/* the main_loop needs to ignore this connection
		   while the thread is working on it */
		client_ignore(ci);
		process_cmd_thread_resource(ci, &h);
		break;
	default:
		log_error("ci %d cmd %d unknown", ci, h.cmd);
	};

	return;

 dead:
	deadfn = client[ci].deadfn;
	if (deadfn)
		deadfn(ci);
}

static void process_listener(int ci GNUC_UNUSED)
{
	int fd;
	int on = 1;

	fd = accept(client[ci].fd, NULL, NULL);
	if (fd < 0)
		return;

	setsockopt(fd, SOL_SOCKET, SO_PASSCRED, &on, sizeof(on));

	client_add(fd, process_connection, NULL);
}

static int setup_listener(void)
{
	int rv, fd, ci;

	rv = setup_listener_socket(&fd);
	if (rv < 0)
		return rv;

	ci = client_add(fd, process_listener, NULL);
	strcpy(client[ci].owner_name, "listener");
	return 0;
}

static void sigterm_handler(int sig GNUC_UNUSED)
{
	external_shutdown = 1;
}

static int make_dirs(void)
{
	mode_t old_umask;
	int rv;

	old_umask = umask(0022);
	rv = mkdir(SANLK_RUN_DIR, 0777);
	if (rv < 0 && errno != EEXIST)
		goto out;

	rv = 0;
 out:
	umask(old_umask);
	return rv;
}

static void setup_priority(void)
{
	struct sched_param sched_param;
	int rv;

	if (!options.high_priority)
		return;

	rv = mlockall(MCL_CURRENT | MCL_FUTURE);
	if (rv < 0) {
		log_error("mlockall failed");
	}

	rv = sched_get_priority_max(SCHED_RR);
	if (rv < 0) {
                log_error("could not get max scheduler priority err %d", errno);
		return;
	}

	sched_param.sched_priority = rv;
	rv = sched_setscheduler(0, SCHED_RR|SCHED_RESET_ON_FORK, &sched_param);
	if (rv < 0) {
		log_error("could not set RR|RESET_ON_FORK priority %d err %d",
			  sched_param.sched_priority, errno);
	}
}

static int do_daemon(void)
{
	struct sigaction act;
	int fd, rv;

	/* TODO: copy comprehensive daemonization method from libvirtd */

	if (!options.debug) {
		if (daemon(0, 0) < 0) {
			log_tool("cannot fork daemon\n");
			exit(EXIT_FAILURE);
		}
		umask(0);
	}

	memset(&act, 0, sizeof(act));
	act.sa_handler = sigterm_handler;
	rv = sigaction(SIGTERM, &act, NULL);
	if (rv < 0)
		return -rv;

	/*
	 * after creating dirs and setting up logging the daemon can
	 * use log_error/log_debug
	 */

	rv = make_dirs();
	if (rv < 0) {
		log_tool("cannot create logging dirs\n");
		return -1;
	}

	setup_logging();

	setup_priority();

	fd = lockfile(SANLK_RUN_DIR, SANLK_LOCKFILE_NAME);
	if (fd < 0)
		goto out;

	rv = setup_watchdog();
	if (rv < 0)
		goto out_lockfile;

	rv = setup_listener();
	if (rv < 0)
		goto out_lockfile;

	setup_token_manager();
	if (rv < 0)
		goto out_lockfile;

	setup_spaces();

	main_loop();

	close_token_manager();

	close_watchdog();

 out_lockfile:
	unlink_lockfile(fd, SANLK_RUN_DIR, SANLK_LOCKFILE_NAME);
 out:
	close_logging();
	return rv;
}

/* arg = <lockspace_name>:<host_id>:<path>:<offset> */

static int parse_arg_lockspace(char *arg)
{
	char *host_id = NULL;
	char *path = NULL;
	char *offset = NULL;

	if (arg)
		host_id = strstr(arg, ":");
	if (host_id)
		path = strstr(host_id+1, ":");
	if (host_id && path)
		offset = strstr(path+1, ":");

	if (host_id) {
		*host_id = '\0';
		host_id++;
	}
	if (path) {
		*path = '\0';
		path++;
	}
	if (offset) {
		*offset= '\0';
		offset++;
	}

	if (arg)
		strncpy(com.lockspace.name, arg, NAME_ID_SIZE);
	if (host_id)
		com.lockspace.host_id = atoll(host_id);
	if (path)
		strncpy(com.lockspace.host_id_disk.path, path, SANLK_PATH_LEN-1);
	if (offset)
		com.lockspace.host_id_disk.offset = atoll(offset);

	log_debug("lockspace %s host_id %llu path %s offset %llu",
		  com.lockspace.name,
		  (unsigned long long)com.lockspace.host_id,
		  com.lockspace.host_id_disk.path,
		  (unsigned long long)com.lockspace.host_id_disk.offset);

	return 0;
}

static int parse_arg_resource(char *arg)
{
	struct sanlk_resource *res;
	int rv, i;

	if (com.res_count >= SANLK_MAX_RESOURCES) {
		log_tool("resource args over max %d", SANLK_MAX_RESOURCES);
		return -1;
	}

	rv = sanlock_str_to_res(arg, &res);
	if (rv < 0) {
		log_tool("resource arg parse error %d\n", rv);
		return rv;
	}

	com.res_args[com.res_count] = res;
	com.res_count++;

	log_debug("resource %s %s num_disks %d flags %x lver %llu",
		  res->lockspace_name, res->name, res->num_disks, res->flags,
		  (unsigned long long)res->lver);
	for (i = 0; i < res->num_disks; i++) {
		log_debug("resource disk %s %llu", res->disks[i].path,
			  (unsigned long long)res->disks[i].offset);
	}
	return 0;
}

static void set_timeout(char *key, char *val)
{
	if (!strcmp(key, "io_timeout")) {
		to.io_timeout_seconds = atoi(val);
		log_debug("io_timeout_seconds %d", to.io_timeout_seconds);
		return;
	}

	if (!strcmp(key, "host_id_timeout")) {
		to.host_id_timeout_seconds = atoi(val);
		log_debug("host_id_timeout_seconds %d", to.host_id_timeout_seconds);
		return;
	}

	if (!strcmp(key, "host_id_renewal")) {
		to.host_id_renewal_seconds = atoi(val);
		log_debug("host_id_renewal_seconds %d", to.host_id_renewal_seconds);
		return;
	}

	if (!strcmp(key, "host_id_renewal_warn")) {
		to.host_id_renewal_warn_seconds = atoi(val);
		log_debug("host_id_renewal_warn_seconds %d", to.host_id_renewal_warn_seconds);
		return;
	}

	if (!strcmp(key, "host_id_renewal_fail")) {
		to.host_id_renewal_fail_seconds = atoi(val);
		log_debug("host_id_renewal_fail_seconds %d", to.host_id_renewal_fail_seconds);
		return;
	}

}

/* optstr format "abc=123,def=456,ghi=789" */

static void parse_arg_timeout(char *optstr)
{
	int copy_key, copy_val, i, kvi;
	char key[64], val[64];

	copy_key = 1;
	copy_val = 0;
	kvi = 0;

	for (i = 0; i < strlen(optstr); i++) {
		if (optstr[i] == ',') {
			set_timeout(key, val);
			memset(key, 0, sizeof(key));
			memset(val, 0, sizeof(val));
			copy_key = 1;
			copy_val = 0;
			kvi = 0;
			continue;
		}

		if (optstr[i] == '=') {
			copy_key = 0;
			copy_val = 1;
			kvi = 0;
			continue;
		}

		if (copy_key)
			key[kvi++] = optstr[i];
		else if (copy_val)
			val[kvi++] = optstr[i];

		if (kvi > 62) {
			log_error("invalid timeout parameter");
			return;
		}
	}

	set_timeout(key, val);
}

#define RELEASE_VERSION "1.2"

/* 
 * daemon: acquires leases for the local host_id, associates them with a local
 * pid, and releases them when the associated pid exits.
 *
 * client: ask daemon to acquire/release leases associated with a given pid.
 *
 * direct: acquires and releases leases directly for the local host_id by
 * reading and writing storage directly.
 */

static void print_usage(void)
{
	printf("Usage:\n");
	printf("sanlock <type> <action> [options]\n\n");

	printf("types:\n");
	printf("  version		print version\n");
	printf("  help			print usage\n");
	printf("  daemon		start daemon\n");
	printf("  client		send request to daemon (default type if none given)\n");
	printf("  direct		access storage directly (no coordination with daemon)\n");
	printf("\n");
	printf("client actions:		ask daemon to:\n");
	printf("  status		send internal state\n");
	printf("  log_dump		send internal debug buffer\n");
	printf("  shutdown		kill pids, release leases and exit\n");
	printf("  add_lockspace		add a lockspace, acquiring a host_id in it\n");
	printf("  rem_lockspace		remove a lockspace, releasing our host_id in it\n");
	printf("  command		acquire leases for the calling pid, then run command\n");
	printf("  acquire		acquire leases for a given pid\n");
	printf("  release		release leases for a given pid\n");
	printf("  inquire		display leases held by a given pid\n");
	printf("\n");
	printf("direct actions:		read/write storage directly to:\n");
	printf("  init			initialize disk areas for host_id and resource leases\n");
	printf("  dump			print initialized leases\n");
	printf("  acquire		acquire leases\n");
	printf("  release		release leases\n");
	printf("  acquire_id		acquire a host_id lease\n");
	printf("  release_id		release a host_id lease\n");
	printf("  renew_id		renew a host_id lease\n");
	printf("\n");
	printf("daemon\n");
	printf("  -D			debug: no fork and print all logging to stderr\n");
	printf("  -L <level>		write logging at level and up to logfile (-1 none)\n");
	printf("  -S <level>		write logging at level and up to syslog (-1 none)\n");
	printf("  -w <num>		use watchdog through wdmd (1 yes, 0 no, default %d)\n", DEFAULT_USE_WATCHDOG);
	printf("  -a <num>		use async io (1 yes, 0 no, default %d)\n", DEFAULT_USE_AIO);
	printf("  -h <num>		use high priority features (1 yes, 0 no, default %d)\n", DEFAULT_HIGH_PRIORITY);
	printf("                        includes max realtime scheduling priority, mlockall\n");
	printf("  -o <key=n,key=n,...>	change default timeouts in seconds, key (default):\n");
	printf("                        io_timeout (%d)\n", DEFAULT_IO_TIMEOUT_SECONDS);
	printf("                        host_id_renewal (%d)\n", DEFAULT_HOST_ID_RENEWAL_SECONDS);
	printf("                        host_id_renewal_warn (%d)\n", DEFAULT_HOST_ID_RENEWAL_WARN_SECONDS);
	printf("                        host_id_renewal_fail (%d)\n", DEFAULT_HOST_ID_RENEWAL_FAIL_SECONDS);
	printf("                        host_id_timeout (%d)\n", DEFAULT_HOST_ID_TIMEOUT_SECONDS);
	printf("\n");
	printf("client status\n");
	printf("  -D			debug: print extra internal state for debugging\n");
	printf("\n");
	printf("client log_dump\n");
	printf("\n");
	printf("client shutdown\n");
	printf("\n");
	printf("client add_lockspace -s LOCKSPACE\n");
	printf("\n");
	printf("client rem_lockspace -s LOCKSPACE\n");
	printf("\n");
	printf("client command -r RESOURCE -c <path> <args>\n");
	printf("  -n <num_hosts>	change num_hosts in leases when acquired\n");
	printf("  -c <path> <args>	run command with args, -c must be final option\n");
	printf("\n");
	printf("client acquire -p <pid> -r RESOURCE\n");
	printf("  -p <pid>		process that lease should be added for\n");
	printf("\n");
	printf("client release -p <pid> -r RESOURCE\n");
	printf("  -p <pid>		process whose lease should be released\n");
	printf("\n");
	printf("client inquire -p <pid>\n");
	printf("  -p <pid>		process whose resource leases should be displayed\n");
	printf("\n");

	printf("direct init -n <num_hosts> [-s LOCKSPACE] [-r RESOURCE]\n");
	printf("  -a <num>		use async io (1 yes, 0 no)\n");
	printf("  -n <num_hosts>	host_id's from 1 to num_hosts will be able to acquire\n");
	printf("                        a resource lease.  This is also number of sectors that\n");
	printf("                        are read when paxos is run to acquire a resource lease.\n");
	printf("  -m <max_hosts>	disk space is allocated to support this many hosts\n");
	printf("                        (default max_hosts %d)\n", DEFAULT_MAX_HOSTS);
	printf("  -s LOCKSPACE		initialize host_id leases for host_id's 1 to max_hosts\n");
	printf("                        (the specific host_id in the LOCKSPACE arg is ignored)\n");
	printf("  -r RESOURCE           initialize a resource lease for use by host_id's 1 to\n");
	printf("                        num_hosts (num_hosts can be extended up to max_hosts)\n");
	printf("\n");
	printf("direct dump <path>[:<offset>] [options]\n");
	printf("  -D			debug: print extra info for debugging\n");
	printf("  -a <num>		use async io (1 yes, 0 no)\n");
	printf("\n");
	printf("direct acquire|release -i <num> -g <num> -r RESOURCE\n");
	printf("  -a <num>		use async io (1 yes, 0 no)\n");
	printf("  -n <num_hosts>	change num_hosts in leases when acquired\n");
	printf("  -i <num>		host_id of local host\n");
	printf("  -g <num>		host_id generation of local host\n");
	printf("\n");
	printf("direct acquire_id|renew_id|release_id -s LOCKSPACE\n");
	printf("  -a <num>		use async io (1 yes, 0 no)\n");
	printf("\n");

	printf("LOCKSPACE = <lockspace_name>:<host_id>:<path>:<offset>\n");
	printf("  <lockspace_name>	name of lockspace\n");
	printf("  <host_id>		local host identifier in lockspace\n");
	printf("  <path>		disk path where host_id leases are written\n");
	printf("  <offset>		offset on disk, in bytes\n");
	printf("\n");
	printf("RESOURCE = <lockspace_name>:<resource_name>:<path>:<offset>[:<lver>]\n");
	printf("  <lockspace_name>	name of lockspace\n");
	printf("  <resource_name>	name of resource being leased\n");
	printf("  <path>		disk path where resource leases are written\n");
	printf("  <offset>		offset on disk in bytes\n");
	printf("  <lver>                optional disk leader version of resource for acquire\n");
	printf("\n");
}

static int read_command_line(int argc, char *argv[])
{
	char optchar;
	char *optionarg;
	char *p;
	char *arg1 = argv[1];
	char *act;
	int i, j, len, begin_command = 0;

	if (argc < 2 || !strcmp(arg1, "help") || !strcmp(arg1, "--help") ||
	    !strcmp(arg1, "-h")) {
		print_usage();
		exit(EXIT_SUCCESS);
	}

	if (!strcmp(arg1, "version") || !strcmp(arg1, "--version") ||
	    !strcmp(arg1, "-V")) {
		printf("%s %s (built %s %s)\n",
		       argv[0], RELEASE_VERSION, __DATE__, __TIME__);
		exit(EXIT_SUCCESS);
	}

	if (!strcmp(arg1, "daemon")) {
		com.type = COM_DAEMON;
		i = 2;
	} else if (!strcmp(arg1, "direct")) {
		com.type = COM_DIRECT;
		act = argv[2];
		i = 3;
	} else if (!strcmp(arg1, "client")) {
		com.type = COM_CLIENT;
		act = argv[2];
		i = 3;
	} else {
		com.type = COM_CLIENT;
		act = argv[1];
		i = 2;
	}

	switch (com.type) {
	case COM_DAEMON:
		break;

	case COM_CLIENT:
		if (!strcmp(act, "status"))
			com.action = ACT_STATUS;
		else if (!strcmp(act, "log_dump"))
			com.action = ACT_LOG_DUMP;
		else if (!strcmp(act, "shutdown"))
			com.action = ACT_SHUTDOWN;
		else if (!strcmp(act, "add_lockspace"))
			com.action = ACT_ADD_LOCKSPACE;
		else if (!strcmp(act, "rem_lockspace"))
			com.action = ACT_REM_LOCKSPACE;
		else if (!strcmp(act, "command"))
			com.action = ACT_COMMAND;
		else if (!strcmp(act, "acquire"))
			com.action = ACT_ACQUIRE;
		else if (!strcmp(act, "release"))
			com.action = ACT_RELEASE;
		else if (!strcmp(act, "inquire"))
			com.action = ACT_INQUIRE;
		else {
			log_tool("client action \"%s\" is unknown", act);
			exit(EXIT_FAILURE);
		}
		break;

	case COM_DIRECT:
		if (!strcmp(act, "init"))
			com.action = ACT_INIT;
		else if (!strcmp(act, "dump"))
			com.action = ACT_DUMP;
		else if (!strcmp(act, "acquire"))
			com.action = ACT_ACQUIRE;
		else if (!strcmp(act, "release"))
			com.action = ACT_RELEASE;
		else if (!strcmp(act, "acquire_id"))
			com.action = ACT_ACQUIRE_ID;
		else if (!strcmp(act, "release_id"))
			com.action = ACT_RELEASE_ID;
		else if (!strcmp(act, "renew_id"))
			com.action = ACT_RENEW_ID;
		else {
			log_tool("direct action \"%s\" is unknown", act);
			exit(EXIT_FAILURE);
		}
		break;
	};


	/* the only action that has an option without dash-letter prefix */
	if (com.action == ACT_DUMP) {
		optionarg = argv[i++];
		com.dump_path = strdup(optionarg);
	}

	for (; i < argc; ) {
		p = argv[i];

		if ((p[0] != '-') || (strlen(p) != 2)) {
			log_tool("unknown option %s", p);
			log_tool("space required before option value");
			exit(EXIT_FAILURE);
		}

		optchar = p[1];
		i++;

		/* the only option that does not have optionarg */
		if (optchar == 'D') {
			options.debug = 1;
			log_stderr_priority = LOG_DEBUG;
			continue;
		}

		if (i >= argc) {
			log_tool("option '%c' requires arg", optchar);
			exit(EXIT_FAILURE);
		}

		optionarg = argv[i];

		switch (optchar) {
		case 'L':
			log_logfile_priority = atoi(optionarg);
			break;
		case 'S':
			log_syslog_priority = atoi(optionarg);
			break;
		case 'a':
			options.use_aio = atoi(optionarg);
			break;
		case 'w':
			options.use_watchdog = atoi(optionarg);
			break;
		case 'h':
			options.high_priority = atoi(optionarg);
			break;
		case 'o':
			parse_arg_timeout(optionarg); /* to */
			break;

		case 'n':
			com.num_hosts = atoi(optionarg);
			break;
		case 'm':
			com.max_hosts = atoi(optionarg);
			break;
		case 'p':
			com.pid = atoi(optionarg);
			break;
		case 'i':
			com.local_host_id = atoll(optionarg);
			break;
		case 'g':
			com.local_host_generation = atoll(optionarg);
			break;

		case 's':
			parse_arg_lockspace(optionarg); /* com.lockspace */
			break;
		case 'r':
			parse_arg_resource(optionarg); /* com.res_args[] */
			break;

		case 'c':
			begin_command = 1;
			break;
		default:
			log_tool("unknown option: %c", optchar);
			exit(EXIT_FAILURE);
		};


		if (begin_command)
			break;

		i++;
	}

	/*
	 * the remaining args are for the command
	 *
	 * sanlock -r foo -n 2 -d bar:0 -c /bin/cmd -X -Y -Z
	 * argc = 12
	 * loop above breaks with i = 8, argv[8] = "/bin/cmd"
	 *
	 * cmd_argc = 4 = argc (12) - i (8)
	 * cmd_argv[0] = "/bin/cmd"
	 * cmd_argv[1] = "-X"
	 * cmd_argv[2] = "-Y"
	 * cmd_argv[3] = "-Z"
	 * cmd_argv[4] = NULL (required by execv)
	 */

	if (begin_command) {
		cmd_argc = argc - i;

		if (cmd_argc < 1) {
			log_tool("command option (-c) requires an arg");
			return -EINVAL;
		}

		len = (cmd_argc + 1) * sizeof(char *); /* +1 for final NULL */
		cmd_argv = malloc(len);
		if (!cmd_argv)
			return -ENOMEM;
		memset(cmd_argv, 0, len);

		for (j = 0; j < cmd_argc; j++) {
			cmd_argv[j] = strdup(argv[i++]);
			if (!cmd_argv[j])
				return -ENOMEM;
		}

		strncpy(command, cmd_argv[0], COMMAND_MAX - 1);
	}

	return 0;
}

static int do_client(void)
{
	struct sanlk_resource **res_args = NULL;
	struct sanlk_resource *res;
	char *res_state = NULL;
	int i, fd, rv = 0;

	if (com.action == ACT_COMMAND || com.action == ACT_ACQUIRE) {
		if (com.num_hosts) {
			for (i = 0; i < com.res_count; i++) {
				res = com.res_args[i];
				res->flags |= SANLK_RES_NUM_HOSTS;
				res->data32 = com.num_hosts;
			}
		}
	}

	switch (com.action) {
	case ACT_STATUS:
		rv = sanlock_status(options.debug);
		break;

	case ACT_LOG_DUMP:
		rv = sanlock_log_dump();
		break;

	case ACT_SHUTDOWN:
		log_tool("shutdown");
		rv = sanlock_shutdown();
		log_tool("shutdown done %d", rv);
		break;

	case ACT_COMMAND:
		log_tool("register");
		fd = sanlock_register();
		log_tool("register done %d", fd);

		if (fd < 0)
			goto out;

		log_tool("acquire fd %d", fd);
		rv = sanlock_acquire(fd, -1, 0, com.res_count, com.res_args, NULL);
		log_tool("acquire done %d", rv);

		if (rv < 0)
			goto out;

		if (!command[0]) {
			while (1)
				sleep(10);
		}
		execv(command, cmd_argv);
		perror("execv failed");

		/* release happens automatically when pid exits and
		   daemon detects POLLHUP on registered connection */
		break;

	case ACT_ADD_LOCKSPACE:
		log_tool("add_lockspace");
		rv = sanlock_add_lockspace(&com.lockspace, 0);
		log_tool("add_lockspace done %d", rv);
		break;

	case ACT_REM_LOCKSPACE:
		log_tool("rem_lockspace");
		rv = sanlock_rem_lockspace(&com.lockspace, 0);
		log_tool("rem_lockspace done %d", rv);
		break;

	case ACT_ACQUIRE:
		log_tool("acquire pid %d", com.pid);
		rv = sanlock_acquire(-1, com.pid, 0, com.res_count, com.res_args, NULL);
		log_tool("acquire done %d", rv);
		break;

	case ACT_RELEASE:
		log_tool("release pid %d", com.pid);
		rv = sanlock_release(-1, com.pid, 0, com.res_count, com.res_args);
		log_tool("release done %d", rv);
		break;

	case ACT_INQUIRE:
		log_tool("inquire pid %d", com.pid);
		rv = sanlock_inquire(-1, com.pid, 0, &com.res_count, &res_state);
		log_tool("inquire done %d res_count %d", rv, com.res_count);
		if (rv < 0)
			break;
		log_tool("\"%s\"", res_state);

		if (!options.debug)
			break;

		com.res_count = 0;

		rv = sanlock_state_to_args(res_state, &com.res_count, &res_args);
		log_tool("\nstate_to_args done %d res_count %d", rv, com.res_count);
		if (rv < 0)
			break;

		free(res_state);
		res_state = NULL;

		for (i = 0; i < com.res_count; i++) {
			res = res_args[i];
			log_tool("\"%s:%s:%s:%llu:%llu\"",
				 res->lockspace_name, res->name, res->disks[0].path,
				 (unsigned long long)res->disks[0].offset,
				 (unsigned long long)res->lver);
		}

		rv = sanlock_args_to_state(com.res_count, res_args, &res_state);
		log_tool("\nargs_to_state done %d", rv);
		if (rv < 0)
			break;
		log_tool("\"%s\"", res_state);
		break;

	default:
		log_tool("action not implemented");
		rv = -1;
	}
 out:
	return rv;
}

static int do_direct(void)
{
	int rv;

	switch (com.action) {
	case ACT_INIT:
		rv = sanlock_direct_init();
		break;

	case ACT_DUMP:
		rv = sanlock_direct_dump();
		break;

	case ACT_ACQUIRE:
		rv = sanlock_direct_acquire();
		break;

	case ACT_RELEASE:
		rv = sanlock_direct_release();
		break;

	case ACT_ACQUIRE_ID:
		rv = sanlock_direct_acquire_id();
		break;

	case ACT_RELEASE_ID:
		rv = sanlock_direct_release_id();
		break;

	case ACT_RENEW_ID:
		rv = sanlock_direct_renew_id();
		break;

	default:
		log_tool("direct action %d not known", com.action);
		rv = -1;
	}

	return rv;
}

int main(int argc, char *argv[])
{
	int rv;
	
	memset(&com, 0, sizeof(com));
	com.max_hosts = DEFAULT_MAX_HOSTS;
	com.pid = -1;

	memset(&options, 0, sizeof(options));
	options.use_aio = DEFAULT_USE_AIO;
	options.use_watchdog = DEFAULT_USE_WATCHDOG;
	options.high_priority = DEFAULT_HIGH_PRIORITY;

	memset(&to, 0, sizeof(to));
	to.io_timeout_seconds = DEFAULT_IO_TIMEOUT_SECONDS;
	to.host_id_renewal_seconds = DEFAULT_HOST_ID_RENEWAL_SECONDS;
	to.host_id_renewal_fail_seconds = DEFAULT_HOST_ID_RENEWAL_FAIL_SECONDS;
	to.host_id_renewal_warn_seconds = DEFAULT_HOST_ID_RENEWAL_WARN_SECONDS;
	to.host_id_timeout_seconds = DEFAULT_HOST_ID_TIMEOUT_SECONDS;

	rv = read_command_line(argc, argv);
	if (rv < 0)
		goto out;

	switch (com.type) {
	case COM_DAEMON:
		rv = do_daemon();
		break;

	case COM_CLIENT:
		rv = do_client();
		break;

	case COM_DIRECT:
		rv = do_direct();
		break;
	};
 out:
	return rv;
}
