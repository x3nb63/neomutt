/**
 * @file
 * Body Caching - local copies of email bodies
 *
 * @authors
 * Copyright (C) 2006-2007,2009,2017 Brendan Cully <brendan@kublai.com>
 * Copyright (C) 2006,2009 Rocco Rutte <pdmef@gmx.net>
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
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "mutt/mutt.h"
#include "bcache.h"
#include "globals.h"
#include "mutt_account.h"
#include "protos.h"
#include "url.h"

/**
 * struct BodyCache - Local cache of email bodies
 */
struct BodyCache
{
  char path[PATH_MAX];
  size_t pathlen;
};

/**
 * bcache_path - Create the cache path for a given account/mailbox
 * @param account Account info
 * @param mailbox Mailbox name
 * @param dst     Buffer for the name
 * @param dstlen  Length of the buffer
 * @retval  0 Success
 * @retval -1 Failure
 */
static int bcache_path(struct Account *account, const char *mailbox, char *dst, size_t dstlen)
{
  char host[STRING];
  struct Url url = { U_UNKNOWN };
  int len;

  if (!account || !MessageCachedir || !*MessageCachedir || !dst || (dstlen == 0))
    return -1;

  /* make up a Url we can turn into a string */
  mutt_account_tourl(account, &url);
  /*
   * mutt_account_tourl() just sets up some pointers;
   * if this ever changes, we have a memleak here
   */
  url.path = NULL;
  if (url_tostring(&url, host, sizeof(host), U_PATH) < 0)
  {
    mutt_debug(1, "URL to string failed\n");
    return -1;
  }

  size_t mailboxlen = mutt_str_strlen(mailbox);
  len = snprintf(dst, dstlen, "%s/%s%s%s", MessageCachedir, host, NONULL(mailbox),
                 (mailboxlen != 0 && mailbox[mailboxlen - 1] == '/') ? "" : "/");

  mutt_encode_path(dst, dstlen, dst);

  mutt_debug(3, "rc: %d, path: '%s'\n", len, dst);

  if (len < 0 || (size_t) len >= dstlen - 1)
    return -1;

  mutt_debug(3, "directory: '%s'\n", dst);

  return 0;
}

/**
 * mutt_bcache_move - Change the id of a message in the cache
 * @param bcache Body cache
 * @param id     Per-mailbox unique identifier for the message
 * @param newid  New id for the message
 */
static int mutt_bcache_move(struct BodyCache *bcache, const char *id, const char *newid)
{
  char path[PATH_MAX];
  char newpath[PATH_MAX];

  if (!bcache || !id || !*id || !newid || !*newid)
    return -1;

  snprintf(path, sizeof(path), "%s%s", bcache->path, id);
  snprintf(newpath, sizeof(newpath), "%s%s", bcache->path, newid);

  mutt_debug(3, "bcache: mv: '%s' '%s'\n", path, newpath);

  return rename(path, newpath);
}

/**
 * mutt_bcache_open - Open an Email-Body Cache
 * @param account current mailbox' account (required)
 * @param mailbox path to the mailbox of the account (optional)
 * @retval NULL Failure
 *
 * The driver using it is responsible for ensuring that hierarchies are
 * separated by '/' (if it knows of such a concepts like mailboxes or
 * hierarchies)
 */
struct BodyCache *mutt_bcache_open(struct Account *account, const char *mailbox)
{
  struct BodyCache *bcache = NULL;

  if (!account)
    goto bail;

  bcache = mutt_mem_calloc(1, sizeof(struct BodyCache));
  if (bcache_path(account, mailbox, bcache->path, sizeof(bcache->path)) < 0)
    goto bail;
  bcache->pathlen = mutt_str_strlen(bcache->path);

  return bcache;

bail:
  if (bcache)
    FREE(&bcache);
  return NULL;
}

/**
 * mutt_bcache_close - Close an Email-Body Cache
 * @param bcache Body cache
 *
 * Free all memory of bcache and finally FREE() it, too.
 */
void mutt_bcache_close(struct BodyCache **bcache)
{
  if (!bcache || !*bcache)
    return;
  FREE(bcache);
}

/**
 * mutt_bcache_get - Open a file in the Body Cache
 * @param bcache Body Cache from mutt_bcache_open()
 * @param id     Per-mailbox unique identifier for the message
 * @retval ptr  Success
 * @retval NULL Failure
 */
FILE *mutt_bcache_get(struct BodyCache *bcache, const char *id)
{
  char path[PATH_MAX];
  FILE *fp = NULL;

  if (!id || !*id || !bcache)
    return NULL;

  path[0] = '\0';
  mutt_str_strncat(path, sizeof(path), bcache->path, bcache->pathlen);
  mutt_str_strncat(path, sizeof(path), id, mutt_str_strlen(id));

  fp = mutt_file_fopen(path, "r");

  mutt_debug(3, "bcache: get: '%s': %s\n", path, fp == NULL ? "no" : "yes");

  return fp;
}

/**
 * mutt_bcache_put - Create a file in the Body Cache
 * @param bcache Body Cache from mutt_bcache_open()
 * @param id     Per-mailbox unique identifier for the message
 * @retval ptr  Success
 * @retval NULL Failure
 *
 * The returned FILE* is in a temporary location.
 * Use mutt_bcache_commit to put it into place
 */
FILE *mutt_bcache_put(struct BodyCache *bcache, const char *id)
{
  char path[PATH_MAX];
  struct stat sb;

  if (!id || !*id || !bcache)
    return NULL;

  if (snprintf(path, sizeof(path), "%s%s%s", bcache->path, id, ".tmp") >= sizeof(path))
  {
    mutt_error(_("Path too long: %s%s%s"), bcache->path, id, ".tmp");
    return NULL;
  }

  if (stat(bcache->path, &sb) == 0)
  {
    if (!S_ISDIR(sb.st_mode))
    {
      mutt_error(_("Message cache isn't a directory: %s."), bcache->path);
      return NULL;
    }
  }
  else
  {
    if (mutt_file_mkdir(bcache->path, S_IRWXU | S_IRWXG | S_IRWXO) < 0)
    {
      mutt_error(_("Can't create %s %s"), bcache->path, strerror(errno));
      return NULL;
    }
  }

  mutt_debug(3, "bcache: put: '%s'\n", path);

  return mutt_file_fopen(path, "w+");
}

/**
 * mutt_bcache_commit - Move a temporary file into the Body Cache
 * @param bcache Body Cache from mutt_bcache_open()
 * @param id     Per-mailbox unique identifier for the message
 * @retval  0 Success
 * @retval -1 Failure
 */
int mutt_bcache_commit(struct BodyCache *bcache, const char *id)
{
  char tmpid[PATH_MAX];

  snprintf(tmpid, sizeof(tmpid), "%s.tmp", id);

  return mutt_bcache_move(bcache, tmpid, id);
}

/**
 * mutt_bcache_del - Delete a file from the Body Cache
 * @param bcache Body Cache from mutt_bcache_open()
 * @param id     Per-mailbox unique identifier for the message
 * @retval  0 Success
 * @retval -1 Failure
 */
int mutt_bcache_del(struct BodyCache *bcache, const char *id)
{
  char path[PATH_MAX];

  if (!id || !*id || !bcache)
    return -1;

  path[0] = '\0';
  mutt_str_strncat(path, sizeof(path), bcache->path, bcache->pathlen);
  mutt_str_strncat(path, sizeof(path), id, mutt_str_strlen(id));

  mutt_debug(3, "bcache: del: '%s'\n", path);

  return unlink(path);
}

/**
 * mutt_bcache_exists - Check if a file exists in the Body Cache
 * @param bcache Body Cache from mutt_bcache_open()
 * @param id     Per-mailbox unique identifier for the message
 * @retval  0 Success
 * @retval -1 Failure
 */
int mutt_bcache_exists(struct BodyCache *bcache, const char *id)
{
  char path[PATH_MAX];
  struct stat st;
  int rc = 0;

  if (!id || !*id || !bcache)
    return -1;

  path[0] = '\0';
  mutt_str_strncat(path, sizeof(path), bcache->path, bcache->pathlen);
  mutt_str_strncat(path, sizeof(path), id, mutt_str_strlen(id));

  if (stat(path, &st) < 0)
    rc = -1;
  else
    rc = S_ISREG(st.st_mode) && st.st_size != 0 ? 0 : -1;

  mutt_debug(3, "bcache: exists: '%s': %s\n", path, rc == 0 ? "yes" : "no");

  return rc;
}

/**
 * mutt_bcache_list - Find matching entries in the Body Cache
 * @param bcache Body Cache from mutt_bcache_open()
 * @param want_id Callback function called for each match
 * @param data    Data to pass to the callback function
 * @retval -1  Failure
 * @retval >=0 count of matching items
 *
 * This more or less "examines" the cache and calls a function with
 * each id it finds if given.
 *
 * The optional callback function gets the id of a message, the very same
 * body cache handle mutt_bcache_list() is called with (to, perhaps,
 * perform further operations on the bcache), and a data cookie which is
 * just passed as-is. If the return value of the callback is non-zero, the
 * listing is aborted and continued otherwise. The callback is optional
 * so that this function can be used to count the items in the cache
 * (see below for return value).
 */
int mutt_bcache_list(struct BodyCache *bcache, bcache_list_t *want_id, void *data)
{
  DIR *d = NULL;
  struct dirent *de = NULL;
  int rc = -1;

  if (!bcache || !(d = opendir(bcache->path)))
    goto out;

  rc = 0;

  mutt_debug(3, "bcache: list: dir: '%s'\n", bcache->path);

  while ((de = readdir(d)))
  {
    if ((mutt_str_strncmp(de->d_name, ".", 1) == 0) ||
        (mutt_str_strncmp(de->d_name, "..", 2) == 0))
    {
      continue;
    }

    mutt_debug(3, "bcache: list: dir: '%s', id :'%s'\n", bcache->path, de->d_name);

    if (want_id && want_id(de->d_name, bcache, data) != 0)
      goto out;

    rc++;
  }

out:
  if (d)
  {
    if (closedir(d) < 0)
      rc = -1;
  }
  mutt_debug(3, "bcache: list: did %d entries\n", rc);
  return rc;
}
