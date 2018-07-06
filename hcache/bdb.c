/**
 * @file
 * Berkeley DB backend for the header cache
 *
 * @authors
 * Copyright (C) 2004 Thomas Glanzmann <sithglan@stud.uni-erlangen.de>
 * Copyright (C) 2004 Tobias Werth <sitowert@stud.uni-erlangen.de>
 * Copyright (C) 2004 Brian Fundakowski Feldman <green@FreeBSD.org>
 * Copyright (C) 2016 Pietro Cerutti <gahr@gahr.ch>
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

/**
 * @page hc_bdb Berkeley DB
 *
 * Use a Berkeley DB file as a header cache backend.
 */

#include "config.h"
#include <db.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include "mutt/mutt.h"
#include "backend.h"
#include "globals.h"

/**
 * struct HcacheDbCtx - Berkeley DB context
 */
struct HcacheDbCtx
{
  DB_ENV *env;
  DB *db;
  int fd;
  char lockfile[PATH_MAX];
};

/**
 * dbt_init - Initialise a BDB context
 * @param dbt  Context to initialise
 * @param data ID string to associate
 * @param len  Length of ID string
 */
static void dbt_init(DBT *dbt, void *data, size_t len)
{
  dbt->data = data;
  dbt->size = dbt->ulen = len;
  dbt->dlen = dbt->doff = 0;
  dbt->flags = DB_DBT_USERMEM;
}

/**
 * dbt_empty_init - Initialise an empty BDB context
 * @param dbt  Context to initialise
 */
static void dbt_empty_init(DBT *dbt)
{
  dbt->data = NULL;
  dbt->size = dbt->ulen = dbt->dlen = dbt->doff = 0;
  dbt->flags = 0;
}

/**
 * hcache_bdb_open - Implements HcacheOps::open()
 */
static void *hcache_bdb_open(const char *path)
{
  struct stat sb;
  int ret;
  u_int32_t createflags = DB_CREATE;
  int pagesize;

  struct HcacheDbCtx *ctx = mutt_mem_malloc(sizeof(struct HcacheDbCtx));

  if (mutt_str_atoi(HeaderCachePagesize, &pagesize) < 0 || pagesize <= 0)
    pagesize = 16384;

  snprintf(ctx->lockfile, sizeof(ctx->lockfile), "%s-lock-hack", path);

  ctx->fd = open(ctx->lockfile, O_WRONLY | O_CREAT, S_IRUSR | S_IWUSR);
  if (ctx->fd < 0)
  {
    FREE(&ctx);
    return NULL;
  }

  if (mutt_file_lock(ctx->fd, 1, 5))
    goto fail_close;

  ret = db_env_create(&ctx->env, 0);
  if (ret)
    goto fail_unlock;

  ret = (*ctx->env->open)(ctx->env, NULL, DB_INIT_MPOOL | DB_CREATE | DB_PRIVATE, 0600);
  if (ret)
    goto fail_env;

  ctx->db = NULL;
  ret = db_create(&ctx->db, ctx->env, 0);
  if (ret)
    goto fail_env;

  if (stat(path, &sb) != 0 && errno == ENOENT)
  {
    createflags |= DB_EXCL;
    ctx->db->set_pagesize(ctx->db, pagesize);
  }

  ret = (*ctx->db->open)(ctx->db, NULL, path, NULL, DB_BTREE, createflags, 0600);
  if (ret)
    goto fail_db;

  return ctx;

fail_db:
  ctx->db->close(ctx->db, 0);
fail_env:
  ctx->env->close(ctx->env, 0);
fail_unlock:
  mutt_file_unlock(ctx->fd);
fail_close:
  close(ctx->fd);
  unlink(ctx->lockfile);
  FREE(&ctx);

  return NULL;
}

/**
 * hcache_bdb_fetch - Implements HcacheOps::fetch()
 */
static void *hcache_bdb_fetch(void *vctx, const char *key, size_t keylen)
{
  DBT dkey;
  DBT data;

  if (!vctx)
    return NULL;

  struct HcacheDbCtx *ctx = vctx;

  dbt_init(&dkey, (void *) key, keylen);
  dbt_empty_init(&data);
  data.flags = DB_DBT_MALLOC;

  ctx->db->get(ctx->db, NULL, &dkey, &data, 0);

  return data.data;
}

/**
 * hcache_bdb_free - Implements HcacheOps::free()
 */
static void hcache_bdb_free(void *vctx, void **data)
{
  FREE(data);
}

/**
 * hcache_bdb_store - Implements HcacheOps::store()
 */
static int hcache_bdb_store(void *vctx, const char *key, size_t keylen, void *data, size_t dlen)
{
  DBT dkey;
  DBT databuf;

  if (!vctx)
    return -1;

  struct HcacheDbCtx *ctx = vctx;

  dbt_init(&dkey, (void *) key, keylen);
  dbt_empty_init(&databuf);
  databuf.flags = DB_DBT_USERMEM;
  databuf.data = data;
  databuf.size = dlen;
  databuf.ulen = dlen;

  return ctx->db->put(ctx->db, NULL, &dkey, &databuf, 0);
}

/**
 * hcache_bdb_delete - Implements HcacheOps::delete()
 */
static int hcache_bdb_delete(void *vctx, const char *key, size_t keylen)
{
  DBT dkey;

  if (!vctx)
    return -1;

  struct HcacheDbCtx *ctx = vctx;

  dbt_init(&dkey, (void *) key, keylen);
  return ctx->db->del(ctx->db, NULL, &dkey, 0);
}

/**
 * hcache_bdb_close - Implements HcacheOps::close()
 */
static void hcache_bdb_close(void **vctx)
{
  if (!vctx || !*vctx)
    return;

  struct HcacheDbCtx *ctx = *vctx;

  ctx->db->close(ctx->db, 0);
  ctx->env->close(ctx->env, 0);
  mutt_file_unlock(ctx->fd);
  close(ctx->fd);
  unlink(ctx->lockfile);
  FREE(vctx);
}

/**
 * hcache_bdb_backend - Implements HcacheOps::backend()
 */
static const char *hcache_bdb_backend(void)
{
  return DB_VERSION_STRING;
}

HCACHE_BACKEND_OPS(bdb)
