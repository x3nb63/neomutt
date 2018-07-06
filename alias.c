/**
 * @file
 * Representation of a single alias to an email address
 *
 * @authors
 * Copyright (C) 1996-2002 Michael R. Elkins <me@mutt.org>
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
#include <errno.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>
#include "mutt/mutt.h"
#include "mutt.h"
#include "alias.h"
#include "globals.h"
#include "mutt_curses.h"
#include "options.h"
#include "protos.h"

/**
 * expand_aliases_r - Expand aliases, recursively
 * @param[in]  a    Address List
 * @param[out] expn Alias List
 * @retval ptr Address List with aliases expanded
 *
 * ListHead expn is used as temporary storage for already-expanded aliases.
 */
static struct Address *expand_aliases_r(struct Address *a, struct ListHead *expn)
{
  struct Address *head = NULL, *last = NULL, *t = NULL, *w = NULL;
  bool i;
  const char *fqdn = NULL;

  while (a)
  {
    if (!a->group && !a->personal && a->mailbox && strchr(a->mailbox, '@') == NULL)
    {
      t = mutt_alias_lookup(a->mailbox);

      if (t)
      {
        i = false;
        struct ListNode *np;
        STAILQ_FOREACH(np, expn, entries)
        {
          if (mutt_str_strcmp(a->mailbox, np->data) == 0) /* alias already found */
          {
            mutt_debug(1, "loop in alias found for '%s'\n", a->mailbox);
            i = true;
            break;
          }
        }

        if (!i)
        {
          mutt_list_insert_head(expn, mutt_str_strdup(a->mailbox));
          w = mutt_addr_copy_list(t, false);
          w = expand_aliases_r(w, expn);
          if (head)
            last->next = w;
          else
            head = last = w;
          while (last && last->next)
            last = last->next;
        }
        t = a;
        a = a->next;
        t->next = NULL;
        mutt_addr_free(&t);
        continue;
      }
      else
      {
        struct passwd *pw = getpwnam(a->mailbox);

        if (pw)
        {
          char namebuf[STRING];

          mutt_gecos_name(namebuf, sizeof(namebuf), pw);
          mutt_str_replace(&a->personal, namebuf);
        }
      }
    }

    if (head)
    {
      last->next = a;
      last = last->next;
    }
    else
      head = last = a;
    a = a->next;
    last->next = NULL;
  }

  if (UseDomain && (fqdn = mutt_fqdn(true)))
  {
    /* now qualify all local addresses */
    mutt_addr_qualify(head, fqdn);
  }

  return head;
}

/**
 * write_safe_address - Defang malicious email addresses
 * @param fp File to write to
 * @param s  Email address to defang
 *
 * if someone has an address like
 *      From: Michael `/bin/rm -f ~` Elkins <me@mutt.org>
 * and the user creates an alias for this, NeoMutt could wind up executing
 * the backticks because it writes aliases like
 *      alias me Michael `/bin/rm -f ~` Elkins <me@mutt.org>
 * To avoid this problem, use a backslash (\) to quote any backticks.  We also
 * need to quote backslashes as well, since you could defeat the above by
 * doing
 *      From: Michael \`/bin/rm -f ~\` Elkins <me@mutt.org>
 * since that would get aliased as
 *      alias me Michael \\`/bin/rm -f ~\\` Elkins <me@mutt.org>
 * which still gets evaluated because the double backslash is not a quote.
 *
 * Additionally, we need to quote ' and " characters - otherwise, neomutt will
 * interpret them on the wrong parsing step.
 *
 * $ wants to be quoted since it may indicate the start of an environment
 * variable.
 */
static void write_safe_address(FILE *fp, char *s)
{
  while (*s)
  {
    if ((*s == '\\') || (*s == '`') || (*s == '\'') || (*s == '"') || (*s == '$'))
      fputc('\\', fp);
    fputc(*s, fp);
    s++;
  }
}

/**
 * recode_buf - Convert some text between two character sets
 * @param buf    Buffer to convert
 * @param buflen Length of buffer
 *
 * The 'from' charset is controlled by the 'charset'        config variable.
 * The 'to'   charset is controlled by the 'config_charset' config variable.
 */
static void recode_buf(char *buf, size_t buflen)
{
  if (!ConfigCharset || !*ConfigCharset || !Charset)
    return;

  char *s = mutt_str_strdup(buf);
  if (!s)
    return;
  if (mutt_ch_convert_string(&s, Charset, ConfigCharset, 0) == 0)
    mutt_str_strfcpy(buf, s, buflen);
  FREE(&s);
}

/**
 * check_alias_name - Sanity-check an alias name
 * @param s       Alias to check
 * @param dest    Buffer for the result
 * @param destlen Length of buffer
 * @retval  0 Success
 * @retval -1 Error
 *
 * Only characters which are non-special to both the RFC822 and the neomutt
 * configuration parser are permitted.
 */
static int check_alias_name(const char *s, char *dest, size_t destlen)
{
  wchar_t wc;
  mbstate_t mb;
  size_t l;
  int rc = 0, dry = !dest || !destlen;

  memset(&mb, 0, sizeof(mbstate_t));

  if (!dry)
    destlen--;
  for (; s && *s && (dry || destlen) && (l = mbrtowc(&wc, s, MB_CUR_MAX, &mb)) != 0;
       s += l, destlen -= l)
  {
    int bad = l == (size_t)(-1) || l == (size_t)(-2); /* conversion error */
    bad = bad || (!dry && l > destlen); /* too few room for mb char */
    if (l == 1)
      bad = bad || (strchr("-_+=.", *s) == NULL && !iswalnum(wc));
    else
      bad = bad || !iswalnum(wc);
    if (bad)
    {
      if (dry)
        return -1;
      if (l == (size_t)(-1))
        memset(&mb, 0, sizeof(mbstate_t));
      *dest++ = '_';
      rc = -1;
    }
    else if (!dry)
    {
      memcpy(dest, s, l);
      dest += l;
    }
  }
  if (!dry)
    *dest = '\0';
  return rc;
}

/**
 * string_is_address - Does an email address match a user and domain?
 * @param str Address string to test
 * @param u   User name
 * @param d   Domain name
 * @retval true They match
 */
static bool string_is_address(const char *str, const char *u, const char *d)
{
  char buf[LONG_STRING];

  snprintf(buf, sizeof(buf), "%s@%s", NONULL(u), NONULL(d));
  if (mutt_str_strcasecmp(str, buf) == 0)
    return true;

  return false;
}

/**
 * mutt_alias_lookup - Find an Alias
 * @param s Alias string to find
 * @retval ptr  Address for the Alias
 * @retval NULL No such Alias
 *
 * @note The search is case-insensitive
 */
struct Address *mutt_alias_lookup(const char *s)
{
  struct Alias *a = NULL;

  TAILQ_FOREACH(a, &Aliases, entries)
  {
    if (mutt_str_strcasecmp(s, a->name) == 0)
      return a->addr;
  }
  return NULL;
}

/**
 * mutt_expand_aliases - Expand aliases in a List of Addresses
 * @param a First Address
 * @retval ptr Top of the de-duped list
 *
 * Duplicate addresses are dropped
 */
struct Address *mutt_expand_aliases(struct Address *a)
{
  struct Address *t = NULL;
  struct ListHead expn; /* previously expanded aliases to avoid loops */

  STAILQ_INIT(&expn);
  t = expand_aliases_r(a, &expn);
  mutt_list_free(&expn);
  return (mutt_remove_duplicates(t));
}

/**
 * mutt_expand_aliases_env - Expand aliases in all the fields of an Envelope
 * @param env Envelope to expand
 */
void mutt_expand_aliases_env(struct Envelope *env)
{
  env->from = mutt_expand_aliases(env->from);
  env->to = mutt_expand_aliases(env->to);
  env->cc = mutt_expand_aliases(env->cc);
  env->bcc = mutt_expand_aliases(env->bcc);
  env->reply_to = mutt_expand_aliases(env->reply_to);
  env->mail_followup_to = mutt_expand_aliases(env->mail_followup_to);
}

/**
 * mutt_get_address - Get an Address from an Envelope
 * @param env  Envelope to examine
 * @param pfxp Prefix for the Address, e.g. "To:"
 * @retval ptr Address in the Envelope
 */
struct Address *mutt_get_address(struct Envelope *env, char **pfxp)
{
  struct Address *addr = NULL;
  char *pfx = NULL;

  if (mutt_addr_is_user(env->from))
  {
    if (env->to && !mutt_is_mail_list(env->to))
    {
      pfx = "To";
      addr = env->to;
    }
    else
    {
      pfx = "Cc";
      addr = env->cc;
    }
  }
  else if (env->reply_to && !mutt_is_mail_list(env->reply_to))
  {
    pfx = "Reply-To";
    addr = env->reply_to;
  }
  else
  {
    addr = env->from;
    pfx = "From";
  }

  if (pfxp)
    *pfxp = pfx;

  return addr;
}

/**
 * mutt_alias_create - Create a new Alias from an Envelope or an Address
 * @param cur   Envelope to use
 * @param iaddr Address to use
 */
void mutt_alias_create(struct Envelope *cur, struct Address *iaddr)
{
  struct Alias *new = NULL;
  char buf[LONG_STRING], tmp[LONG_STRING], prompt[SHORT_STRING], *pc = NULL;
  char *err = NULL;
  char fixed[LONG_STRING];
  FILE *rc = NULL;
  struct Address *addr = NULL;

  if (cur)
  {
    addr = mutt_get_address(cur, NULL);
  }
  else if (iaddr)
  {
    addr = iaddr;
  }

  if (addr && addr->mailbox)
  {
    mutt_str_strfcpy(tmp, addr->mailbox, sizeof(tmp));
    pc = strchr(tmp, '@');
    if (pc)
      *pc = '\0';
  }
  else
    tmp[0] = '\0';

  /* Don't suggest a bad alias name in the event of a strange local part. */
  check_alias_name(tmp, buf, sizeof(buf));

retry_name:
  /* L10N: prompt to add a new alias */
  if (mutt_get_field(_("Alias as: "), buf, sizeof(buf), 0) != 0 || !buf[0])
    return;

  /* check to see if the user already has an alias defined */
  if (mutt_alias_lookup(buf))
  {
    mutt_error(_("You already have an alias defined with that name!"));
    return;
  }

  if (check_alias_name(buf, fixed, sizeof(fixed)))
  {
    switch (mutt_yesorno(_("Warning: This alias name may not work.  Fix it?"), MUTT_YES))
    {
      case MUTT_YES:
        mutt_str_strfcpy(buf, fixed, sizeof(buf));
        goto retry_name;
      case MUTT_ABORT:
        return;
    }
  }

  new = mutt_mem_calloc(1, sizeof(struct Alias));
  new->name = mutt_str_strdup(buf);

  mutt_addrlist_to_local(addr);

  if (addr && addr->mailbox)
    mutt_str_strfcpy(buf, addr->mailbox, sizeof(buf));
  else
    buf[0] = '\0';

  mutt_addrlist_to_intl(addr, NULL);

  do
  {
    if (mutt_get_field(_("Address: "), buf, sizeof(buf), 0) != 0 || !buf[0])
    {
      mutt_alias_free(&new);
      return;
    }

    new->addr = mutt_addr_parse_list(new->addr, buf);
    if (!new->addr)
      BEEP();
    if (mutt_addrlist_to_intl(new->addr, &err))
    {
      mutt_error(_("Bad IDN: '%s'"), err);
      continue;
    }
  } while (!new->addr);

  if (addr && addr->personal && !mutt_is_mail_list(addr))
    mutt_str_strfcpy(buf, addr->personal, sizeof(buf));
  else
    buf[0] = '\0';

  if (mutt_get_field(_("Personal name: "), buf, sizeof(buf), 0) != 0)
  {
    mutt_alias_free(&new);
    return;
  }
  mutt_str_replace(&new->addr->personal, buf);

  buf[0] = '\0';
  mutt_addr_write(buf, sizeof(buf), new->addr, true);
  snprintf(prompt, sizeof(prompt), _("[%s = %s] Accept?"), new->name, buf);
  if (mutt_yesorno(prompt, MUTT_YES) != MUTT_YES)
  {
    mutt_alias_free(&new);
    return;
  }

  mutt_alias_add_reverse(new);

  TAILQ_INSERT_TAIL(&Aliases, new, entries);

  mutt_str_strfcpy(buf, NONULL(AliasFile), sizeof(buf));
  if (mutt_get_field(_("Save to file: "), buf, sizeof(buf), MUTT_FILE) != 0)
    return;
  mutt_expand_path(buf, sizeof(buf));
  rc = fopen(buf, "a+");
  if (rc)
  {
    /* terminate existing file with \n if necessary */
    if (fseek(rc, 0, SEEK_END))
      goto fseek_err;
    if (ftell(rc) > 0)
    {
      if (fseek(rc, -1, SEEK_CUR) < 0)
        goto fseek_err;
      if (fread(buf, 1, 1, rc) != 1)
      {
        mutt_perror(_("Error reading alias file"));
        mutt_file_fclose(&rc);
        return;
      }
      if (fseek(rc, 0, SEEK_END) < 0)
        goto fseek_err;
      if (buf[0] != '\n')
        fputc('\n', rc);
    }

    if (check_alias_name(new->name, NULL, 0))
      mutt_file_quote_filename(buf, sizeof(buf), new->name);
    else
      mutt_str_strfcpy(buf, new->name, sizeof(buf));
    recode_buf(buf, sizeof(buf));
    fprintf(rc, "alias %s ", buf);
    buf[0] = '\0';
    mutt_addr_write(buf, sizeof(buf), new->addr, false);
    recode_buf(buf, sizeof(buf));
    write_safe_address(rc, buf);
    fputc('\n', rc);
    if (mutt_file_fsync_close(&rc) != 0)
      mutt_message("Trouble adding alias: %s.", strerror(errno));
    else
      mutt_message(_("Alias added."));
  }
  else
    mutt_perror(buf);

  return;

fseek_err:
  mutt_perror(_("Error seeking in alias file"));
  mutt_file_fclose(&rc);
  return;
}

/**
 * mutt_alias_reverse_lookup - Does the user have an alias for the given address
 * @param a Address to lookup
 * @retval ptr Matching Address
 */
struct Address *mutt_alias_reverse_lookup(struct Address *a)
{
  if (!a || !a->mailbox)
    return NULL;

  return mutt_hash_find(ReverseAliases, a->mailbox);
}

/**
 * mutt_alias_add_reverse - Add an email address lookup for an Alias
 * @param t Alias to use
 */
void mutt_alias_add_reverse(struct Alias *t)
{
  struct Address *ap = NULL;
  if (!t)
    return;

  /* Note that the address mailbox should be converted to intl form
   * before using as a key in the hash.  This is currently done
   * by all callers, but added here mostly as documentation. */
  mutt_addrlist_to_intl(t->addr, NULL);

  for (ap = t->addr; ap; ap = ap->next)
  {
    if (!ap->group && ap->mailbox)
      mutt_hash_insert(ReverseAliases, ap->mailbox, ap);
  }
}

/**
 * mutt_alias_delete_reverse - Remove an email address lookup for an Alias
 * @param t Alias to use
 */
void mutt_alias_delete_reverse(struct Alias *t)
{
  struct Address *ap = NULL;
  if (!t)
    return;

  /* If the alias addresses were converted to local form, they won't
   * match the hash entries. */
  mutt_addrlist_to_intl(t->addr, NULL);

  for (ap = t->addr; ap; ap = ap->next)
  {
    if (!ap->group && ap->mailbox)
      mutt_hash_delete(ReverseAliases, ap->mailbox, ap);
  }
}

/**
 * mutt_alias_complete - alias completion routine
 * @param buf    Partial Alias to complete
 * @param buflen Length of buffer
 * @retval 1 Success
 * @retval 0 Error
 *
 * Given a partial alias, this routine attempts to fill in the alias
 * from the alias list as much as possible. if given empty search string
 * or found nothing, present all aliases
 */
int mutt_alias_complete(char *buf, size_t buflen)
{
  struct Alias *a = NULL, *tmp = NULL;
  struct AliasList a_list = TAILQ_HEAD_INITIALIZER(a_list);
  char bestname[HUGE_STRING] = { 0 };

  if (buf[0] != 0) /* avoid empty string as strstr argument */
  {
    TAILQ_FOREACH(a, &Aliases, entries)
    {
      if (a->name && strncmp(a->name, buf, strlen(buf)) == 0)
      {
        if (!bestname[0]) /* init */
        {
          mutt_str_strfcpy(bestname, a->name,
                           MIN(mutt_str_strlen(a->name) + 1, sizeof(bestname)));
        }
        else
        {
          int i;
          for (i = 0; a->name[i] && (a->name[i] == bestname[i]); i++)
            ;
          bestname[i] = '\0';
        }
      }
    }

    if (bestname[0] != 0)
    {
      if (mutt_str_strcmp(bestname, buf) != 0)
      {
        /* we are adding something to the completion */
        mutt_str_strfcpy(buf, bestname, mutt_str_strlen(bestname) + 1);
        return 1;
      }

      /* build alias list and show it */
      TAILQ_FOREACH(a, &Aliases, entries)
      {
        if (a->name && strncmp(a->name, buf, strlen(buf)) == 0)
        {
          tmp = mutt_mem_calloc(1, sizeof(struct Alias));
          memcpy(tmp, a, sizeof(struct Alias));
          TAILQ_INSERT_TAIL(&a_list, tmp, entries);
        }
      }
    }
  }

  bestname[0] = '\0';
  mutt_alias_menu(bestname, sizeof(bestname), !TAILQ_EMPTY(&a_list) ? &a_list : &Aliases);
  if (bestname[0] != 0)
    mutt_str_strfcpy(buf, bestname, buflen);

  /* free the alias list */
  TAILQ_FOREACH_SAFE(a, &a_list, entries, tmp)
  {
    TAILQ_REMOVE(&a_list, a, entries);
    FREE(&a);
  }

  /* remove any aliases marked for deletion */
  TAILQ_FOREACH_SAFE(a, &Aliases, entries, tmp)
  {
    if (a->del)
    {
      TAILQ_REMOVE(&Aliases, a, entries);
      mutt_alias_free(&a);
    }
  }

  return 0;
}

/**
 * mutt_addr_is_user - Does the address belong to the user
 * @param addr Address to check
 * @retval true if the given address belongs to the user
 */
bool mutt_addr_is_user(struct Address *addr)
{
  const char *fqdn = NULL;

  /* NULL address is assumed to be the user. */
  if (!addr)
  {
    mutt_debug(5, "yes, NULL address\n");
    return true;
  }
  if (!addr->mailbox)
  {
    mutt_debug(5, "no, no mailbox\n");
    return false;
  }

  if (mutt_str_strcasecmp(addr->mailbox, Username) == 0)
  {
    mutt_debug(5, "#1 yes, %s = %s\n", addr->mailbox, Username);
    return true;
  }
  if (string_is_address(addr->mailbox, Username, ShortHostname))
  {
    mutt_debug(5, "#2 yes, %s = %s @ %s\n", addr->mailbox, Username, ShortHostname);
    return true;
  }
  fqdn = mutt_fqdn(false);
  if (string_is_address(addr->mailbox, Username, fqdn))
  {
    mutt_debug(5, "#3 yes, %s = %s @ %s\n", addr->mailbox, Username, NONULL(fqdn));
    return true;
  }
  fqdn = mutt_fqdn(true);
  if (string_is_address(addr->mailbox, Username, fqdn))
  {
    mutt_debug(5, "#4 yes, %s = %s @ %s\n", addr->mailbox, Username, NONULL(fqdn));
    return true;
  }

  if (From && (mutt_str_strcasecmp(From->mailbox, addr->mailbox) == 0))
  {
    mutt_debug(5, "#5 yes, %s = %s\n", addr->mailbox, From->mailbox);
    return true;
  }

  if (mutt_regexlist_match(Alternates, addr->mailbox))
  {
    mutt_debug(5, "yes, %s matched by alternates.\n", addr->mailbox);
    if (mutt_regexlist_match(UnAlternates, addr->mailbox))
      mutt_debug(5, "but, %s matched by unalternates.\n", addr->mailbox);
    else
      return true;
  }

  mutt_debug(5, "no, all failed.\n");
  return false;
}

/**
 * mutt_alias_free - Free an Alias
 * @param p Alias to free
 */
void mutt_alias_free(struct Alias **p)
{
  if (!p || !*p)
    return;

  mutt_alias_delete_reverse(*p);
  FREE(&(*p)->name);
  mutt_addr_free(&(*p)->addr);
  FREE(p);
}

/**
 * mutt_aliaslist_free - Free a List of Aliases
 * @param a_list AliasList to free
 */
void mutt_aliaslist_free(struct AliasList *a_list)
{
  struct Alias *a = NULL, *tmp = NULL;
  TAILQ_FOREACH_SAFE(a, a_list, entries, tmp)
  {
    TAILQ_REMOVE(a_list, a, entries);
    mutt_alias_free(&a);
  }
  TAILQ_INIT(a_list);
}
