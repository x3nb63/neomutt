/**
 * @file
 * Support of Mixmaster anonymous remailer
 *
 * @authors
 * Copyright (C) 1999-2001 Thomas Roessler <roessler@does-not-exist.org>
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
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "mutt/mutt.h"
#include "mutt.h"
#include "remailer.h"
#include "filter.h"
#include "format_flags.h"
#include "globals.h"
#include "handler.h"
#include "header.h"
#include "keymap.h"
#include "mutt_curses.h"
#include "mutt_menu.h"
#include "mutt_window.h"
#include "opcodes.h"
#include "options.h"
#include "protos.h"

/**
 * struct Coord - Screen coordinates
 */
struct Coord
{
  short r; /**< row */
  short c; /**< column */
};

static int mix_get_caps(const char *capstr)
{
  int caps = 0;

  while (*capstr)
  {
    switch (*capstr)
    {
      case 'C':
        caps |= MIX_CAP_COMPRESS;
        break;

      case 'M':
        caps |= MIX_CAP_MIDDLEMAN;
        break;

      case 'N':
      {
        switch (*++capstr)
        {
          case 'm':
            caps |= MIX_CAP_NEWSMAIL;
            break;

          case 'p':
            caps |= MIX_CAP_NEWSPOST;
            break;
        }
      }
    }

    if (*capstr)
      capstr++;
  }

  return caps;
}

static void mix_add_entry(struct Remailer ***type2_list, struct Remailer *entry,
                          size_t *slots, size_t *used)
{
  if (*used == *slots)
  {
    *slots += 5;
    mutt_mem_realloc(type2_list, sizeof(struct Remailer *) * (*slots));
  }

  (*type2_list)[(*used)++] = entry;
  if (entry)
    entry->num = *used;
}

static struct Remailer *mix_new_remailer(void)
{
  return mutt_mem_calloc(1, sizeof(struct Remailer));
}

static void mix_free_remailer(struct Remailer **r)
{
  FREE(&(*r)->shortname);
  FREE(&(*r)->addr);
  FREE(&(*r)->ver);

  FREE(r);
}

/**
 * mix_type2_list - parse the type2.list as given by mixmaster -T
 */
static struct Remailer **mix_type2_list(size_t *l)
{
  FILE *fp = NULL;
  pid_t mm_pid;
  int devnull;

  char cmd[HUGE_STRING];
  char line[HUGE_STRING];
  char *t = NULL;

  struct Remailer **type2_list = NULL, *p = NULL;
  size_t slots = 0, used = 0;

  if (!l)
    return NULL;

  devnull = open("/dev/null", O_RDWR);
  if (devnull == -1)
    return NULL;

  snprintf(cmd, sizeof(cmd), "%s -T", Mixmaster);

  mm_pid = mutt_create_filter_fd(cmd, NULL, &fp, NULL, devnull, -1, devnull);
  if (mm_pid == -1)
  {
    close(devnull);
    return NULL;
  }

  /* first, generate the "random" remailer */

  p = mix_new_remailer();
  p->shortname = mutt_str_strdup(_("<random>"));
  mix_add_entry(&type2_list, p, &slots, &used);

  while (fgets(line, sizeof(line), fp))
  {
    p = mix_new_remailer();

    t = strtok(line, " \t\n");
    if (!t)
      goto problem;

    p->shortname = mutt_str_strdup(t);

    t = strtok(NULL, " \t\n");
    if (!t)
      goto problem;

    p->addr = mutt_str_strdup(t);

    t = strtok(NULL, " \t\n");
    if (!t)
      goto problem;

    t = strtok(NULL, " \t\n");
    if (!t)
      goto problem;

    p->ver = mutt_str_strdup(t);

    t = strtok(NULL, " \t\n");
    if (!t)
      goto problem;

    p->caps = mix_get_caps(t);

    mix_add_entry(&type2_list, p, &slots, &used);
    continue;

  problem:
    mix_free_remailer(&p);
  }

  *l = used;

  mix_add_entry(&type2_list, NULL, &slots, &used);
  mutt_wait_filter(mm_pid);

  close(devnull);

  return type2_list;
}

static void mix_free_type2_list(struct Remailer ***ttlp)
{
  struct Remailer **type2_list = *ttlp;

  for (int i = 0; type2_list[i]; i++)
    mix_free_remailer(&type2_list[i]);

  FREE(type2_list);
}

#define MIX_HOFFSET 2
#define MIX_VOFFSET (MuttIndexWindow->rows - 4)
#define MIX_MAXROW (MuttIndexWindow->rows - 1)

static void mix_screen_coordinates(struct Remailer **type2_list, struct Coord **coordsp,
                                   struct MixChain *chain, int i)
{
  short c, r;

  if (!chain->cl)
    return;

  mutt_mem_realloc(coordsp, sizeof(struct Coord) * chain->cl);

  struct Coord *coords = *coordsp;

  if (i)
  {
    c = coords[i - 1].c + strlen(type2_list[chain->ch[i - 1]]->shortname) + 2;
    r = coords[i - 1].r;
  }
  else
  {
    r = MIX_VOFFSET;
    c = MIX_HOFFSET;
  }

  for (; i < chain->cl; i++)
  {
    short oc = c;
    c += strlen(type2_list[chain->ch[i]]->shortname) + 2;

    if (c >= MuttIndexWindow->cols)
    {
      oc = c = MIX_HOFFSET;
      r++;
    }

    coords[i].c = oc;
    coords[i].r = r;
  }
}

static void mix_redraw_ce(struct Remailer **type2_list, struct Coord *coords,
                          struct MixChain *chain, int i, short selected)
{
  if (!coords || !chain)
    return;

  if (coords[i].r < MIX_MAXROW)
  {
    if (selected)
      SETCOLOR(MT_COLOR_INDICATOR);
    else
      NORMAL_COLOR;

    mutt_window_mvaddstr(MuttIndexWindow, coords[i].r, coords[i].c,
                         type2_list[chain->ch[i]]->shortname);
    NORMAL_COLOR;

    if (i + 1 < chain->cl)
      addstr(", ");
  }
}

static void mix_redraw_chain(struct Remailer **type2_list, struct Coord *coords,
                             struct MixChain *chain, int cur)
{
  for (int i = MIX_VOFFSET; i < MIX_MAXROW; i++)
  {
    mutt_window_move(MuttIndexWindow, i, 0);
    mutt_window_clrtoeol(MuttIndexWindow);
  }

  for (int i = 0; i < chain->cl; i++)
    mix_redraw_ce(type2_list, coords, chain, i, i == cur);
}

static void mix_redraw_head(struct MixChain *chain)
{
  SETCOLOR(MT_COLOR_STATUS);
  mutt_window_mvprintw(MuttIndexWindow, MIX_VOFFSET - 1, 0,
                       "-- Remailer chain [Length: %d]", chain ? chain->cl : 0);
  mutt_window_clrtoeol(MuttIndexWindow);
  NORMAL_COLOR;
}

static const char *mix_format_caps(struct Remailer *r)
{
  static char capbuf[10];
  char *t = capbuf;

  if (r->caps & MIX_CAP_COMPRESS)
    *t++ = 'C';
  else
    *t++ = ' ';

  if (r->caps & MIX_CAP_MIDDLEMAN)
    *t++ = 'M';
  else
    *t++ = ' ';

  if (r->caps & MIX_CAP_NEWSPOST)
  {
    *t++ = 'N';
    *t++ = 'p';
  }
  else
  {
    *t++ = ' ';
    *t++ = ' ';
  }

  if (r->caps & MIX_CAP_NEWSMAIL)
  {
    *t++ = 'N';
    *t++ = 'm';
  }
  else
  {
    *t++ = ' ';
    *t++ = ' ';
  }

  *t = '\0';

  return capbuf;
}

/**
 * mix_format_str - Format a string for the remailer menu
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
 * mix_format_str() is a callback function for mutt_expando_format().
 *
 * | Expando | Description
 * |:--------|:--------------------------------------------------------
 * | \%a     | The remailer's e-mail address
 * | \%c     | Remailer capabilities
 * | \%n     | The running number on the menu
 * | \%s     | The remailer's short name
 */
static const char *mix_format_str(char *buf, size_t buflen, size_t col, int cols,
                                  char op, const char *src, const char *prec,
                                  const char *if_str, const char *else_str,
                                  unsigned long data, enum FormatFlag flags)
{
  char fmt[SHORT_STRING];
  struct Remailer *remailer = (struct Remailer *) data;
  int optional = (flags & MUTT_FORMAT_OPTIONAL);

  switch (op)
  {
    case 'a':
      if (!optional)
      {
        snprintf(fmt, sizeof(fmt), "%%%ss", prec);
        snprintf(buf, buflen, fmt, NONULL(remailer->addr));
      }
      else if (!remailer->addr)
        optional = 0;
      break;

    case 'c':
      if (!optional)
      {
        snprintf(fmt, sizeof(fmt), "%%%ss", prec);
        snprintf(buf, buflen, fmt, mix_format_caps(remailer));
      }
      break;

    case 'n':
      if (!optional)
      {
        snprintf(fmt, sizeof(fmt), "%%%sd", prec);
        snprintf(buf, buflen, fmt, remailer->num);
      }
      break;

    case 's':
      if (!optional)
      {
        snprintf(fmt, sizeof(fmt), "%%%ss", prec);
        snprintf(buf, buflen, fmt, NONULL(remailer->shortname));
      }
      else if (!remailer->shortname)
        optional = 0;
      break;

    default:
      *buf = '\0';
  }

  if (optional)
    mutt_expando_format(buf, buflen, col, cols, if_str, attach_format_str, data, 0);
  else if (flags & MUTT_FORMAT_OPTIONAL)
    mutt_expando_format(buf, buflen, col, cols, else_str, attach_format_str, data, 0);
  return src;
}

/**
 * mix_entry - Format a menu item for the mixmaster chain list
 * @param[out] buf    Buffer in which to save string
 * @param[in]  buflen Buffer length
 * @param[in]  menu   Menu containing aliases
 * @param[in]  num    Index into the menu
 */
static void mix_entry(char *buf, size_t buflen, struct Menu *menu, int num)
{
  struct Remailer **type2_list = (struct Remailer **) menu->data;
  mutt_expando_format(buf, buflen, 0, MuttIndexWindow->cols,
                      NONULL(MixEntryFormat), mix_format_str,
                      (unsigned long) type2_list[num], MUTT_FORMAT_ARROWCURSOR);
}

static int mix_chain_add(struct MixChain *chain, const char *s, struct Remailer **type2_list)
{
  int i;

  if (chain->cl >= MAXMIXES)
    return -1;

  if ((mutt_str_strcmp(s, "0") == 0) || (mutt_str_strcasecmp(s, "<random>") == 0))
  {
    chain->ch[chain->cl++] = 0;
    return 0;
  }

  for (i = 0; type2_list[i]; i++)
  {
    if (mutt_str_strcasecmp(s, type2_list[i]->shortname) == 0)
    {
      chain->ch[chain->cl++] = i;
      return 0;
    }
  }

  /* replace unknown remailers by <random> */

  if (!type2_list[i])
    chain->ch[chain->cl++] = 0;

  return 0;
}

static const struct Mapping RemailerHelp[] = {
  { N_("Append"), OP_MIX_APPEND }, { N_("Insert"), OP_MIX_INSERT },
  { N_("Delete"), OP_MIX_DELETE }, { N_("Abort"), OP_EXIT },
  { N_("OK"), OP_MIX_USE },        { NULL, 0 },
};

/**
 * mix_make_chain - Create a Mixmaster chain
 * @param chainhead List if chain links
 *
 * Ask the user to select Mixmaster hosts to create a chain.
 */
void mix_make_chain(struct ListHead *chainhead)
{
  int c_cur = 0, c_old = 0;
  bool c_redraw = true;
  size_t ttll = 0;

  struct Coord *coords = NULL;

  struct Menu *menu = NULL;
  char helpstr[LONG_STRING];
  bool loop = true;

  char *t = NULL;

  struct Remailer **type2_list = mix_type2_list(&ttll);
  if (!type2_list)
  {
    mutt_error(_("Can't get mixmaster's type2.list!"));
    return;
  }

  struct MixChain *chain = mutt_mem_calloc(1, sizeof(struct MixChain));

  struct ListNode *p;
  STAILQ_FOREACH(p, chainhead, entries)
  {
    mix_chain_add(chain, p->data, type2_list);
  }
  mutt_list_free(chainhead);

  /* safety check */
  for (int i = 0; i < chain->cl; i++)
  {
    if (chain->ch[i] >= ttll)
      chain->ch[i] = 0;
  }

  mix_screen_coordinates(type2_list, &coords, chain, 0);

  menu = mutt_menu_new(MENU_MIX);
  menu->max = ttll;
  menu->make_entry = mix_entry;
  menu->tag = NULL;
  menu->title = _("Select a remailer chain.");
  menu->data = type2_list;
  menu->help = mutt_compile_help(helpstr, sizeof(helpstr), MENU_MIX, RemailerHelp);
  menu->pagelen = MIX_VOFFSET - 1;
  mutt_menu_push_current(menu);

  while (loop)
  {
    if (menu->pagelen != MIX_VOFFSET - 1)
    {
      menu->pagelen = MIX_VOFFSET - 1;
      menu->redraw = REDRAW_FULL;
    }

    if (c_redraw)
    {
      mix_redraw_head(chain);
      mix_redraw_chain(type2_list, coords, chain, c_cur);
      c_redraw = false;
    }
    else if (c_cur != c_old)
    {
      mix_redraw_ce(type2_list, coords, chain, c_old, 0);
      mix_redraw_ce(type2_list, coords, chain, c_cur, 1);
    }

    c_old = c_cur;

    const int op = mutt_menu_loop(menu);
    switch (op)
    {
      case OP_REDRAW:
      {
        menu_redraw_status(menu);
        mix_redraw_head(chain);
        mix_screen_coordinates(type2_list, &coords, chain, 0);
        mix_redraw_chain(type2_list, coords, chain, c_cur);
        menu->pagelen = MIX_VOFFSET - 1;
        break;
      }

      case OP_EXIT:
      {
        chain->cl = 0;
        loop = false;
        break;
      }

      case OP_MIX_USE:
      {
        if (!chain->cl)
        {
          chain->cl++;
          chain->ch[0] = menu->current;
          mix_screen_coordinates(type2_list, &coords, chain, c_cur);
          c_redraw = true;
        }

        if (chain->cl && chain->ch[chain->cl - 1] &&
            (type2_list[chain->ch[chain->cl - 1]]->caps & MIX_CAP_MIDDLEMAN))
        {
          mutt_error(
              _("Error: %s can't be used as the final remailer of a chain."),
              type2_list[chain->ch[chain->cl - 1]]->shortname);
        }
        else
        {
          loop = false;
        }
        break;
      }

      case OP_GENERIC_SELECT_ENTRY:
      case OP_MIX_APPEND:
      {
        if (chain->cl < MAXMIXES && c_cur < chain->cl)
          c_cur++;
      }
      /* fallthrough */
      case OP_MIX_INSERT:
      {
        if (chain->cl < MAXMIXES)
        {
          chain->cl++;
          for (int i = chain->cl - 1; i > c_cur; i--)
            chain->ch[i] = chain->ch[i - 1];

          chain->ch[c_cur] = menu->current;
          mix_screen_coordinates(type2_list, &coords, chain, c_cur);
          c_redraw = true;
        }
        else
        {
          /* L10N The '%d' here hard-coded to 19 */
          mutt_error(_("Mixmaster chains are limited to %d elements."), MAXMIXES);
        }

        break;
      }

      case OP_MIX_DELETE:
      {
        if (chain->cl)
        {
          chain->cl--;

          for (int i = c_cur; i < chain->cl; i++)
            chain->ch[i] = chain->ch[i + 1];

          if (c_cur == chain->cl && c_cur)
            c_cur--;

          mix_screen_coordinates(type2_list, &coords, chain, c_cur);
          c_redraw = true;
        }
        else
        {
          mutt_error(_("The remailer chain is already empty."));
        }
        break;
      }

      case OP_MIX_CHAIN_PREV:
      {
        if (c_cur)
          c_cur--;
        else
          mutt_error(_("You already have the first chain element selected."));

        break;
      }

      case OP_MIX_CHAIN_NEXT:
      {
        if (chain->cl && c_cur < chain->cl - 1)
          c_cur++;
        else
          mutt_error(_("You already have the last chain element selected."));

        break;
      }
    }
  }

  mutt_menu_pop_current(menu);
  mutt_menu_destroy(&menu);

  /* construct the remailer list */

  if (chain->cl)
  {
    for (int i = 0; i < chain->cl; i++)
    {
      const int j = chain->ch[i];
      if (j != 0)
        t = type2_list[j]->shortname;
      else
        t = "*";

      mutt_list_insert_tail(chainhead, mutt_str_strdup(t));
    }
  }

  mix_free_type2_list(&type2_list);
  FREE(&coords);
  FREE(&chain);
}

/**
 * mix_check_message - Safety-check the message before passing it to mixmaster
 */
int mix_check_message(struct Header *msg)
{
  const char *fqdn = NULL;
  bool need_hostname = false;

  if (msg->env->cc || msg->env->bcc)
  {
    mutt_error(_("Mixmaster doesn't accept Cc or Bcc headers."));
    return -1;
  }

  /* When using mixmaster, we MUST qualify any addresses since
   * the message will be delivered through remote systems.
   *
   * use_domain won't be respected at this point, hidden_host will.
   */

  for (struct Address *a = msg->env->to; a; a = a->next)
  {
    if (!a->group && strchr(a->mailbox, '@') == NULL)
    {
      need_hostname = true;
      break;
    }
  }

  if (need_hostname)
  {
    fqdn = mutt_fqdn(true);
    if (!fqdn)
    {
      mutt_error(_("Please set the hostname variable to a proper value when "
                   "using mixmaster!"));
      return -1;
    }

    /* Cc and Bcc are empty at this point. */
    mutt_addr_qualify(msg->env->to, fqdn);
    mutt_addr_qualify(msg->env->reply_to, fqdn);
    mutt_addr_qualify(msg->env->mail_followup_to, fqdn);
  }

  return 0;
}

/**
 * mix_send_message - Send an email via Mixmaster
 * @param chain    String list of hosts
 * @param tempfile Temporary file containing email
 * @retval -1  Error
 * @retval >=0 Success (Mixmaster's return code)
 */
int mix_send_message(struct ListHead *chain, const char *tempfile)
{
  char cmd[HUGE_STRING];
  char tmp[HUGE_STRING];
  char cd_quoted[STRING];
  int i;

  snprintf(cmd, sizeof(cmd), "cat %s | %s -m ", tempfile, Mixmaster);

  struct ListNode *np;
  STAILQ_FOREACH(np, chain, entries)
  {
    mutt_str_strfcpy(tmp, cmd, sizeof(tmp));
    mutt_file_quote_filename(cd_quoted, sizeof(cd_quoted), np->data);
    snprintf(cmd, sizeof(cmd), "%s%s%s", tmp,
             (np == STAILQ_FIRST(chain)) ? " -l " : ",", cd_quoted);
  }

  mutt_endwin();

  i = mutt_system(cmd);
  if (i != 0)
  {
    fprintf(stderr, _("Error sending message, child exited %d.\n"), i);
    if (!OptNoCurses)
    {
      mutt_any_key_to_continue(NULL);
      mutt_error(_("Error sending message."));
    }
  }

  unlink(tempfile);
  return i;
}
