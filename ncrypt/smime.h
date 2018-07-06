/**
 * @file
 * SMIME helper routines
 *
 * @authors
 * Copyright (C) 2001-2002 Oliver Ehli <elmy@acm.org>
 * Copyright (C) 2004 g10 Code GmbH
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

#ifndef _NCRYPT_SMIME_H
#define _NCRYPT_SMIME_H

#ifdef CRYPT_BACKEND_CLASSIC_SMIME

#include <stdbool.h>
#include <stdio.h>

struct Address;
struct Body;
struct Envelope;
struct Header;
struct State;

/**
 * struct SmimeKey - An SIME key
 */
struct SmimeKey
{
  char *email;
  char *hash;
  char *label;
  char *issuer;
  char trust; /**< i=Invalid r=revoked e=expired u=unverified v=verified t=trusted */
  int flags;
  struct SmimeKey *next;
};

int          smime_class_application_handler(struct Body *m, struct State *s);
struct Body *smime_class_build_smime_entity(struct Body *a, char *certlist);
int          smime_class_decrypt_mime(FILE *fpin, FILE **fpout, struct Body *b, struct Body **cur);
char *       smime_class_find_keys(struct Address *addrlist, bool oppenc_mode);
void         smime_class_getkeys(struct Envelope *env);
void         smime_class_invoke_import(char *infile, char *mailbox);
int          smime_class_send_menu(struct Header *msg);
struct Body *smime_class_sign_message(struct Body *a);
int          smime_class_valid_passphrase(void);
int          smime_class_verify_one(struct Body *sigbdy, struct State *s, const char *tempfile);
int          smime_class_verify_sender(struct Header *h);
void         smime_class_void_passphrase(void);

#endif

#endif /* _NCRYPT_SMIME_H */
