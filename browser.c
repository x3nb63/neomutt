/**
 * @file
 * GUI component for displaying/selecting items from a list
 *
 * @authors
 * Copyright (C) 1996-2000,2007,2010,2013 Michael R. Elkins <me@mutt.org>
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
#include <grp.h>
#include <limits.h>
#include <locale.h>
#include <pwd.h>
#include <regex.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "mutt/mutt.h"
#include "conn/conn.h"
#include "mutt.h"
#include "browser.h"
#include "attach.h"
#include "buffy.h"
#include "context.h"
#include "format_flags.h"
#include "globals.h"
#include "keymap.h"
#include "mailbox.h"
#include "mutt_account.h"
#include "mutt_curses.h"
#include "mutt_menu.h"
#include "mutt_window.h"
#include "mx.h"
#include "opcodes.h"
#include "options.h"
#include "protos.h"
#include "sort.h"
#include "url.h"
#ifdef USE_IMAP
#include "imap/imap.h"
#endif
#ifdef USE_NNTP
#include "nntp.h"
#endif
#ifdef USE_NOTMUCH
#include "mutt_notmuch.h"
#endif

static const struct Mapping FolderHelp[] = {
  { N_("Exit"), OP_EXIT },
  { N_("Chdir"), OP_CHANGE_DIRECTORY },
  { N_("Goto"), OP_BROWSER_GOTO_FOLDER },
  { N_("Mask"), OP_ENTER_MASK },
  { N_("Help"), OP_HELP },
  { NULL, 0 },
};

#ifdef USE_NNTP
static struct Mapping FolderNewsHelp[] = {
  { N_("Exit"), OP_EXIT },
  { N_("List"), OP_TOGGLE_MAILBOXES },
  { N_("Subscribe"), OP_BROWSER_SUBSCRIBE },
  { N_("Unsubscribe"), OP_BROWSER_UNSUBSCRIBE },
  { N_("Catchup"), OP_CATCHUP },
  { N_("Mask"), OP_ENTER_MASK },
  { N_("Help"), OP_HELP },
  { NULL, 0 },
};
#endif

/**
 * struct Folder - A folder/dir in the browser
 */
struct Folder
{
  struct FolderFile *ff;
  int num;
};

static char OldLastDir[PATH_MAX] = "";
static char LastDir[PATH_MAX] = "";

/**
 * destroy_state - Free the BrowserState
 *
 * Frees up the memory allocated for the local-global variables.
 */
static void destroy_state(struct BrowserState *state)
{
  for (size_t c = 0; c < state->entrylen; c++)
  {
    FREE(&((state->entry)[c].name));
    FREE(&((state->entry)[c].desc));
  }
#ifdef USE_IMAP
  FREE(&state->folder);
#endif
  FREE(&state->entry);
}

static int browser_compare_subject(const void *a, const void *b)
{
  struct FolderFile *pa = (struct FolderFile *) a;
  struct FolderFile *pb = (struct FolderFile *) b;

  /* inbox should be sorted ahead of its siblings */
  int r = mutt_inbox_cmp(pa->name, pb->name);
  if (r == 0)
    r = mutt_str_strcoll(pa->name, pb->name);
  return ((SortBrowser & SORT_REVERSE) ? -r : r);
}

static int browser_compare_desc(const void *a, const void *b)
{
  struct FolderFile *pa = (struct FolderFile *) a;
  struct FolderFile *pb = (struct FolderFile *) b;

  int r = mutt_str_strcoll(pa->desc, pb->desc);

  return ((SortBrowser & SORT_REVERSE) ? -r : r);
}

static int browser_compare_date(const void *a, const void *b)
{
  struct FolderFile *pa = (struct FolderFile *) a;
  struct FolderFile *pb = (struct FolderFile *) b;

  int r = pa->mtime - pb->mtime;

  return ((SortBrowser & SORT_REVERSE) ? -r : r);
}

static int browser_compare_size(const void *a, const void *b)
{
  struct FolderFile *pa = (struct FolderFile *) a;
  struct FolderFile *pb = (struct FolderFile *) b;

  int r = pa->size - pb->size;

  return ((SortBrowser & SORT_REVERSE) ? -r : r);
}

static int browser_compare_count(const void *a, const void *b)
{
  struct FolderFile *pa = (struct FolderFile *) a;
  struct FolderFile *pb = (struct FolderFile *) b;

  int r = 0;
  if (pa->has_buffy && pb->has_buffy)
    r = pa->msg_count - pb->msg_count;
  else if (pa->has_buffy)
    r = -1;
  else
    r = 1;

  return ((SortBrowser & SORT_REVERSE) ? -r : r);
}

static int browser_compare_count_new(const void *a, const void *b)
{
  struct FolderFile *pa = (struct FolderFile *) a;
  struct FolderFile *pb = (struct FolderFile *) b;

  int r = 0;
  if (pa->has_buffy && pb->has_buffy)
    r = pa->msg_unread - pb->msg_unread;
  else if (pa->has_buffy)
    r = -1;
  else
    r = 1;

  return ((SortBrowser & SORT_REVERSE) ? -r : r);
}

/**
 * browser_compare - Sort the items in the browser
 *
 * Wild compare function that calls the others. It's useful because it provides
 * a way to tell "../" is always on the top of the list, independently of the
 * sort method.
 */
static int browser_compare(const void *a, const void *b)
{
  struct FolderFile *pa = (struct FolderFile *) a;
  struct FolderFile *pb = (struct FolderFile *) b;

  if ((mutt_str_strcoll(pa->desc, "../") == 0) || (mutt_str_strcoll(pa->desc, "..") == 0))
    return -1;
  if ((mutt_str_strcoll(pb->desc, "../") == 0) || (mutt_str_strcoll(pb->desc, "..") == 0))
    return 1;

  switch (SortBrowser & SORT_MASK)
  {
    case SORT_COUNT:
      return browser_compare_count(a, b);
    case SORT_DATE:
      return browser_compare_date(a, b);
    case SORT_DESC:
      return browser_compare_desc(a, b);
    case SORT_SIZE:
      return browser_compare_size(a, b);
    case SORT_UNREAD:
      return browser_compare_count_new(a, b);
    case SORT_SUBJECT:
    default:
      return browser_compare_subject(a, b);
  }
}

/**
 * browser_sort - Sort the entries in the browser
 *
 * Call to qsort using browser_compare function.
 * Some specific sort methods are not used via NNTP.
 */
static void browser_sort(struct BrowserState *state)
{
  switch (SortBrowser & SORT_MASK)
  {
    /* Also called "I don't care"-sort-method. */
    case SORT_ORDER:
      return;
#ifdef USE_NNTP
    case SORT_SIZE:
    case SORT_DATE:
      if (OptNews)
        return;
#endif
    default:
      break;
  }

  qsort(state->entry, state->entrylen, sizeof(struct FolderFile), browser_compare);
}

static int link_is_dir(const char *folder, const char *path)
{
  struct stat st;
  char fullpath[PATH_MAX];

  mutt_file_concat_path(fullpath, folder, path, sizeof(fullpath));

  if (stat(fullpath, &st) == 0)
    return (S_ISDIR(st.st_mode));
  else
    return 0;
}

/**
 * folder_format_str - Format a string for the folder browser
 * @param[out] buf      Buffer in which to save string
 * @param[in]  buflen   Buffer length
 * @param[in]  col      Starting column
 * @param[in]  cols     Number of screen columns
 * @param[in]  op       printf-like operator, e.g. 't'
 * @param[in]  src      printf-like format string
 * @param[in]  prec     Field precision, e.g. "-3.4"
 * @param[in]  if_str   If condition is met, display this string
 * @param[in]  else_str Otherwise, display this string
 * @param[in]  data     Pointer to the mailbox Context
 * @param[in]  flags    Format flags
 * @retval src (unchanged)
 *
 * folder_format_str() is a callback function for mutt_expando_format().
 *
 * | Expando | Description
 * |:--------|:--------------------------------------------------------
 * | \%C     | Current file number
 * | \%d     | Date/time folder was last modified
 * | \%D     | Date/time folder was last modified using $$date_format.
 * | \%F     | File permissions
 * | \%f     | Filename (with suffix '/', '@' or '*')
 * | \%g     | Group name (or numeric gid, if missing)
 * | \%l     | Number of hard links
 * | \%m     | Number of messages in the mailbox *
 * | \%N     | N if mailbox has new mail, blank otherwise
 * | \%n     | Number of unread messages in the mailbox *
 * | \%s     | Size in bytes
 * | \%t     | '*' if the file is tagged, blank otherwise
 * | \%u     | Owner name (or numeric uid, if missing)
 */
static const char *folder_format_str(char *buf, size_t buflen, size_t col, int cols,
                                     char op, const char *src, const char *prec,
                                     const char *if_str, const char *else_str,
                                     unsigned long data, enum FormatFlag flags)
{
  char fn[SHORT_STRING], fmt[SHORT_STRING], permission[11];
  char *t_fmt = NULL;
  struct Folder *folder = (struct Folder *) data;
  struct passwd *pw = NULL;
  struct group *gr = NULL;
  int optional = (flags & MUTT_FORMAT_OPTIONAL);

  switch (op)
  {
    case 'C':
      snprintf(fmt, sizeof(fmt), "%%%sd", prec);
      snprintf(buf, buflen, fmt, folder->num + 1);
      break;

    case 'd':
    case 'D':
      if (folder->ff->local)
      {
        int do_locales = true;

        if (op == 'D')
        {
          t_fmt = NONULL(DateFormat);
          if (*t_fmt == '!')
          {
            t_fmt++;
            do_locales = false;
          }
        }
        else
        {
          time_t tnow = time(NULL);
          t_fmt = tnow - folder->ff->mtime < 31536000 ? "%b %d %H:%M" : "%b %d  %Y";
        }

        if (!do_locales)
          setlocale(LC_TIME, "C");
        char date[SHORT_STRING];
        strftime(date, sizeof(date), t_fmt, localtime(&folder->ff->mtime));
        if (!do_locales)
          setlocale(LC_TIME, "");

        mutt_format_s(buf, buflen, prec, date);
      }
      else
        mutt_format_s(buf, buflen, prec, "");
      break;

    case 'f':
    {
      char *s = NULL;

#ifdef USE_NOTMUCH
      if (mx_is_notmuch(folder->ff->name))
        s = NONULL(folder->ff->desc);
      else
#endif
#ifdef USE_IMAP
          if (folder->ff->imap)
        s = NONULL(folder->ff->desc);
      else
#endif
        s = NONULL(folder->ff->name);

      snprintf(fn, sizeof(fn), "%s%s", s,
               folder->ff->local ?
                   (S_ISLNK(folder->ff->mode) ?
                        "@" :
                        (S_ISDIR(folder->ff->mode) ?
                             "/" :
                             ((folder->ff->mode & S_IXUSR) != 0 ? "*" : ""))) :
                   "");

      mutt_format_s(buf, buflen, prec, fn);
      break;
    }
    case 'F':
      if (folder->ff->local)
      {
        snprintf(permission, sizeof(permission), "%c%c%c%c%c%c%c%c%c%c",
                 S_ISDIR(folder->ff->mode) ? 'd' : (S_ISLNK(folder->ff->mode) ? 'l' : '-'),
                 (folder->ff->mode & S_IRUSR) != 0 ? 'r' : '-',
                 (folder->ff->mode & S_IWUSR) != 0 ? 'w' : '-',
                 (folder->ff->mode & S_ISUID) != 0 ?
                     's' :
                     (folder->ff->mode & S_IXUSR) != 0 ? 'x' : '-',
                 (folder->ff->mode & S_IRGRP) != 0 ? 'r' : '-',
                 (folder->ff->mode & S_IWGRP) != 0 ? 'w' : '-',
                 (folder->ff->mode & S_ISGID) != 0 ?
                     's' :
                     (folder->ff->mode & S_IXGRP) != 0 ? 'x' : '-',
                 (folder->ff->mode & S_IROTH) != 0 ? 'r' : '-',
                 (folder->ff->mode & S_IWOTH) != 0 ? 'w' : '-',
                 (folder->ff->mode & S_ISVTX) != 0 ?
                     't' :
                     (folder->ff->mode & S_IXOTH) != 0 ? 'x' : '-');
        mutt_format_s(buf, buflen, prec, permission);
      }
#ifdef USE_IMAP
      else if (folder->ff->imap)
      {
        /* mark folders with subfolders AND mail */
        snprintf(permission, sizeof(permission), "IMAP %c",
                 (folder->ff->inferiors && folder->ff->selectable) ? '+' : ' ');
        mutt_format_s(buf, buflen, prec, permission);
      }
#endif
      else
        mutt_format_s(buf, buflen, prec, "");
      break;

    case 'g':
      if (folder->ff->local)
      {
        gr = getgrgid(folder->ff->gid);
        if (gr)
          mutt_format_s(buf, buflen, prec, gr->gr_name);
        else
        {
          snprintf(fmt, sizeof(fmt), "%%%sld", prec);
          snprintf(buf, buflen, fmt, folder->ff->gid);
        }
      }
      else
        mutt_format_s(buf, buflen, prec, "");
      break;

    case 'l':
      if (folder->ff->local)
      {
        snprintf(fmt, sizeof(fmt), "%%%sd", prec);
        snprintf(buf, buflen, fmt, folder->ff->nlink);
      }
      else
        mutt_format_s(buf, buflen, prec, "");
      break;

    case 'm':
      if (!optional)
      {
        if (folder->ff->has_buffy)
        {
          snprintf(fmt, sizeof(fmt), "%%%sd", prec);
          snprintf(buf, buflen, fmt, folder->ff->msg_count);
        }
        else
          mutt_format_s(buf, buflen, prec, "");
      }
      else if (!folder->ff->msg_count)
        optional = 0;
      break;

    case 'N':
      snprintf(fmt, sizeof(fmt), "%%%sc", prec);
      snprintf(buf, buflen, fmt, folder->ff->new ? 'N' : ' ');
      break;

    case 'n':
      if (!optional)
      {
        if (folder->ff->has_buffy)
        {
          snprintf(fmt, sizeof(fmt), "%%%sd", prec);
          snprintf(buf, buflen, fmt, folder->ff->msg_unread);
        }
        else
          mutt_format_s(buf, buflen, prec, "");
      }
      else if (!folder->ff->msg_unread)
        optional = 0;
      break;

    case 's':
      if (folder->ff->local)
      {
        mutt_str_pretty_size(fn, sizeof(fn), folder->ff->size);
        snprintf(fmt, sizeof(fmt), "%%%ss", prec);
        snprintf(buf, buflen, fmt, fn);
      }
      else
        mutt_format_s(buf, buflen, prec, "");
      break;

    case 't':
      snprintf(fmt, sizeof(fmt), "%%%sc", prec);
      snprintf(buf, buflen, fmt, folder->ff->tagged ? '*' : ' ');
      break;

    case 'u':
      if (folder->ff->local)
      {
        pw = getpwuid(folder->ff->uid);
        if (pw)
          mutt_format_s(buf, buflen, prec, pw->pw_name);
        else
        {
          snprintf(fmt, sizeof(fmt), "%%%sld", prec);
          snprintf(buf, buflen, fmt, folder->ff->uid);
        }
      }
      else
        mutt_format_s(buf, buflen, prec, "");
      break;

    default:
      snprintf(fmt, sizeof(fmt), "%%%sc", prec);
      snprintf(buf, buflen, fmt, op);
      break;
  }

  if (optional)
    mutt_expando_format(buf, buflen, col, cols, if_str, folder_format_str, data, 0);
  else if (flags & MUTT_FORMAT_OPTIONAL)
    mutt_expando_format(buf, buflen, col, cols, else_str, folder_format_str, data, 0);

  return src;
}

#ifdef USE_NNTP
/**
 * group_index_format_str - Format a string for the newsgroup menu
 * @param[out] buf      Buffer in which to save string
 * @param[in]  buflen   Buffer length
 * @param[in]  col      Starting column
 * @param[in]  cols     Number of screen columns
 * @param[in]  op       printf-like operator, e.g. 't'
 * @param[in]  src      printf-like format string
 * @param[in]  prec     Field precision, e.g. "-3.4"
 * @param[in]  if_str   If condition is met, display this string
 * @param[in]  else_str Otherwise, display this string
 * @param[in]  data     Pointer to the mailbox Context
 * @param[in]  flags    Format flags
 * @retval src (unchanged)
 *
 * group_index_format_str() is a callback function for mutt_expando_format().
 *
 * | Expando | Description
 * |:--------|:--------------------------------------------------------
 * | \%C     | Current newsgroup number
 * | \%d     | Description of newsgroup (becomes from server)
 * | \%f     | Newsgroup name
 * | \%M     | - if newsgroup not allowed for direct post (moderated for example)
 * | \%N     | N if newsgroup is new, u if unsubscribed, blank otherwise
 * | \%n     | Number of new articles in newsgroup
 * | \%s     | Number of unread articles in newsgroup
 */
static const char *group_index_format_str(char *buf, size_t buflen, size_t col, int cols,
                                          char op, const char *src, const char *prec,
                                          const char *if_str, const char *else_str,
                                          unsigned long data, enum FormatFlag flags)
{
  char fn[SHORT_STRING], fmt[SHORT_STRING];
  struct Folder *folder = (struct Folder *) data;

  switch (op)
  {
    case 'C':
      snprintf(fmt, sizeof(fmt), "%%%sd", prec);
      snprintf(buf, buflen, fmt, folder->num + 1);
      break;

    case 'd':
      if (folder->ff->nd->desc)
      {
        char *desc = mutt_str_strdup(folder->ff->nd->desc);
        if (NewsgroupsCharset && *NewsgroupsCharset)
          mutt_ch_convert_string(&desc, NewsgroupsCharset, Charset, MUTT_ICONV_HOOK_FROM);
        mutt_mb_filter_unprintable(&desc);

        snprintf(fmt, sizeof(fmt), "%%%ss", prec);
        snprintf(buf, buflen, fmt, desc);
        FREE(&desc);
      }
      else
      {
        snprintf(fmt, sizeof(fmt), "%%%ss", prec);
        snprintf(buf, buflen, fmt, "");
      }
      break;

    case 'f':
      strncpy(fn, folder->ff->name, sizeof(fn) - 1);
      snprintf(fmt, sizeof(fmt), "%%%ss", prec);
      snprintf(buf, buflen, fmt, fn);
      break;

    case 'M':
      snprintf(fmt, sizeof(fmt), "%%%sc", prec);
      if (folder->ff->nd->deleted)
        snprintf(buf, buflen, fmt, 'D');
      else
        snprintf(buf, buflen, fmt, folder->ff->nd->allowed ? ' ' : '-');
      break;

    case 'N':
      snprintf(fmt, sizeof(fmt), "%%%sc", prec);
      if (folder->ff->nd->subscribed)
        snprintf(buf, buflen, fmt, ' ');
      else
        snprintf(buf, buflen, fmt, folder->ff->new ? 'N' : 'u');
      break;

    case 'n':
      if (Context && Context->data == folder->ff->nd)
      {
        snprintf(fmt, sizeof(fmt), "%%%sd", prec);
        snprintf(buf, buflen, fmt, Context->new);
      }
      else if (MarkOld && folder->ff->nd->last_cached >= folder->ff->nd->first_message &&
               folder->ff->nd->last_cached <= folder->ff->nd->last_message)
      {
        snprintf(fmt, sizeof(fmt), "%%%sd", prec);
        snprintf(buf, buflen, fmt, folder->ff->nd->last_message - folder->ff->nd->last_cached);
      }
      else
      {
        snprintf(fmt, sizeof(fmt), "%%%sd", prec);
        snprintf(buf, buflen, fmt, folder->ff->nd->unread);
      }
      break;

    case 's':
      if (flags & MUTT_FORMAT_OPTIONAL)
      {
        if (folder->ff->nd->unread != 0)
        {
          mutt_expando_format(buf, buflen, col, cols, if_str,
                              group_index_format_str, data, flags);
        }
        else
        {
          mutt_expando_format(buf, buflen, col, cols, else_str,
                              group_index_format_str, data, flags);
        }
      }
      else if (Context && Context->data == folder->ff->nd)
      {
        snprintf(fmt, sizeof(fmt), "%%%sd", prec);
        snprintf(buf, buflen, fmt, Context->unread);
      }
      else
      {
        snprintf(fmt, sizeof(fmt), "%%%sd", prec);
        snprintf(buf, buflen, fmt, folder->ff->nd->unread);
      }
      break;
  }
  return src;
}
#endif /* USE_NNTP */

static void add_folder(struct Menu *m, struct BrowserState *state, const char *name,
                       const char *desc, const struct stat *s, struct Buffy *b, void *data)
{
  if (state->entrylen == state->entrymax)
  {
    /* need to allocate more space */
    mutt_mem_realloc(&state->entry, sizeof(struct FolderFile) * (state->entrymax += 256));
    memset(&state->entry[state->entrylen], 0, sizeof(struct FolderFile) * 256);
    if (m)
      m->data = state->entry;
  }

  if (s)
  {
    (state->entry)[state->entrylen].mode = s->st_mode;
    (state->entry)[state->entrylen].mtime = s->st_mtime;
    (state->entry)[state->entrylen].size = s->st_size;
    (state->entry)[state->entrylen].gid = s->st_gid;
    (state->entry)[state->entrylen].uid = s->st_uid;
    (state->entry)[state->entrylen].nlink = s->st_nlink;

    (state->entry)[state->entrylen].local = true;
  }
  else
    (state->entry)[state->entrylen].local = false;

  if (b)
  {
    (state->entry)[state->entrylen].has_buffy = true;
    (state->entry)[state->entrylen].new = b->new;
    (state->entry)[state->entrylen].msg_count = b->msg_count;
    (state->entry)[state->entrylen].msg_unread = b->msg_unread;
  }

  (state->entry)[state->entrylen].name = mutt_str_strdup(name);
  (state->entry)[state->entrylen].desc = mutt_str_strdup(desc ? desc : name);
#ifdef USE_IMAP
  (state->entry)[state->entrylen].imap = false;
#endif
#ifdef USE_NNTP
  if (OptNews)
    (state->entry)[state->entrylen].nd = (struct NntpData *) data;
#endif
  (state->entrylen)++;
}

static void init_state(struct BrowserState *state, struct Menu *menu)
{
  state->entrylen = 0;
  state->entrymax = 256;
  state->entry = mutt_mem_calloc(state->entrymax, sizeof(struct FolderFile));
#ifdef USE_IMAP
  state->imap_browse = false;
#endif
  if (menu)
    menu->data = state->entry;
}

/**
 * examine_directory - get list of all files/newsgroups with mask
 */
static int examine_directory(struct Menu *menu, struct BrowserState *state,
                             char *d, const char *prefix)
{
#ifdef USE_NNTP
  if (OptNews)
  {
    struct NntpServer *nserv = CurrentNewsSrv;

    init_state(state, menu);

    for (unsigned int i = 0; i < nserv->groups_num; i++)
    {
      struct NntpData *nntp_data = nserv->groups_list[i];
      if (!nntp_data)
        continue;
      if (prefix && *prefix && (strncmp(prefix, nntp_data->group, strlen(prefix)) != 0))
        continue;
      if (Mask && Mask->regex &&
          !((regexec(Mask->regex, nntp_data->group, 0, NULL, 0) == 0) ^ Mask->not))
      {
        continue;
      }
      add_folder(menu, state, nntp_data->group, NULL, NULL, NULL, nntp_data);
    }
  }
  else
#endif /* USE_NNTP */
  {
    struct stat s;
    DIR *dp = NULL;
    struct dirent *de = NULL;
    char buffer[PATH_MAX + SHORT_STRING];
    struct Buffy *tmp = NULL;

    while (stat(d, &s) == -1)
    {
      if (errno == ENOENT)
      {
        /* The last used directory is deleted, try to use the parent dir. */
        char *c = strrchr(d, '/');

        if (c && (c > d))
        {
          *c = 0;
          continue;
        }
      }
      mutt_perror(d);
      return -1;
    }

    if (!S_ISDIR(s.st_mode))
    {
      mutt_error(_("%s is not a directory."), d);
      return -1;
    }

    mutt_buffy_check(0);

    dp = opendir(d);
    if (!dp)
    {
      mutt_perror(d);
      return -1;
    }

    init_state(state, menu);

    while ((de = readdir(dp)) != NULL)
    {
      if (mutt_str_strcmp(de->d_name, ".") == 0)
        continue; /* we don't need . */

      if (prefix && *prefix &&
          (mutt_str_strncmp(prefix, de->d_name, mutt_str_strlen(prefix)) != 0))
      {
        continue;
      }
      if (Mask && Mask->regex &&
          !((regexec(Mask->regex, de->d_name, 0, NULL, 0) == 0) ^ Mask->not))
      {
        continue;
      }

      mutt_file_concat_path(buffer, d, de->d_name, sizeof(buffer));
      if (lstat(buffer, &s) == -1)
        continue;

      /* No size for directories or symlinks */
      if (S_ISDIR(s.st_mode) || S_ISLNK(s.st_mode))
        s.st_size = 0;
      else if (!S_ISREG(s.st_mode))
        continue;

      tmp = Incoming;
      while (tmp && (mutt_str_strcmp(buffer, tmp->path) != 0))
        tmp = tmp->next;
      if (tmp && Context && (mutt_str_strcmp(tmp->realpath, Context->realpath) == 0))
      {
        tmp->msg_count = Context->msgcount;
        tmp->msg_unread = Context->unread;
      }
      add_folder(menu, state, de->d_name, NULL, &s, tmp, NULL);
    }
    closedir(dp);
  }
  browser_sort(state);
  return 0;
}

#ifdef USE_NOTMUCH
static int examine_vfolders(struct Menu *menu, struct BrowserState *state)
{
  struct Buffy *tmp = Incoming;
  if (!tmp)
    return -1;

  mutt_buffy_check(0);

  init_state(state, menu);

  do
  {
    if (mx_is_notmuch(tmp->path))
    {
      nm_nonctx_get_count(tmp->path, &tmp->msg_count, &tmp->msg_unread);
      add_folder(menu, state, tmp->path, tmp->desc, NULL, tmp, NULL);
      continue;
    }
  } while ((tmp = tmp->next));
  browser_sort(state);
  return 0;
}
#endif

/**
 * examine_mailboxes - Get list of mailboxes/subscribed newsgroups
 */
static int examine_mailboxes(struct Menu *menu, struct BrowserState *state)
{
  struct stat s;

#ifdef USE_NNTP
  if (OptNews)
  {
    struct NntpServer *nserv = CurrentNewsSrv;

    init_state(state, menu);

    for (unsigned int i = 0; i < nserv->groups_num; i++)
    {
      struct NntpData *nntp_data = nserv->groups_list[i];
      if (nntp_data && (nntp_data->new || (nntp_data->subscribed &&
                                           (nntp_data->unread || !ShowOnlyUnread))))
      {
        add_folder(menu, state, nntp_data->group, NULL, NULL, NULL, nntp_data);
      }
    }
  }
  else
#endif
  {
    struct Buffy *tmp = Incoming;

    init_state(state, menu);

    if (!Incoming)
      return -1;
    mutt_buffy_check(0);

    do
    {
      if (Context && (mutt_str_strcmp(tmp->realpath, Context->realpath) == 0))
      {
        tmp->msg_count = Context->msgcount;
        tmp->msg_unread = Context->unread;
      }

      char buffer[PATH_MAX];
      mutt_str_strfcpy(buffer, tmp->path, sizeof(buffer));
      if (BrowserAbbreviateMailboxes)
        mutt_pretty_mailbox(buffer, sizeof(buffer));

#ifdef USE_IMAP
      if (mx_is_imap(tmp->path))
      {
        add_folder(menu, state, buffer, NULL, NULL, tmp, NULL);
        continue;
      }
#endif
#ifdef USE_POP
      if (mx_is_pop(tmp->path))
      {
        add_folder(menu, state, buffer, NULL, NULL, tmp, NULL);
        continue;
      }
#endif
#ifdef USE_NNTP
      if (mx_is_nntp(tmp->path))
      {
        add_folder(menu, state, tmp->path, NULL, NULL, tmp, NULL);
        continue;
      }
#endif
      if (lstat(tmp->path, &s) == -1)
        continue;

      if ((!S_ISREG(s.st_mode)) && (!S_ISDIR(s.st_mode)) && (!S_ISLNK(s.st_mode)))
        continue;

      if (mx_is_maildir(tmp->path))
      {
        struct stat st2;
        char md[PATH_MAX];

        snprintf(md, sizeof(md), "%s/new", tmp->path);
        if (stat(md, &s) < 0)
          s.st_mtime = 0;
        snprintf(md, sizeof(md), "%s/cur", tmp->path);
        if (stat(md, &st2) < 0)
          st2.st_mtime = 0;
        if (st2.st_mtime > s.st_mtime)
          s.st_mtime = st2.st_mtime;
      }

      add_folder(menu, state, buffer, NULL, &s, tmp, NULL);
    } while ((tmp = tmp->next));
  }
  browser_sort(state);
  return 0;
}

static int select_file_search(struct Menu *menu, regex_t *re, int n)
{
#ifdef USE_NNTP
  if (OptNews)
    return (regexec(re, ((struct FolderFile *) menu->data)[n].desc, 0, NULL, 0));
#endif
  return (regexec(re, ((struct FolderFile *) menu->data)[n].name, 0, NULL, 0));
}

#ifdef USE_NOTMUCH
static int select_vfolder_search(struct Menu *menu, regex_t *re, int n)
{
  return (regexec(re, ((struct FolderFile *) menu->data)[n].desc, 0, NULL, 0));
}
#endif

/**
 * folder_entry - Format a menu item for the folder browser
 * @param[out] buf    Buffer in which to save string
 * @param[in]  buflen Buffer length
 * @param[in]  menu   Menu containing aliases
 * @param[in]  num    Index into the menu
 */
static void folder_entry(char *buf, size_t buflen, struct Menu *menu, int num)
{
  struct Folder folder;

  folder.ff = &((struct FolderFile *) menu->data)[num];
  folder.num = num;

#ifdef USE_NNTP
  if (OptNews)
  {
    mutt_expando_format(buf, buflen, 0, MuttIndexWindow->cols,
                        NONULL(GroupIndexFormat), group_index_format_str,
                        (unsigned long) &folder, MUTT_FORMAT_ARROWCURSOR);
  }
  else
#endif
  {
    mutt_expando_format(buf, buflen, 0, MuttIndexWindow->cols,
                        NONULL(FolderFormat), folder_format_str,
                        (unsigned long) &folder, MUTT_FORMAT_ARROWCURSOR);
  }
}

#ifdef USE_NOTMUCH
/**
 * vfolder_entry - Format a menu item for the virtual folder list
 * @param[out] buf    Buffer in which to save string
 * @param[in]  buflen Buffer length
 * @param[in]  menu   Menu containing aliases
 * @param[in]  num    Index into the menu
 */
static void vfolder_entry(char *buf, size_t buflen, struct Menu *menu, int num)
{
  struct Folder folder;

  folder.ff = &((struct FolderFile *) menu->data)[num];
  folder.num = num;

  mutt_expando_format(buf, buflen, 0, MuttIndexWindow->cols, NONULL(VfolderFormat),
                      folder_format_str, (unsigned long) &folder, MUTT_FORMAT_ARROWCURSOR);
}
#endif

/**
 * browser_highlight_default - Decide which browser item should be highlighted
 *
 * This function takes a menu and a state and defines the current entry that
 * should be highlighted.
 */
static void browser_highlight_default(struct BrowserState *state, struct Menu *menu)
{
  menu->top = 0;
  /* Reset menu position to 1.
   * We do not risk overflow as the init_menu function changes
   * current if it is bigger than state->entrylen.
   */
  if ((mutt_str_strcmp(state->entry[0].desc, "..") == 0) ||
      (mutt_str_strcmp(state->entry[0].desc, "../") == 0))
  {
    /* Skip the first entry, unless there's only one entry. */
    menu->current = (menu->max > 1);
  }
  else
  {
    menu->current = 0;
  }
}

static void init_menu(struct BrowserState *state, struct Menu *menu,
                      char *title, size_t titlelen, bool buffy)
{
  menu->max = state->entrylen;

  if (menu->current >= menu->max)
    menu->current = menu->max - 1;
  if (menu->current < 0)
    menu->current = 0;
  if (menu->top > menu->current)
    menu->top = 0;

  menu->tagged = 0;

#ifdef USE_NNTP
  if (OptNews)
  {
    if (buffy)
      snprintf(title, titlelen, _("Subscribed newsgroups"));
    else
    {
      snprintf(title, titlelen, _("Newsgroups on server [%s]"),
               CurrentNewsSrv->conn->account.host);
    }
  }
  else
#endif
  {
    if (buffy)
    {
      menu->is_mailbox_list = 1;
      snprintf(title, titlelen, _("Mailboxes [%d]"), mutt_buffy_check(0));
    }
    else
    {
      char path[PATH_MAX];
      menu->is_mailbox_list = 0;
      mutt_str_strfcpy(path, LastDir, sizeof(path));
      mutt_pretty_mailbox(path, sizeof(path));
      snprintf(title, titlelen, _("Directory [%s], File mask: %s"), path,
               NONULL(Mask ? Mask->pattern : NULL));
    }
  }

  /* Browser tracking feature.
   * The goal is to highlight the good directory if LastDir is the parent dir
   * of OldLastDir (this occurs mostly when one hit "../"). It should also work
   * properly when the user is in examine_mailboxes-mode.
   */
  int ldlen = mutt_str_strlen(LastDir);
  if ((ldlen > 0) && (mutt_str_strncmp(LastDir, OldLastDir, ldlen) == 0))
  {
    char TargetDir[PATH_MAX] = "";

#ifdef USE_IMAP
    /* Use mx_is_imap to check what kind of dir is OldLastDir.
     */
    if (mx_is_imap(OldLastDir))
    {
      mutt_str_strfcpy(TargetDir, OldLastDir, sizeof(TargetDir));
      imap_clean_path(TargetDir, sizeof(TargetDir));
    }
    else
#endif
      mutt_str_strfcpy(TargetDir, strrchr(OldLastDir, '/') + 1, sizeof(TargetDir));

    /* If we get here, it means that LastDir is the parent directory of
     * OldLastDir.  I.e., we're returning from a subdirectory, and we want
     * to position the cursor on the directory we're returning from. */
    bool matched = false;
    for (unsigned int i = 0; i < state->entrylen; i++)
    {
      if (mutt_str_strcmp(state->entry[i].name, TargetDir) == 0)
      {
        menu->current = i;
        matched = true;
        break;
      }
    }
    if (!matched)
      browser_highlight_default(state, menu);
  }
  else
    browser_highlight_default(state, menu);

  menu->redraw = REDRAW_FULL;
}

static int file_tag(struct Menu *menu, int n, int m)
{
  struct FolderFile *ff = &(((struct FolderFile *) menu->data)[n]);
  if (S_ISDIR(ff->mode) || (S_ISLNK(ff->mode) && link_is_dir(LastDir, ff->name)))
  {
    mutt_error(_("Can't attach a directory!"));
    return 0;
  }

  bool ot = ff->tagged;
  ff->tagged = (m >= 0 ? m : !ff->tagged);

  return (ff->tagged - ot);
}

/**
 * mutt_browser_select_dir - Remember the last directory selected
 *
 * This function helps the browser to know which directory has been selected.
 * It should be called anywhere a confirm hit is done to open a new
 * directory/file which is a maildir/mbox.
 *
 * We could check if the sort method is appropriate with this feature.
 */
void mutt_browser_select_dir(char *f)
{
  mutt_str_strfcpy(OldLastDir, f, sizeof(OldLastDir));

  /* Method that will fetch the parent path depending on the
     type of the path. */
  mutt_get_parent_path(LastDir, OldLastDir, sizeof(LastDir));
}

void mutt_select_file(char *f, size_t flen, int flags, char ***files, int *numfiles)
{
  char buf[PATH_MAX];
  char prefix[PATH_MAX] = "";
  char helpstr[LONG_STRING];
  char title[STRING];
  struct BrowserState state = { 0 };
  struct Menu *menu = NULL;
  struct stat st;
  int i, kill_prefix = 0;
  int multiple = (flags & MUTT_SEL_MULTI) ? 1 : 0;
  int folder = (flags & MUTT_SEL_FOLDER) ? 1 : 0;
  int buffy = (flags & MUTT_SEL_BUFFY) ? 1 : 0;

  /* Keeps in memory the directory we were in when hitting '='
   * to go directly to $folder (Folder)
   */
  char GotoSwapper[PATH_MAX] = "";

  buffy = buffy && folder;

#ifdef USE_NNTP
  if (OptNews)
  {
    if (*f)
      mutt_str_strfcpy(prefix, f, sizeof(prefix));
    else
    {
      struct NntpServer *nserv = CurrentNewsSrv;

      /* default state for news reader mode is browse subscribed newsgroups */
      buffy = 0;
      for (unsigned int j = 0; j < nserv->groups_num; j++)
      {
        struct NntpData *nntp_data = nserv->groups_list[j];
        if (nntp_data && nntp_data->subscribed)
        {
          buffy = 1;
          break;
        }
      }
    }
  }
  else
#endif
      if (*f)
  {
    mutt_expand_path(f, flen);
#ifdef USE_IMAP
    if (mx_is_imap(f))
    {
      init_state(&state, NULL);
      state.imap_browse = true;
      if (imap_browse(f, &state) == 0)
      {
        mutt_str_strfcpy(LastDir, state.folder, sizeof(LastDir));
        browser_sort(&state);
      }
    }
    else
    {
#endif
      for (i = mutt_str_strlen(f) - 1; i > 0 && f[i] != '/'; i--)
        ;
      if (i > 0)
      {
        if (f[0] == '/')
        {
          if (i > sizeof(LastDir) - 1)
            i = sizeof(LastDir) - 1;
          strncpy(LastDir, f, i);
          LastDir[i] = 0;
        }
        else
        {
          getcwd(LastDir, sizeof(LastDir));
          mutt_str_strcat(LastDir, sizeof(LastDir), "/");
          mutt_str_strncat(LastDir, sizeof(LastDir), f, i);
        }
      }
      else
      {
        if (f[0] == '/')
          strcpy(LastDir, "/");
        else
          getcwd(LastDir, sizeof(LastDir));
      }

      if (i <= 0 && f[0] != '/')
        mutt_str_strfcpy(prefix, f, sizeof(prefix));
      else
        mutt_str_strfcpy(prefix, f + i + 1, sizeof(prefix));
      kill_prefix = 1;
#ifdef USE_IMAP
    }
#endif
  }
#ifdef USE_NOTMUCH
  else if (!(flags & MUTT_SEL_VFOLDER))
#else
  else
#endif
  {
    if (!folder)
      getcwd(LastDir, sizeof(LastDir));
    else
    {
      /* Whether we use the tracking feature of the browser depends
       * on which sort method we chose to use. This variable is defined
       * only to help readability of the code.
       */
      short browser_track;

      switch (SortBrowser & SORT_MASK)
      {
        case SORT_DESC:
        case SORT_SUBJECT:
        case SORT_ORDER:
          browser_track = 1;
          break;

        default:
          browser_track = 0;
          break;
      }

      /* We use mutt_browser_select_dir to initialize the two
       * variables (LastDir, OldLastDir) at the appropriate
       * values.
       *
       * We do it only when LastDir is not set (first pass there)
       * or when CurrentFolder and OldLastDir are not the same.
       * This code is executed only when we list files, not when
       * we press up/down keys to navigate in a displayed list.
       *
       * We only do this when CurrentFolder has been set (ie, not
       * when listing folders on startup with "neomutt -y").
       *
       * This tracker is only used when browser_track is true,
       * meaning only with sort methods SUBJECT/DESC for now.
       */
      if (CurrentFolder)
      {
        if (!LastDir[0])
        {
          /* If browsing in "local"-mode, than we chose to define LastDir to
           * MailDir
           */
          switch (mx_get_magic(CurrentFolder))
          {
            case MUTT_IMAP:
            case MUTT_MAILDIR:
            case MUTT_MBOX:
            case MUTT_MH:
            case MUTT_MMDF:
              if (Folder)
                mutt_str_strfcpy(LastDir, NONULL(Folder), sizeof(LastDir));
              else if (Spoolfile)
                mutt_browser_select_dir(Spoolfile);
              break;
            default:
              mutt_browser_select_dir(CurrentFolder);
              break;
          }
        }
        else if (mutt_str_strcmp(CurrentFolder, OldLastDir) != 0)
        {
          mutt_browser_select_dir(CurrentFolder);
        }
      }

      /* When browser tracking feature is disabled, shoot a 0
       * on first char of OldLastDir to make it useless.
       */
      if (!browser_track)
        OldLastDir[0] = '\0';
    }

#ifdef USE_IMAP
    if (!buffy && mx_is_imap(LastDir))
    {
      init_state(&state, NULL);
      state.imap_browse = true;
      imap_browse(LastDir, &state);
      browser_sort(&state);
    }
    else
#endif
    {
      i = mutt_str_strlen(LastDir);
      while (i && LastDir[--i] == '/')
        LastDir[i] = '\0';
      if (!LastDir[0])
        getcwd(LastDir, sizeof(LastDir));
    }
  }

  *f = 0;

#ifdef USE_NOTMUCH
  if (flags & MUTT_SEL_VFOLDER)
  {
    if (examine_vfolders(NULL, &state) == -1)
      goto bail;
  }
  else
#endif
      if (buffy)
  {
    examine_mailboxes(NULL, &state);
  }
  else
#ifdef USE_IMAP
      if (!state.imap_browse)
#endif
  {
    if (examine_directory(NULL, &state, LastDir, prefix) == -1)
      goto bail;
  }
  menu = mutt_menu_new(MENU_FOLDER);
  menu->make_entry = folder_entry;
  menu->search = select_file_search;
  menu->title = title;
  menu->data = state.entry;
  if (multiple)
    menu->tag = file_tag;

#ifdef USE_NOTMUCH
  if (flags & MUTT_SEL_VFOLDER)
  {
    menu->make_entry = vfolder_entry;
    menu->search = select_vfolder_search;
  }
  else
#endif
    menu->make_entry = folder_entry;

  menu->help = mutt_compile_help(helpstr, sizeof(helpstr), MENU_FOLDER,
#ifdef USE_NNTP
                                 OptNews ? FolderNewsHelp :
#endif
                                           FolderHelp);
  mutt_menu_push_current(menu);

  init_menu(&state, menu, title, sizeof(title), buffy);

  while (true)
  {
    switch (i = mutt_menu_loop(menu))
    {
      case OP_GENERIC_SELECT_ENTRY:

        if (state.entrylen == 0)
        {
          mutt_error(_("No files match the file mask"));
          break;
        }

        if (S_ISDIR(state.entry[menu->current].mode) ||
            (S_ISLNK(state.entry[menu->current].mode) &&
             link_is_dir(LastDir, state.entry[menu->current].name))
#ifdef USE_IMAP
            || state.entry[menu->current].inferiors
#endif
        )
        {
          /* make sure this isn't a MH or maildir mailbox */
          if (buffy)
          {
            mutt_str_strfcpy(buf, state.entry[menu->current].name, sizeof(buf));
            mutt_expand_path(buf, sizeof(buf));
          }
#ifdef USE_IMAP
          else if (state.imap_browse)
          {
            mutt_str_strfcpy(buf, state.entry[menu->current].name, sizeof(buf));
          }
#endif
          else
          {
            mutt_file_concat_path(buf, LastDir, state.entry[menu->current].name,
                                  sizeof(buf));
          }

          if ((mx_get_magic(buf) <= 0)
#ifdef USE_IMAP
              || state.entry[menu->current].inferiors
#endif
          )
          {
            /* save the old directory */
            mutt_str_strfcpy(OldLastDir, LastDir, sizeof(OldLastDir));

            if (mutt_str_strcmp(state.entry[menu->current].name, "..") == 0)
            {
              if (mutt_str_strcmp("..", LastDir + mutt_str_strlen(LastDir) - 2) == 0)
                strcat(LastDir, "/..");
              else
              {
                char *p = strrchr(LastDir + 1, '/');

                if (p)
                  *p = 0;
                else
                {
                  if (LastDir[0] == '/')
                    LastDir[1] = 0;
                  else
                    strcat(LastDir, "/..");
                }
              }
            }
            else if (buffy)
            {
              mutt_str_strfcpy(LastDir, state.entry[menu->current].name, sizeof(LastDir));
              mutt_expand_path(LastDir, sizeof(LastDir));
            }
#ifdef USE_IMAP
            else if (state.imap_browse)
            {
              int n;
              struct Url url;

              mutt_str_strfcpy(LastDir, state.entry[menu->current].name, sizeof(LastDir));
              /* tack on delimiter here */
              n = strlen(LastDir) + 1;

              /* special case "" needs no delimiter */
              url_parse(&url, state.entry[menu->current].name);
              if (url.path && (state.entry[menu->current].delim != '\0') &&
                  (n < sizeof(LastDir)))
              {
                LastDir[n] = '\0';
                LastDir[n - 1] = state.entry[menu->current].delim;
              }
              url_free(&url);
            }
#endif
            else
            {
              char tmp[PATH_MAX];
              mutt_file_concat_path(tmp, LastDir,
                                    state.entry[menu->current].name, sizeof(tmp));
              mutt_str_strfcpy(LastDir, tmp, sizeof(LastDir));
            }

            destroy_state(&state);
            if (kill_prefix)
            {
              prefix[0] = 0;
              kill_prefix = 0;
            }
            buffy = 0;
#ifdef USE_IMAP
            if (state.imap_browse)
            {
              init_state(&state, NULL);
              state.imap_browse = true;
              imap_browse(LastDir, &state);
              browser_sort(&state);
              menu->data = state.entry;
            }
            else
#endif
            {
              if (examine_directory(menu, &state, LastDir, prefix) == -1)
              {
                /* try to restore the old values */
                mutt_str_strfcpy(LastDir, OldLastDir, sizeof(LastDir));
                if (examine_directory(menu, &state, LastDir, prefix) == -1)
                {
                  mutt_str_strfcpy(LastDir, NONULL(HomeDir), sizeof(LastDir));
                  goto bail;
                }
              }
              /* resolve paths navigated from GUI */
              if (mutt_realpath(LastDir) == 0)
                break;
            }

            browser_highlight_default(&state, menu);
            init_menu(&state, menu, title, sizeof(title), buffy);
            if (GotoSwapper[0])
              GotoSwapper[0] = '\0';
            break;
          }
        }

        if (buffy || OptNews) /* USE_NNTP */
        {
          mutt_str_strfcpy(f, state.entry[menu->current].name, flen);
          mutt_expand_path(f, flen);
        }
#ifdef USE_IMAP
        else if (state.imap_browse)
          mutt_str_strfcpy(f, state.entry[menu->current].name, flen);
#endif
#ifdef USE_NOTMUCH
        else if (mx_is_notmuch(state.entry[menu->current].name))
          mutt_str_strfcpy(f, state.entry[menu->current].name, flen);
#endif
        else
          mutt_file_concat_path(f, LastDir, state.entry[menu->current].name, flen);
        /* fallthrough */

      case OP_EXIT:

        if (multiple)
        {
          char **tfiles = NULL;

          if (menu->tagged)
          {
            *numfiles = menu->tagged;
            tfiles = mutt_mem_calloc(*numfiles, sizeof(char *));
            for (int j = 0, k = 0; j < state.entrylen; j++)
            {
              struct FolderFile ff = state.entry[j];
              if (ff.tagged)
              {
                char full[PATH_MAX];
                mutt_file_concat_path(full, LastDir, ff.name, sizeof(full));
                mutt_expand_path(full, sizeof(full));
                tfiles[k++] = mutt_str_strdup(full);
              }
            }
            *files = tfiles;
          }
          else if (f[0]) /* no tagged entries. return selected entry */
          {
            *numfiles = 1;
            tfiles = mutt_mem_calloc(*numfiles, sizeof(char *));
            mutt_expand_path(f, flen);
            tfiles[0] = mutt_str_strdup(f);
            *files = tfiles;
          }
        }

        destroy_state(&state);
        goto bail;

      case OP_BROWSER_TELL:
        if (state.entrylen)
          mutt_message("%s", state.entry[menu->current].name);
        break;

#ifdef USE_IMAP
      case OP_BROWSER_TOGGLE_LSUB:
        ImapListSubscribed = !ImapListSubscribed;

        mutt_unget_event(0, OP_CHECK_NEW);
        break;

      case OP_CREATE_MAILBOX:
        if (!state.imap_browse)
        {
          mutt_error(_("Create is only supported for IMAP mailboxes"));
          break;
        }

        if (imap_mailbox_create(LastDir) == 0)
        {
          /* TODO: find a way to detect if the new folder would appear in
           *   this window, and insert it without starting over. */
          destroy_state(&state);
          init_state(&state, NULL);
          state.imap_browse = true;
          imap_browse(LastDir, &state);
          browser_sort(&state);
          menu->data = state.entry;
          browser_highlight_default(&state, menu);
          init_menu(&state, menu, title, sizeof(title), buffy);
        }
        /* else leave error on screen */
        break;

      case OP_RENAME_MAILBOX:
        if (!state.entry[menu->current].imap)
          mutt_error(_("Rename is only supported for IMAP mailboxes"));
        else
        {
          int nentry = menu->current;

          if (imap_mailbox_rename(state.entry[nentry].name) >= 0)
          {
            destroy_state(&state);
            init_state(&state, NULL);
            state.imap_browse = true;
            imap_browse(LastDir, &state);
            browser_sort(&state);
            menu->data = state.entry;
            browser_highlight_default(&state, menu);
            init_menu(&state, menu, title, sizeof(title), buffy);
          }
        }
        break;

      case OP_DELETE_MAILBOX:
        if (!state.entry[menu->current].imap)
          mutt_error(_("Delete is only supported for IMAP mailboxes"));
        else
        {
          char msg[SHORT_STRING];
          struct ImapMbox mx;
          int nentry = menu->current;

          if (imap_parse_path(state.entry[nentry].name, &mx) < 0)
          {
            mutt_debug(1, "imap_parse_path failed\n");
            mutt_error(_("Mailbox deletion failed."));
            break;
          }
          if (!mx.mbox)
          {
            mutt_error(_("Cannot delete root folder"));
            break;
          }
          snprintf(msg, sizeof(msg), _("Really delete mailbox \"%s\"?"), mx.mbox);
          if (mutt_yesorno(msg, MUTT_NO) == MUTT_YES)
          {
            if (imap_delete_mailbox(Context, &mx) == 0)
            {
              /* free the mailbox from the browser */
              FREE(&((state.entry)[nentry].name));
              FREE(&((state.entry)[nentry].desc));
              /* and move all other entries up */
              if (nentry + 1 < state.entrylen)
              {
                memmove(state.entry + nentry, state.entry + nentry + 1,
                        sizeof(struct FolderFile) * (state.entrylen - (nentry + 1)));
              }
              memset(&state.entry[state.entrylen - 1], 0, sizeof(struct FolderFile));
              state.entrylen--;
              mutt_message(_("Mailbox deleted."));
              init_menu(&state, menu, title, sizeof(title), buffy);
            }
            else
              mutt_error(_("Mailbox deletion failed."));
          }
          else
            mutt_message(_("Mailbox not deleted."));
          FREE(&mx.mbox);
        }
        break;
#endif

      case OP_GOTO_PARENT:
      case OP_CHANGE_DIRECTORY:

#ifdef USE_NNTP
        if (OptNews)
          break;
#endif

        mutt_str_strfcpy(buf, LastDir, sizeof(buf));
#ifdef USE_IMAP
        if (!state.imap_browse)
#endif
        {
          /* add '/' at the end of the directory name if not already there */
          size_t len = mutt_str_strlen(buf);
          if ((len > 0) && (buf[len - 1] != '/') && (sizeof(buf) > (len + 1)))
          {
            buf[len] = '/';
            buf[len + 1] = '\0';
          }
        }

        if (i == OP_CHANGE_DIRECTORY)
        {
          int ret = mutt_get_field(_("Chdir to: "), buf, sizeof(buf), MUTT_FILE);
          if (ret != 0)
            break;
        }
        else if (i == OP_GOTO_PARENT)
          mutt_get_parent_path(buf, buf, sizeof(buf));

        if (buf[0])
        {
          buffy = 0;
          mutt_expand_path(buf, sizeof(buf));
#ifdef USE_IMAP
          if (mx_is_imap(buf))
          {
            mutt_str_strfcpy(LastDir, buf, sizeof(LastDir));
            destroy_state(&state);
            init_state(&state, NULL);
            state.imap_browse = true;
            imap_browse(LastDir, &state);
            browser_sort(&state);
            menu->data = state.entry;
            browser_highlight_default(&state, menu);
            init_menu(&state, menu, title, sizeof(title), buffy);
          }
          else
#endif
          {
            if (*buf != '/')
            {
              /* in case dir is relative, make it relative to LastDir,
               * not current working dir */
              char tmp[PATH_MAX];
              mutt_file_concat_path(tmp, LastDir, buf, sizeof(tmp));
              mutt_str_strfcpy(buf, tmp, sizeof(buf));
            }
            /* Resolve path from <chdir>
             * Avoids buildup such as /a/b/../../c
             * Symlinks are always unraveled to keep code simple */
            if (mutt_realpath(buf) == 0)
              break;

            if (stat(buf, &st) == 0)
            {
              if (S_ISDIR(st.st_mode))
              {
                destroy_state(&state);
                if (examine_directory(menu, &state, buf, prefix) == 0)
                  mutt_str_strfcpy(LastDir, buf, sizeof(LastDir));
                else
                {
                  mutt_error(_("Error scanning directory."));
                  if (examine_directory(menu, &state, LastDir, prefix) == -1)
                  {
                    goto bail;
                  }
                }
                browser_highlight_default(&state, menu);
                init_menu(&state, menu, title, sizeof(title), buffy);
              }
              else
                mutt_error(_("%s is not a directory."), buf);
            }
            else
              mutt_perror(buf);
          }
        }
        break;

      case OP_ENTER_MASK:

        mutt_str_strfcpy(buf, NONULL(Mask ? Mask->pattern : NULL), sizeof(buf));
        if (mutt_get_field(_("File Mask: "), buf, sizeof(buf), 0) == 0)
        {
          regex_t *rx = mutt_mem_malloc(sizeof(regex_t));
          char *s = buf;
          int not = 0, err;

          buffy = 0;
          /* assume that the user wants to see everything */
          if (!buf[0])
            mutt_str_strfcpy(buf, ".", sizeof(buf));
          SKIPWS(s);
          if (*s == '!')
          {
            s++;
            SKIPWS(s);
            not = 1;
          }

          err = REGCOMP(rx, s, REG_NOSUB);
          if (err != 0)
          {
            regerror(err, rx, buf, sizeof(buf));
            FREE(&rx);
            mutt_error("%s", buf);
          }
          else
          {
            if (Mask && Mask->regex)
            {
              regfree(Mask->regex);
              FREE(&Mask->regex);
            }
            else
            {
              Mask = mutt_mem_calloc(1, sizeof(struct Regex));
            }
            mutt_str_replace(&Mask->pattern, buf);
            Mask->regex = rx;
            Mask->not = not;

            destroy_state(&state);
#ifdef USE_IMAP
            if (state.imap_browse)
            {
              init_state(&state, NULL);
              state.imap_browse = true;
              imap_browse(LastDir, &state);
              browser_sort(&state);
              menu->data = state.entry;
              init_menu(&state, menu, title, sizeof(title), buffy);
            }
            else
#endif
                if (examine_directory(menu, &state, LastDir, NULL) == 0)
              init_menu(&state, menu, title, sizeof(title), buffy);
            else
            {
              mutt_error(_("Error scanning directory."));
              goto bail;
            }
            kill_prefix = 0;
            if (state.entrylen == 0)
            {
              mutt_error(_("No files match the file mask"));
              break;
            }
          }
        }
        break;

      case OP_SORT:
      case OP_SORT_REVERSE:

      {
        bool resort = true;
        int sort = -1;
        int reverse = (i == OP_SORT_REVERSE);

        switch (mutt_multi_choice(
            (reverse) ?
                /* L10N: The highlighted letters must match the "Sort" options */
                _("Reverse sort by (d)ate, (a)lpha, si(z)e, d(e)scription, "
                  "(c)ount, ne(w) count, or do(n)'t sort? ") :
                /* L10N: The highlighted letters must match the "Reverse Sort" options */
                _("Sort by (d)ate, (a)lpha, si(z)e, d(e)scription, (c)ount, "
                  "ne(w) count, or do(n)'t sort? "),
            /* L10N: These must match the highlighted letters from "Sort" and "Reverse Sort" */
            _("dazecwn")))
        {
          case -1: /* abort */
            resort = false;
            break;

          case 1: /* (d)ate */
            sort = SORT_DATE;
            break;

          case 2: /* (a)lpha */
            sort = SORT_SUBJECT;
            break;

          case 3: /* si(z)e */
            sort = SORT_SIZE;
            break;

          case 4: /* d(e)scription */
            sort = SORT_DESC;
            break;

          case 5: /* (c)ount */
            sort = SORT_COUNT;
            break;

          case 6: /* ne(w) count */
            sort = SORT_UNREAD;
            break;

          case 7: /* do(n)'t sort */
            sort = SORT_ORDER;
            resort = false;
            break;
        }
        if (resort)
        {
          sort |= reverse ? SORT_REVERSE : 0;
          browser_sort(&state);
          browser_highlight_default(&state, menu);
          menu->redraw = REDRAW_FULL;
        }
        SortBrowser = sort;
        break;
      }

      case OP_TOGGLE_MAILBOXES:
      case OP_BROWSER_GOTO_FOLDER:
      case OP_CHECK_NEW:
        if (i == OP_TOGGLE_MAILBOXES)
          buffy = 1 - buffy;

        if (i == OP_BROWSER_GOTO_FOLDER)
        {
          /* When in mailboxes mode, disables this feature */
          if (Folder)
          {
            mutt_debug(5, "= hit! Folder: %s, LastDir: %s\n", Folder, LastDir);
            if (!GotoSwapper[0])
            {
              if (mutt_str_strcmp(LastDir, Folder) != 0)
              {
                /* Stores into GotoSwapper LastDir, and swaps to Folder */
                mutt_str_strfcpy(GotoSwapper, LastDir, sizeof(GotoSwapper));
                mutt_str_strfcpy(OldLastDir, LastDir, sizeof(OldLastDir));
                mutt_str_strfcpy(LastDir, Folder, sizeof(LastDir));
              }
            }
            else
            {
              mutt_str_strfcpy(OldLastDir, LastDir, sizeof(OldLastDir));
              mutt_str_strfcpy(LastDir, GotoSwapper, sizeof(LastDir));
              GotoSwapper[0] = '\0';
            }
          }
        }
        destroy_state(&state);
        prefix[0] = 0;
        kill_prefix = 0;

        if (buffy)
        {
          examine_mailboxes(menu, &state);
        }
#ifdef USE_IMAP
        else if (mx_is_imap(LastDir))
        {
          init_state(&state, NULL);
          state.imap_browse = true;
          imap_browse(LastDir, &state);
          browser_sort(&state);
          menu->data = state.entry;
        }
#endif
        else if (examine_directory(menu, &state, LastDir, prefix) == -1)
          goto bail;
        init_menu(&state, menu, title, sizeof(title), buffy);
        break;

      case OP_BUFFY_LIST:
        mutt_buffy_list();
        break;

      case OP_BROWSER_NEW_FILE:

        snprintf(buf, sizeof(buf), "%s/", LastDir);
        if (mutt_get_field(_("New file name: "), buf, sizeof(buf), MUTT_FILE) == 0)
        {
          mutt_str_strfcpy(f, buf, flen);
          destroy_state(&state);
          goto bail;
        }
        break;

      case OP_BROWSER_VIEW_FILE:
        if (state.entrylen == 0)
        {
          mutt_error(_("No files match the file mask"));
          break;
        }

#ifdef USE_IMAP
        if (state.entry[menu->current].selectable)
        {
          mutt_str_strfcpy(f, state.entry[menu->current].name, flen);
          destroy_state(&state);
          goto bail;
        }
        else
#endif
            if (S_ISDIR(state.entry[menu->current].mode) ||
                (S_ISLNK(state.entry[menu->current].mode) &&
                 link_is_dir(LastDir, state.entry[menu->current].name)))
        {
          mutt_error(_("Can't view a directory"));
          break;
        }
        else
        {
          char buf2[PATH_MAX];

          mutt_file_concat_path(buf2, LastDir, state.entry[menu->current].name,
                                sizeof(buf2));
          struct Body *b = mutt_make_file_attach(buf2);
          if (b)
          {
            mutt_view_attachment(NULL, b, MUTT_REGULAR, NULL, NULL);
            mutt_body_free(&b);
            menu->redraw = REDRAW_FULL;
          }
          else
            mutt_error(_("Error trying to view file"));
        }
        break;

#ifdef USE_NNTP
      case OP_CATCHUP:
      case OP_UNCATCHUP:
        if (OptNews)
        {
          struct FolderFile *ff = &state.entry[menu->current];
          int rc;
          struct NntpData *nntp_data = NULL;

          rc = nntp_newsrc_parse(CurrentNewsSrv);
          if (rc < 0)
            break;

          if (i == OP_CATCHUP)
            nntp_data = mutt_newsgroup_catchup(CurrentNewsSrv, ff->name);
          else
            nntp_data = mutt_newsgroup_uncatchup(CurrentNewsSrv, ff->name);

          if (nntp_data)
          {
            nntp_newsrc_update(CurrentNewsSrv);
            if (menu->current + 1 < menu->max)
              menu->current++;
            menu->redraw = REDRAW_MOTION_RESYNCH;
          }
          if (rc)
            menu->redraw = REDRAW_INDEX;
          nntp_newsrc_close(CurrentNewsSrv);
        }
        break;

      case OP_LOAD_ACTIVE:
        if (OptNews)
        {
          struct NntpServer *nserv = CurrentNewsSrv;

          if (nntp_newsrc_parse(nserv) < 0)
            break;

          for (unsigned int j = 0; j < nserv->groups_num; j++)
          {
            struct NntpData *nntp_data = nserv->groups_list[j];
            if (nntp_data)
              nntp_data->deleted = true;
          }
          nntp_active_fetch(nserv, true);
          nntp_newsrc_update(nserv);
          nntp_newsrc_close(nserv);

          destroy_state(&state);
          if (buffy)
            examine_mailboxes(menu, &state);
          else
          {
            if (examine_directory(menu, &state, NULL, NULL) == -1)
              break;
          }
          init_menu(&state, menu, title, sizeof(title), buffy);
        }
        break;
#endif /* USE_NNTP */

#if defined(USE_IMAP) || defined(USE_NNTP)
      case OP_BROWSER_SUBSCRIBE:
      case OP_BROWSER_UNSUBSCRIBE:
#endif
#ifdef USE_NNTP
      case OP_SUBSCRIBE_PATTERN:
      case OP_UNSUBSCRIBE_PATTERN:
        if (OptNews)
        {
          struct NntpServer *nserv = CurrentNewsSrv;
          regex_t rx;
          memset(&rx, 0, sizeof(rx));
          char *s = buf;
          int rc, j = menu->current;

          if (i == OP_SUBSCRIBE_PATTERN || i == OP_UNSUBSCRIBE_PATTERN)
          {
            char tmp[STRING];
            int err;

            buf[0] = 0;
            if (i == OP_SUBSCRIBE_PATTERN)
              snprintf(tmp, sizeof(tmp), _("Subscribe pattern: "));
            else
              snprintf(tmp, sizeof(tmp), _("Unsubscribe pattern: "));
            if (mutt_get_field(tmp, buf, sizeof(buf), 0) != 0 || !buf[0])
            {
              break;
            }

            err = REGCOMP(&rx, s, REG_NOSUB);
            if (err != 0)
            {
              regerror(err, &rx, buf, sizeof(buf));
              regfree(&rx);
              mutt_error("%s", buf);
              break;
            }
            menu->redraw = REDRAW_FULL;
            j = 0;
          }
          else if (state.entrylen == 0)
          {
            mutt_error(_("No newsgroups match the mask"));
            break;
          }

          rc = nntp_newsrc_parse(nserv);
          if (rc < 0)
            break;

          for (; j < state.entrylen; j++)
          {
            struct FolderFile *ff = &state.entry[j];

            if (i == OP_BROWSER_SUBSCRIBE || i == OP_BROWSER_UNSUBSCRIBE ||
                regexec(&rx, ff->name, 0, NULL, 0) == 0)
            {
              if (i == OP_BROWSER_SUBSCRIBE || i == OP_SUBSCRIBE_PATTERN)
                mutt_newsgroup_subscribe(nserv, ff->name);
              else
                mutt_newsgroup_unsubscribe(nserv, ff->name);
            }
            if (i == OP_BROWSER_SUBSCRIBE || i == OP_BROWSER_UNSUBSCRIBE)
            {
              if (menu->current + 1 < menu->max)
                menu->current++;
              menu->redraw = REDRAW_MOTION_RESYNCH;
              break;
            }
          }
          if (i == OP_SUBSCRIBE_PATTERN)
          {
            for (unsigned int k = 0; nserv && (k < nserv->groups_num); k++)
            {
              struct NntpData *nntp_data = nserv->groups_list[k];
              if (nntp_data && nntp_data->group && !nntp_data->subscribed)
              {
                if (regexec(&rx, nntp_data->group, 0, NULL, 0) == 0)
                {
                  mutt_newsgroup_subscribe(nserv, nntp_data->group);
                  add_folder(menu, &state, nntp_data->group, NULL, NULL, NULL, nntp_data);
                }
              }
            }
            init_menu(&state, menu, title, sizeof(title), buffy);
          }
          if (rc > 0)
            menu->redraw = REDRAW_FULL;
          nntp_newsrc_update(nserv);
          nntp_clear_cache(nserv);
          nntp_newsrc_close(nserv);
          if (i != OP_BROWSER_SUBSCRIBE && i != OP_BROWSER_UNSUBSCRIBE)
            regfree(&rx);
        }
#ifdef USE_IMAP
        else
#endif /* USE_IMAP && USE_NNTP */
#endif /* USE_NNTP */
#ifdef USE_IMAP
        {
          if (i == OP_BROWSER_SUBSCRIBE)
            imap_subscribe(state.entry[menu->current].name, 1);
          else
            imap_subscribe(state.entry[menu->current].name, 0);
        }
#endif /* USE_IMAP */
    }
  }

bail:

  if (menu)
  {
    mutt_menu_pop_current(menu);
    mutt_menu_destroy(&menu);
  }

  if (GotoSwapper[0])
    GotoSwapper[0] = '\0';
}
