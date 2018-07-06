/**
 * @file
 * Parse and identify different URL schemes
 *
 * @authors
 * Copyright (C) 2000-2002,2004 Thomas Roessler <roessler@does-not-exist.org>
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
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include "mutt/mutt.h"
#include "mutt.h"
#include "url.h"
#include "globals.h"
#include "protos.h"
#include "rfc2047.h"

static const struct Mapping UrlMap[] = {
  { "file", U_FILE },       { "imap", U_IMAP },     { "imaps", U_IMAPS },
  { "pop", U_POP },         { "pops", U_POPS },     { "news", U_NNTP },
  { "snews", U_NNTPS },     { "mailto", U_MAILTO },
#ifdef USE_NOTMUCH
  { "notmuch", U_NOTMUCH },
#endif
  { "smtp", U_SMTP },       { "smtps", U_SMTPS },   { NULL, U_UNKNOWN },
};

int url_pct_decode(char *s)
{
  char *d = NULL;

  if (!s)
    return -1;

  for (d = s; *s; s++)
  {
    if (*s == '%')
    {
      if (s[1] && s[2] && isxdigit((unsigned char) s[1]) &&
          isxdigit((unsigned char) s[2]) && hexval(s[1]) >= 0 && hexval(s[2]) >= 0)
      {
        *d++ = (hexval(s[1]) << 4) | (hexval(s[2]));
        s += 2;
      }
      else
        return -1;
    }
    else
      *d++ = *s;
  }
  *d = '\0';
  return 0;
}

enum UrlScheme url_check_scheme(const char *s)
{
  char sbuf[STRING];
  char *t = NULL;
  int i;

  if (!s || !(t = strchr(s, ':')))
    return U_UNKNOWN;
  if ((size_t)(t - s) >= sizeof(sbuf) - 1)
    return U_UNKNOWN;

  mutt_str_strfcpy(sbuf, s, t - s + 1);
  for (t = sbuf; *t; t++)
    *t = tolower(*t);

  i = mutt_map_get_value(sbuf, UrlMap);
  if (i == -1)
    return U_UNKNOWN;
  else
    return (enum UrlScheme) i;
}

static int parse_query_string(struct Url *u, char *src)
{
  struct UrlQueryString *qs = NULL;
  char *k = NULL, *v = NULL;

  while (src && *src)
  {
    qs = mutt_mem_calloc(1, sizeof(struct UrlQueryString));
    k = strchr(src, '&');
    if (k)
      *k = '\0';

    v = strchr(src, '=');
    if (v)
    {
      *v = '\0';
      qs->value = v + 1;
      if (url_pct_decode(qs->value) < 0)
      {
        FREE(&qs);
        return -1;
      }
    }
    qs->name = src;
    if (url_pct_decode(qs->name) < 0)
    {
      FREE(&qs);
      return -1;
    }
    STAILQ_INSERT_TAIL(&u->query_strings, qs, entries);

    if (!k)
      break;
    src = k + 1;
  }
  return 0;
}

/**
 * url_parse - Fill in Url
 * @param u   Url where the result is stored
 * @param src String to parse
 * @retval 0  String is valid
 * @retval -1 String is invalid
 *
 * char* elements are pointers into src, which is modified by this call
 * (duplicate it first if you need to).
 *
 * This method doesn't allocated any additional char* of the Url and
 * UrlQueryString structs.
 *
 * To free Url, caller must free "src" and call url_free()
 */
int url_parse(struct Url *u, char *src)
{
  char *t = NULL, *p = NULL;

  u->user = NULL;
  u->pass = NULL;
  u->host = NULL;
  u->port = 0;
  u->path = NULL;
  STAILQ_INIT(&u->query_strings);

  u->scheme = url_check_scheme(src);
  if (u->scheme == U_UNKNOWN)
    return -1;

  src = strchr(src, ':') + 1;

  if (strncmp(src, "//", 2) != 0)
  {
    u->path = src;
    return url_pct_decode(u->path);
  }

  src += 2;

  /* Notmuch and mailto schemes can include a query */
#ifdef USE_NOTMUCH
  if ((u->scheme == U_NOTMUCH) || (u->scheme == U_MAILTO))
#else
  if (u->scheme == U_MAILTO)
#endif
  {
    t = strrchr(src, '?');
    if (t)
    {
      *t++ = '\0';
      if (parse_query_string(u, t) < 0)
        goto err;
    }
  }

  u->path = strchr(src, '/');
  if (u->path)
  {
    *u->path++ = '\0';
    if (url_pct_decode(u->path) < 0)
      goto err;
  }

  t = strrchr(src, '@');
  if (t)
  {
    *t = '\0';
    p = strchr(src, ':');
    if (p)
    {
      *p = '\0';
      u->pass = p + 1;
      if (url_pct_decode(u->pass) < 0)
        goto err;
    }
    u->user = src;
    if (url_pct_decode(u->user) < 0)
      goto err;
    src = t + 1;
  }

  /* IPv6 literal address.  It may contain colons, so set t to start
   * the port scan after it.
   */
  if ((*src == '[') && (t = strchr(src, ']')))
  {
    src++;
    *t++ = '\0';
  }
  else
    t = src;

  p = strchr(t, ':');
  if (p)
  {
    int num;
    *p++ = '\0';
    if (mutt_str_atoi(p, &num) < 0 || num < 0 || num > 0xffff)
      goto err;
    u->port = (unsigned short) num;
  }
  else
    u->port = 0;

  if (mutt_str_strlen(src) != 0)
  {
    u->host = src;
    if (url_pct_decode(u->host) < 0)
      goto err;
  }
  else if (u->path)
  {
    /* No host are provided, we restore the / because this is absolute path */
    u->path = src;
    *src++ = '/';
  }

  return 0;

err:
  url_free(u);
  return -1;
}

void url_free(struct Url *u)
{
  struct UrlQueryString *np = STAILQ_FIRST(&u->query_strings), *next = NULL;
  while (np)
  {
    next = STAILQ_NEXT(np, entries);
    /* NOTE(sileht): We don't free members, they will be freed when
     * the src char* passed to url_parse() is freed */
    FREE(&np);
    np = next;
  }
  STAILQ_INIT(&u->query_strings);
}

void url_pct_encode(char *dst, size_t l, const char *src)
{
  static const char *alph = "0123456789ABCDEF";

  *dst = 0;
  l--;
  while (src && *src && l)
  {
    if (strchr("/:&%", *src) && l > 3)
    {
      *dst++ = '%';
      *dst++ = alph[(*src >> 4) & 0xf];
      *dst++ = alph[*src & 0xf];
      src++;
      continue;
    }
    *dst++ = *src++;
  }
  *dst = 0;
}

/**
 * url_tostring - output the URL string for a given Url object
 */
int url_tostring(struct Url *u, char *dest, size_t len, int flags)
{
  if (u->scheme == U_UNKNOWN)
    return -1;

  snprintf(dest, len, "%s:", mutt_map_get_name(u->scheme, UrlMap));

  if (u->host)
  {
    if (!(flags & U_PATH))
      mutt_str_strcat(dest, len, "//");
    size_t l = strlen(dest);
    len -= l;
    dest += l;

    if (u->user && (u->user[0] || !(flags & U_PATH)))
    {
      char str[STRING];
      url_pct_encode(str, sizeof(str), u->user);

      if (flags & U_DECODE_PASSWD && u->pass)
      {
        char p[STRING];
        url_pct_encode(p, sizeof(p), u->pass);
        snprintf(dest, len, "%s:%s@", str, p);
      }
      else
        snprintf(dest, len, "%s@", str);

      l = strlen(dest);
      len -= l;
      dest += l;
    }

    if (strchr(u->host, ':'))
      snprintf(dest, len, "[%s]", u->host);
    else
      snprintf(dest, len, "%s", u->host);

    l = strlen(dest);
    len -= l;
    dest += l;

    if (u->port)
      snprintf(dest, len, ":%hu/", u->port);
    else
      snprintf(dest, len, "/");
  }

  if (u->path)
    mutt_str_strcat(dest, len, u->path);

  return 0;
}

int url_parse_mailto(struct Envelope *e, char **body, const char *src)
{
  char *p = NULL;
  char *tag = NULL, *value = NULL;

  int rc = -1;

  char *t = strchr(src, ':');
  if (!t)
    return -1;

  /* copy string for safe use of strtok() */
  char *tmp = mutt_str_strdup(t + 1);
  if (!tmp)
    return -1;

  char *headers = strchr(tmp, '?');
  if (headers)
    *headers++ = '\0';

  if (url_pct_decode(tmp) < 0)
    goto out;

  e->to = mutt_addr_parse_list(e->to, tmp);

  tag = headers ? strtok_r(headers, "&", &p) : NULL;

  for (; tag; tag = strtok_r(NULL, "&", &p))
  {
    value = strchr(tag, '=');
    if (value)
      *value++ = '\0';
    if (!value || !*value)
      continue;

    if (url_pct_decode(tag) < 0)
      goto out;
    if (url_pct_decode(value) < 0)
      goto out;

    /* Determine if this header field is on the allowed list.  Since NeoMutt
     * interprets some header fields specially (such as
     * "Attach: ~/.gnupg/secring.gpg"), care must be taken to ensure that
     * only safe fields are allowed.
     *
     * RFC2368, "4. Unsafe headers"
     * The user agent interpreting a mailto URL SHOULD choose not to create
     * a message if any of the headers are considered dangerous; it may also
     * choose to create a message with only a subset of the headers given in
     * the URL.
     */
    if (mutt_list_match(tag, &MailToAllow))
    {
      if (mutt_str_strcasecmp(tag, "body") == 0)
      {
        if (body)
          mutt_str_replace(body, value);
      }
      else
      {
        char *scratch = NULL;
        size_t taglen = mutt_str_strlen(tag);

        safe_asprintf(&scratch, "%s: %s", tag, value);
        scratch[taglen] = 0; /* overwrite the colon as mutt_rfc822_parse_line expects */
        value = mutt_str_skip_email_wsp(&scratch[taglen + 1]);
        mutt_rfc822_parse_line(e, NULL, scratch, value, 1, 0, 1);
        FREE(&scratch);
      }
    }
  }

  /* RFC2047 decode after the RFC822 parsing */
  rfc2047_decode_addrlist(e->from);
  rfc2047_decode_addrlist(e->to);
  rfc2047_decode_addrlist(e->cc);
  rfc2047_decode_addrlist(e->bcc);
  rfc2047_decode_addrlist(e->reply_to);
  rfc2047_decode_addrlist(e->mail_followup_to);
  rfc2047_decode_addrlist(e->return_path);
  rfc2047_decode_addrlist(e->sender);
  mutt_rfc2047_decode(&e->x_label);
  mutt_rfc2047_decode(&e->subject);

  rc = 0;

out:
  FREE(&tmp);
  return rc;
}
