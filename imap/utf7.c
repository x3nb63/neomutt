/**
 * @file
 * Convert strings to/from utf7/utf8
 *
 * @authors
 * Copyright (C) 2000,2003 Edmund Grimley Evans <edmundo@rano.org>
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
 * @page imap_utf7 UTF-7 Manipulation
 *
 * Convert strings to/from utf7/utf8
 */

#include "config.h"
#include <stdbool.h>
#include <string.h>
#include "imap_private.h"
#include "mutt/mutt.h"

// clang-format off
/**
 * Index64u - Lookup table for Base64 encoding/decoding
 *
 * This is very similar to the table in lib/lib_base64.c
 * Encoding chars:
 *   utf7 A-Za-z0-9+,
 *   mime A-Za-z0-9+/
 */
const int Index64u[128] = {
    -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1,
    -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1,
    -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,62, 63,-1,-1,-1,
    52,53,54,55, 56,57,58,59, 60,61,-1,-1, -1,-1,-1,-1,
    -1, 0, 1, 2,  3, 4, 5, 6,  7, 8, 9,10, 11,12,13,14,
    15,16,17,18, 19,20,21,22, 23,24,25,-1, -1,-1,-1,-1,
    -1,26,27,28, 29,30,31,32, 33,34,35,36, 37,38,39,40,
    41,42,43,44, 45,46,47,48, 49,50,51,-1, -1,-1,-1,-1
};
// clang-format on

/**
 * B64Chars - Characters of the Base64 encoding
 */
static const char B64Chars[64] = {
  'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M',
  'N', 'O', 'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z',
  'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l', 'm',
  'n', 'o', 'p', 'q', 'r', 's', 't', 'u', 'v', 'w', 'x', 'y', 'z',
  '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', '+', ',',
};

/**
 * utf7_to_utf8 - Convert data from RFC2060's UTF-7 to UTF-8
 * @param[in]  u7    UTF-7 data
 * @param[in]  u7len Length of UTF-7 data
 * @param[out] u8    Save the UTF-8 data pointer
 * @param[out] u8len Save the UTF-8 data length
 * @retval ptr  UTF-8 data
 * @retval NULL Error
 *
 * RFC2060 obviously intends the encoding to be unique (see point 5 in section
 * 5.1.3), so we reject any non-canonical form, such as &ACY- (instead of &-)
 * or &AMA-&AMA- (instead of &AMAAwA-).
 *
 * @note The result is null-terminated.
 * @note The caller must free() the returned data.
 */
static char *utf7_to_utf8(const char *u7, size_t u7len, char **u8, size_t *u8len)
{
  int b, ch, k;

  char *buf = mutt_mem_malloc(u7len + u7len / 8 + 1);
  char *p = buf;

  for (; u7len; u7++, u7len--)
  {
    if (*u7 == '&')
    {
      u7++;
      u7len--;

      if (u7len && *u7 == '-')
      {
        *p++ = '&';
        continue;
      }

      ch = 0;
      k = 10;
      for (; u7len; u7++, u7len--)
      {
        if ((*u7 & 0x80) || (b = Index64u[(int) *u7]) == -1)
          break;
        if (k > 0)
        {
          ch |= b << k;
          k -= 6;
        }
        else
        {
          ch |= b >> (-k);
          if (ch < 0x80)
          {
            if (0x20 <= ch && ch < 0x7f)
            {
              /* Printable US-ASCII */
              goto bail;
            }
            *p++ = ch;
          }
          else if (ch < 0x800)
          {
            *p++ = 0xc0 | (ch >> 6);
            *p++ = 0x80 | (ch & 0x3f);
          }
          else
          {
            *p++ = 0xe0 | (ch >> 12);
            *p++ = 0x80 | ((ch >> 6) & 0x3f);
            *p++ = 0x80 | (ch & 0x3f);
          }
          ch = (b << (16 + k)) & 0xffff;
          k += 10;
        }
      }
      if (ch || k < 6)
      {
        /* Non-zero or too many extra bits */
        goto bail;
      }
      if (!u7len || *u7 != '-')
      {
        /* BASE64 not properly terminated */
        goto bail;
      }
      if (u7len > 2 && u7[1] == '&' && u7[2] != '-')
      {
        /* Adjacent BASE64 sections */
        goto bail;
      }
    }
    else if (*u7 < 0x20 || *u7 >= 0x7f)
    {
      /* Not printable US-ASCII */
      goto bail;
    }
    else
      *p++ = *u7;
  }
  *p++ = '\0';
  if (u8len)
    *u8len = p - buf;

  mutt_mem_realloc(&buf, p - buf);
  if (u8)
    *u8 = buf;
  return buf;

bail:
  FREE(&buf);
  return NULL;
}

/**
 * utf8_to_utf7 - Convert data from UTF-8 to RFC2060's UTF-7
 * @param[in]  u8    UTF-8 data
 * @param[in]  u8len Length of UTF-8 data
 * @param[out] u7    Save the UTF-7 data pointer
 * @param[out] u7len Save the UTF-7 data length
 * @retval ptr  UTF-7 data
 * @retval NULL Error
 *
 * Unicode characters above U+FFFF are replaced by U+FFFE.
 *
 * @note The result is null-terminated.
 * @note The caller must free() the returned data.
 */
static char *utf8_to_utf7(const char *u8, size_t u8len, char **u7, size_t *u7len)
{
  int ch;
  int n, b = 0, k = 0;
  bool base64 = false;

  /*
   * In the worst case we convert 2 chars to 7 chars. For example:
   * "\x10&\x10&..." -> "&ABA-&-&ABA-&-...".
   */
  char *buf = mutt_mem_malloc((u8len / 2) * 7 + 6);
  char *p = buf;

  while (u8len)
  {
    unsigned char c = *u8;

    if (c < 0x80)
    {
      ch = c;
      n = 0;
    }
    else if (c < 0xc2)
      goto bail;
    else if (c < 0xe0)
    {
      ch = c & 0x1f;
      n = 1;
    }
    else if (c < 0xf0)
    {
      ch = c & 0x0f;
      n = 2;
    }
    else if (c < 0xf8)
    {
      ch = c & 0x07;
      n = 3;
    }
    else if (c < 0xfc)
    {
      ch = c & 0x03;
      n = 4;
    }
    else if (c < 0xfe)
    {
      ch = c & 0x01;
      n = 5;
    }
    else
      goto bail;

    u8++;
    u8len--;
    if (n > u8len)
      goto bail;
    for (int i = 0; i < n; i++)
    {
      if ((u8[i] & 0xc0) != 0x80)
        goto bail;
      ch = (ch << 6) | (u8[i] & 0x3f);
    }
    if (n > 1 && !(ch >> (n * 5 + 1)))
      goto bail;
    u8 += n;
    u8len -= n;

    if (ch < 0x20 || ch >= 0x7f)
    {
      if (!base64)
      {
        *p++ = '&';
        base64 = true;
        b = 0;
        k = 10;
      }
      if (ch & ~0xffff)
        ch = 0xfffe;
      *p++ = B64Chars[b | ch >> k];
      k -= 6;
      for (; k >= 0; k -= 6)
        *p++ = B64Chars[(ch >> k) & 0x3f];
      b = (ch << (-k)) & 0x3f;
      k += 16;
    }
    else
    {
      if (base64)
      {
        if (k > 10)
          *p++ = B64Chars[b];
        *p++ = '-';
        base64 = false;
      }
      *p++ = ch;
      if (ch == '&')
        *p++ = '-';
    }
  }

  if (base64)
  {
    if (k > 10)
      *p++ = B64Chars[b];
    *p++ = '-';
  }

  *p++ = '\0';
  if (u7len)
    *u7len = p - buf;
  mutt_mem_realloc(&buf, p - buf);
  if (u7)
    *u7 = buf;
  return buf;

bail:
  FREE(&buf);
  return NULL;
}

/**
 * imap_utf_encode - Encode email from local charset to UTF-8
 * @param idata Server data
 * @param s     Email to convert
 */
void imap_utf_encode(struct ImapData *idata, char **s)
{
  if (!Charset || !s)
    return;

  char *t = mutt_str_strdup(*s);
  if (t && (mutt_ch_convert_string(&t, Charset, "utf-8", 0) == 0))
  {
    FREE(s);
    if (idata->unicode)
      *s = mutt_str_strdup(t);
    else
      *s = utf8_to_utf7(t, strlen(t), NULL, 0);
  }
  FREE(&t);
}

/**
 * imap_utf_decode - Decode email from UTF-8 to local charset
 * @param[in]  idata Server data
 * @param[out] s     Email to convert
 */
void imap_utf_decode(struct ImapData *idata, char **s)
{
  if (!Charset)
    return;

  char *t = NULL;

  if (idata->unicode)
    t = mutt_str_strdup(*s);
  else
    t = utf7_to_utf8(*s, strlen(*s), 0, 0);

  if (t && mutt_ch_convert_string(&t, "utf-8", Charset, 0) == 0)
  {
    FREE(s);
    *s = t;
  }
  else
    FREE(&t);
}
