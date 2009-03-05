/*
 * 2008+ Copyright (c) Evgeniy Polyakov <zbr@ioremap.net>
 * All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "elliptics.h"
#include "interface.h"

static int dnet_send_list_entry(struct dnet_net_state *st, struct el_cmd *req, unsigned char *id)
{
	int fd, err;
	char file[EL_ID_SIZE*2 + sizeof(EL_HISTORY_SUFFIX) + 5];
	struct el_cmd *cmd;
	struct el_attr *a;
	struct el_io_attr *io;
	struct stat stat;

	snprintf(file, sizeof(file), "%02x/%s%s", id[0], el_dump_id(id), EL_HISTORY_SUFFIX);

	fd = openat(st->n->rootfd, file, O_RDONLY);
	if (fd <= 0) {
		err = -errno;
		ulog_err("%s: failed to open history file '%s'", el_dump_id(id), file);
		goto err_out_exit;
	}

	err = fstat(fd, &stat);
	if (err) {
		err = -errno;
		ulog_err("%s: failed to stat history file '%s'", el_dump_id(id), file);
		goto err_out_close;
	}

	cmd = malloc(sizeof(struct el_cmd) + sizeof(struct el_attr) + sizeof(struct el_io_attr));
	if (!cmd) {
		ulog("%s: failed to allocate list reply.\n", el_dump_id(id));
		err = -ENOMEM;
		goto err_out_close;
	}

	a = (struct el_attr *)(cmd + 1);
	io = (struct el_io_attr *)(a + 1);

	memcpy(cmd->id, req->id, EL_ID_SIZE);
	cmd->size = sizeof(struct el_attr) + sizeof(struct el_io_attr) + stat.st_size;
	cmd->trans = req->trans | DNET_TRANS_REPLY;
	cmd->status = 0;
	cmd->flags = DNET_FLAGS_MORE;

	a->flags = 0;
	a->size = sizeof(struct el_io_attr) + stat.st_size;
	a->cmd = DNET_CMD_LIST;

	memcpy(io->id, id, EL_ID_SIZE);
	io->size = stat.st_size;
	io->offset = 0;
	io->flags = 0;

	el_convert_cmd(cmd);
	el_convert_attr(a);
	el_convert_io_attr(io);

	err = dnet_sendfile_data(st, file, fd, 0, stat.st_size,
			cmd, sizeof(struct el_cmd) + sizeof(struct el_attr) + sizeof(struct el_io_attr));
	if (err)
		goto err_out_free;

	free(cmd);
	close(fd);
	return 0;

err_out_free:
	free(cmd);
err_out_close:
	close(fd);
err_out_exit:
	return err;
}

static void dnet_convert_name_to_id(char *name, unsigned char *id)
{
	int i;
	char sub[3];

	sub[2] = '\0';
	for (i=0; i<EL_ID_SIZE; i++) {
		sub[0] = name[2*i];
		sub[1] = name[2*i + 1];
		id[i] = strtol(sub, NULL, 16);
	}
}

static int dnet_listdir(struct dnet_net_state *st, struct el_cmd *cmd,
		char *sub, unsigned char *first_id)
{
	int fd, err = 0;
	DIR *dir;
	struct dirent64 *d;
	unsigned char id[EL_ID_SIZE];
	unsigned int len;

	fd = openat(st->n->rootfd, sub, O_RDONLY);
	if (fd == -1) {
		err = -errno;
		//ulog_err("Failed to open '%s/%s'", st->n->root, sub);
		return err;
	}

	dir = fdopendir(fd);
	err = 0;

	while ((d = readdir64(dir)) != NULL) {
		if (d->d_name[0] == '.' && d->d_name[1] == '\0')
			continue;
		if (d->d_name[0] == '.' && d->d_name[1] == '.' && d->d_name[2] == '\0')
			continue;

		if (d->d_type != DT_REG)
			continue;

		len = strlen(d->d_name);

		if (len != strlen(EL_HISTORY_SUFFIX) + EL_ID_SIZE*2)
			continue;

		if (strcmp(&d->d_name[EL_ID_SIZE*2], EL_HISTORY_SUFFIX))
			continue;

		dnet_convert_name_to_id(d->d_name, id);

		if (first_id) {
			err = el_id_cmp(first_id, id);
			if (err > 0)
				continue;
		}

		err = dnet_send_list_entry(st, cmd, id);

		ulog("%s -> %s.\n", d->d_name, el_dump_id(id));
	}

	close(fd);

	return 0;
}

int dnet_cmd_list(struct dnet_net_state *st, struct el_cmd *cmd)
{
	char sub[3];
	unsigned char start;
	int err;

	sprintf(sub, "%02x", cmd->id[0]);
	
	err = dnet_listdir(st, cmd, sub, cmd->id);
	if (err && (err != -ENOENT))
		return err;

	if (cmd->id[0] != 0) {
		for (start = cmd->id[0]-1; start != 0; --start) {
			sprintf(sub, "%02x", start);

			err = dnet_listdir(st, cmd, sub, NULL);
			if (err && (err != -ENOENT))
				return err;
		}
	}

	return 0;
}

static int dnet_process_existing_history(struct dnet_net_state *st __unused, struct el_io_attr *io, int fd)
{
	int err;
	struct stat stat;
	struct el_io_attr last_io;
	off_t off;

	err = fstat(fd, &stat);
	if (err < 0) {
		err = -errno;
		ulog_err("%s: failed to stat the history file", el_dump_id(io->id));
		goto err_out_exit;
	}

	if (!stat.st_size || (stat.st_size % sizeof(struct el_io_attr))) {
		uloga("%s: corrupted history file: size %lu not multiple of %u.\n",
				el_dump_id(io->id), stat.st_size, sizeof(struct el_io_attr));
		err = -EINVAL;
		goto err_out_exit;
	}

	off = lseek(fd, -sizeof(struct el_io_attr), SEEK_END);
	if (off < 0) {
		err = -errno;
		ulog_err("%s: corrupted history file: can not seek to the end", el_dump_id(io->id));
		goto err_out_exit;
	}

	err = read(fd, &last_io, sizeof(struct el_io_attr));
	if (err <= 0) {
		err = -errno;
		ulog_err("%s: corrupted history file: can not read the last transaction history entry", el_dump_id(io->id));
		goto err_out_exit;
	}

	err = memcmp(io->id, last_io.id, EL_ID_SIZE);

	ulog("%s: the last local update: offset: %llu, size: %llu, id: ",
			el_dump_id(io->id), last_io.offset, last_io.size);
	uloga("%s, same: %d.\n", el_dump_id(last_io.id), !err);

	return err;

err_out_exit:
	return err;
}

static int dnet_read_complete_history(struct dnet_trans *t, struct dnet_net_state *st)
{
	int err;
	struct el_cmd *cmd = &t->cmd;
	char tmp[2*EL_ID_SIZE + sizeof(EL_HISTORY_SUFFIX) + 5 + 4];
	char file[2*EL_ID_SIZE + sizeof(EL_HISTORY_SUFFIX) + 5 + 4];
	struct dnet_io_completion *c = t->priv;

	ulog("%s: file: '%s'.\n", el_dump_id(cmd->id), c->file);

	if (cmd->status != 0 || cmd->size == 0)
		goto out;

	if (cmd->flags & DNET_FLAGS_DESTROY) {
	}

	err = dnet_read_complete(t, st);
	if (err)
		return err;

	snprintf(tmp, sizeof(tmp), "%s%s.tmp", c->file, EL_HISTORY_SUFFIX);
	snprintf(file, sizeof(file), "%s%s", c->file, EL_HISTORY_SUFFIX);

	err = renameat(st->n->rootfd, tmp, st->n->rootfd, file);
	if (err) {
		err = -errno;
		ulog_err("%s: failed to rename '%s' -> '%s'", el_dump_id(cmd->id), tmp, file);
		return err;
	}

out:
	return 0;
}

static int dnet_process_history(struct dnet_net_state *st, struct el_io_attr *io)
{
	char file[2*EL_ID_SIZE + sizeof(EL_HISTORY_SUFFIX) + 5 + 4];
	int fd, err;
	struct dnet_io_completion *cmp;
	char dir[3];
	struct el_io_attr req;

	snprintf(file, sizeof(file), "%02x/%s%s", io->id[0], el_dump_id(io->id), EL_HISTORY_SUFFIX);

	fd = openat(st->n->rootfd, file, O_RDONLY);
	if (fd >= 0) {
		err = dnet_process_existing_history(st, io, fd);
		if (err)
			goto err_out_close;

		close(fd);
		goto out;
	}
	if (errno != ENOENT) {
		err = -errno;
		ulog_err("%s: failed to open history file '%s'", el_dump_id(io->id), file);
		goto err_out_exit;
	}

	sprintf(dir, "%02x", io->id[0]);
	err = mkdirat(st->n->rootfd, dir, 0755);
	if (err < 0) {
		if (errno != EEXIST) {
			err = -errno;
			ulog_err("%s: failed to create dir '%s' in the root '%s'",
					el_dump_id(io->id), dir, st->n->root);
			goto err_out_exit;
		}
	}

	snprintf(file, sizeof(file), "%02x/%s%s.tmp", io->id[0], el_dump_id(io->id), EL_HISTORY_SUFFIX);

	fd = openat(st->n->rootfd, file, O_RDWR | O_CREAT | O_TRUNC | O_LARGEFILE, 0644);
	if (fd < 0) {
		err = -errno;
		ulog_err("%s: failed to create history file '%s'", el_dump_id(io->id), file);
		goto err_out_exit;
	}

	err = write(fd, io+1, io->size);
	if (err <= 0) {
		err = -errno;
		ulog_err("%s: failed to write history file '%s'", el_dump_id(io->id), file);
		goto err_out_close;
	}

	close(fd);

	cmp = malloc(sizeof(struct dnet_io_completion) + sizeof(file) + strlen(st->n->root));
	if (!cmp) {
		err = -ENOMEM;
		ulog("%s: failed to allocate read completion structure.\n", el_dump_id(io->id));
		goto err_out_exit;
	}

	memcpy(req.id, io->id, EL_ID_SIZE);
	req.size = 0;
	req.offset = 0;

	cmp->offset = 0;
	cmp->size = 0;
	cmp->file = (char *)(cmp + 1);

	snprintf(cmp->file, sizeof(file), "%02x/%s", io->id[0], el_dump_id(io->id));

	err = dnet_read_object(st->n, &req, dnet_read_complete_history, cmp);
	if (err)
		goto err_out_exit;
out:
	return 0;

err_out_close:
	close(fd);
err_out_exit:
	return err;
}

static int dnet_recv_list_complete(struct dnet_trans *t, struct dnet_net_state *st)
{
	struct el_cmd *cmd = &t->cmd;
	__u64 size = cmd->size;
	void *data = t->data;
	int err = cmd->status;

	if (size < sizeof(struct el_attr) + sizeof(struct el_io_attr))
		goto out;

	while (size) {
		struct el_attr *a = data;
		struct el_io_attr *io;

		el_convert_attr(a);

		if (a->size < sizeof(struct el_io_attr)) {
			ulog("%s: wrong list reply attribute size: %llu, mut be greater or equal than %u.\n",
					el_dump_id(cmd->id), (unsigned long long)a->size, sizeof(struct el_io_attr));
			err = -EPROTO;
			goto out;
		}

		io = (struct el_io_attr *)(a + 1);

		el_convert_io_attr(io);

		if (size < sizeof(struct el_attr) + sizeof(struct el_io_attr) + io->size) {
			ulog("%s: wrong list reply IO attribute size: %llu, mut be less or equal than %llu.\n",
					el_dump_id(cmd->id), (unsigned long long)io->size,
					(unsigned long long)size - sizeof(struct el_attr) - sizeof(struct el_io_attr));
			err = -EPROTO;
			goto out;
		}

		/*
		 * Process the received history.
		 */

		err = dnet_process_history(st, io);

		ulog("%s: list entry offset: %llu, size: %llu, err: %d.\n", el_dump_id(io->id),
				(unsigned long long)io->offset, (unsigned long long)io->size, err);

		data += sizeof(struct el_attr) + sizeof(struct el_io_attr) + io->size;
		size -= sizeof(struct el_attr) + sizeof(struct el_io_attr) + io->size;
	}

out:
	ulog("%s: listing completed with status: %d, size: %llu, err: %d.\n",
			el_dump_id(cmd->id), cmd->status, cmd->size, err);
	return err;
}

int dnet_recv_list(struct dnet_node *n)
{
	struct dnet_trans *t;
	struct el_cmd *cmd;
	struct el_attr *a;
	struct dnet_net_state *st;
	int err;

	t = malloc(sizeof(struct dnet_trans) + sizeof(struct el_cmd) + sizeof(struct el_attr));
	if (!t) {
		err = -ENOMEM;
		goto err_out_exit;
	}

	memset(t, 0, sizeof(struct dnet_trans));

	t->complete = dnet_recv_list_complete;

	cmd = (struct el_cmd *)(t + 1);
	a = (struct el_attr *)(cmd + 1);

	memcpy(cmd->id, n->id, EL_ID_SIZE);
	cmd->flags = DNET_FLAGS_NEED_ACK;
	cmd->status = 0;
	cmd->trans = 0;
	cmd->size = sizeof(struct el_attr);

	a->cmd = DNET_CMD_LIST;
	a->size = 0;
	a->flags = 0;

	t->st = st = dnet_state_get_first(n, n->st);
	if (!st) {
		err = -ENOENT;
		ulog("%s: can not get output state.\n", el_dump_id(n->id));
		goto err_out_destroy;
	}

	err = dnet_trans_insert(t);
	if (err)
		goto err_out_destroy;

	cmd->trans = t->trans;

	el_convert_cmd(cmd);
	el_convert_attr(a);

	pthread_mutex_lock(&st->lock);
	err = dnet_send(st, cmd, sizeof(struct el_cmd) + sizeof(struct el_attr));
	if (err)
		goto err_out_unlock;
	pthread_mutex_unlock(&st->lock);

	return 0;

err_out_unlock:
	pthread_mutex_unlock(&st->lock);
err_out_destroy:
	dnet_trans_destroy(t);
err_out_exit:
	return err;
}
