/**
 * @file
 * Help system
 *
 * @authors
 * Copyright (C) 2018 Richard Russon <rich@flatcap.org>
 *
 * @copyright
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 2 of the License, or (at your option) any later
 * version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "config.h"
#include <stddef.h>
#include <stdbool.h>
#include <time.h>
#include "mutt/mutt.h"
#include "body.h"
#include "context.h"
#include "envelope.h"
#include "header.h"
#include "mailbox.h"
#include "mx.h"
#include "protos.h"

static int help_open(struct Context *ctx)
{
  if (!ctx || (ctx->magic != MUTT_HELP))
    return -1;

  mutt_debug(1, "entering help_open\n");

  ctx->msgcount = 10;
  mx_alloc_memory(ctx);

  time_t now = time(NULL) - 864000; /* minus 10 days */

  char buf[32];
  for (size_t i = 0; i < 10; i++)
  {
    struct Body *b = mutt_body_new();
    b->type = TYPETEXT;
    b->subtype = mutt_str_strdup("plain");
    b->encoding = ENC7BIT;
    b->length = -1;
    b->disposition = DISPINLINE;

    struct Envelope *e = mutt_env_new();
    snprintf(buf, sizeof(buf), "message %ld", i);
    e->subject = mutt_str_strdup(buf);
    e->from = mutt_addr_parse_list(NULL, "Richard Russon <rich@flatcap.org>");

    struct Header *h = mutt_header_new();
    h->content = b;
    h->env = e;
    h->date_sent = now + 86400 * i;
    h->received = now + 86400 * i;
    h->index = i;
    ctx->hdrs[i] = h;
  }
  mx_update_context(ctx, 10);
  return 0;
}

static int help_open_append(struct Context *ctx, int flags)
{
  mutt_debug(1, "entering help_open_append\n");
  return -1;
}

static int help_close(struct Context *ctx)
{
  mutt_debug(1, "entering help_close\n");
  return 0;
}

static int help_check(struct Context *ctx, int *index_hint)
{
  mutt_debug(1, "entering help_check\n");
  return 0;
}

static int help_sync(struct Context *ctx, int *index_hint)
{
  mutt_debug(1, "entering help_sync\n");
  return 0;
}

static int help_open_msg(struct Context *ctx, struct Message *msg, int msgno)
{
  mutt_debug(1, "entering help_open_msg: %d, %s\n", msgno, ctx->hdrs[msgno]->env->subject);

  char buf[PATH_MAX];
  mutt_mktemp(buf, sizeof(buf));
  FILE *fp = mutt_file_fopen(buf, "w+");
  if (!fp)
    return -1;

  fprintf(fp, "From rich@flatcap.org Sun Feb 11 02:06:47 2018\n");
  fprintf(fp, "Subject: test message\n");
  fprintf(fp, "From: Richard Russon <rich@flatcap.org>\n");
  fprintf(fp, "To: john@example.com\n");
  fprintf(fp, "\n");
  ctx->hdrs[msgno]->content->offset = ftello(fp);

  for (size_t i = 0; i < 10; i++)
    fprintf(fp, "contents %03ld\n", (msgno * 100) + i);

  ctx->hdrs[msgno]->content->length = ftello(fp);
  msg->fp = fp;
  msg->path = mutt_str_strdup(buf);
  return 0;
}

static int help_close_msg(struct Context *ctx, struct Message *msg)
{
  mutt_debug(1, "entering help_close_msg\n");
  mutt_file_fclose(&msg->fp);
  return 0;
}

static int help_commit_msg(struct Context *ctx, struct Message *msg)
{
  mutt_debug(1, "entering help_commit_msg\n");
  return -1;
}

static int help_open_new_msg(struct Context *ctx, struct Message *msg, struct Header *hdr)
{
  mutt_debug(1, "entering help_open_new_msg\n");
  return -1;
}

static int help_edit_msg_tags(struct Context *ctx, const char *tags, char *buf, size_t buflen)
{
  mutt_debug(1, "entering help_edit_msg_tags\n");
  return -1;
}

static int help_commit_msg_tags(struct Context *msg, struct Header *hdr, char *buf)
{
  mutt_debug(1, "entering help_commit_msg_tags\n");
  return -1;
}

// clang-format off
/**
 * mx_help_ops - Help Mailbox callback functions
 */
struct MxOps mx_help_ops = {
  .mbox_open        = help_open,
  .mbox_open_append = help_open_append,
  .mbox_check       = help_check,
  .mbox_sync        = help_sync,
  .mbox_close       = help_close,
  .msg_open         = help_open_msg,
  .msg_open_new     = help_open_new_msg,
  .msg_commit       = help_commit_msg,
  .msg_close        = help_close_msg,
  .tags_edit        = help_edit_msg_tags,
  .tags_commit      = help_commit_msg_tags,
};
// clang-format on
