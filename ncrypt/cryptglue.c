/**
 * @file
 * Wrapper around crypto functions
 *
 * @authors
 * Copyright (C) 2003 Werner Koch <wk@gnupg.org>
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

/**
 * @page crypt_cryptglue Wrapper around crypto functions
 *
 * This file dispatches the generic crypto functions to the implemented
 * backend or provides dummy stubs.
 *
 * @note Some generic functions are handled in crypt.c
 *
 * @note This file has been changed to make use of the new module system.
 * Consequently there's a 1:1 mapping between the functions contained in this
 * file and the functions implemented by the crypto modules.
 */

#include "config.h"
#include <stdbool.h>
#include <stdio.h>
#include "mutt/mutt.h"
#include "crypt_mod.h"
#include "ncrypt.h"
#include "options.h"
#include "protos.h"

struct State;

#ifdef CRYPT_BACKEND_CLASSIC_PGP
extern struct CryptModuleSpecs crypt_mod_pgp_classic;
#endif

#ifdef CRYPT_BACKEND_CLASSIC_SMIME
extern struct CryptModuleSpecs crypt_mod_smime_classic;
#endif

#ifdef CRYPT_BACKEND_GPGME
extern struct CryptModuleSpecs crypt_mod_pgp_gpgme;
extern struct CryptModuleSpecs crypt_mod_smime_gpgme;
#endif

/* If the crypto module identifier by IDENTIFIER has been registered,
 * call its function FUNC.  Do nothing else.  This may be used as an
 * expression. */
#define CRYPT_MOD_CALL_CHECK(identifier, func)                                 \
  (crypto_module_lookup(APPLICATION_##identifier) &&                           \
   (crypto_module_lookup(APPLICATION_##identifier))->func)

/* Call the function FUNC in the crypto module identified by
 * IDENTIFIER. This may be used as an expression. */
#define CRYPT_MOD_CALL(identifier, func)                                       \
  *(crypto_module_lookup(APPLICATION_##identifier))->func

/**
 * crypt_init - Initialise the crypto backends
 *
 * This calls CryptModuleSpecs::init()
 */
void crypt_init(void)
{
#ifdef CRYPT_BACKEND_CLASSIC_PGP
  if (
#ifdef CRYPT_BACKEND_GPGME
      (!CryptUseGpgme)
#else
      1
#endif
  )
    crypto_module_register(&crypt_mod_pgp_classic);
#endif

#ifdef CRYPT_BACKEND_CLASSIC_SMIME
  if (
#ifdef CRYPT_BACKEND_GPGME
      (!CryptUseGpgme)
#else
      1
#endif
  )
    crypto_module_register(&crypt_mod_smime_classic);
#endif

  if (CryptUseGpgme)
  {
#ifdef CRYPT_BACKEND_GPGME
    crypto_module_register(&crypt_mod_pgp_gpgme);
    crypto_module_register(&crypt_mod_smime_gpgme);
#else
    mutt_message(_("\"crypt_use_gpgme\" set"
                   " but not built with GPGME support."));
    if (mutt_any_key_to_continue(NULL) == -1)
      mutt_exit(1);
#endif
  }

#if defined(CRYPT_BACKEND_CLASSIC_PGP) ||                                      \
    defined(CRYPT_BACKEND_CLASSIC_SMIME) || defined(CRYPT_BACKEND_GPGME)
  if (CRYPT_MOD_CALL_CHECK(PGP, init))
    (CRYPT_MOD_CALL(PGP, init))();

  if (CRYPT_MOD_CALL_CHECK(SMIME, init))
    (CRYPT_MOD_CALL(SMIME, init))();
#endif
}

/**
 * crypt_invoke_message - Display an informative message
 * @param type Crypto type, e.g. #APPLICATION_PGP
 *
 * Show a message that a backend will be invoked.
 */
void crypt_invoke_message(int type)
{
  if (((WithCrypto & APPLICATION_PGP) != 0) && (type & APPLICATION_PGP))
    mutt_message(_("Invoking PGP..."));
  else if (((WithCrypto & APPLICATION_SMIME) != 0) && (type & APPLICATION_SMIME))
    mutt_message(_("Invoking S/MIME..."));
}

/**
 * crypt_has_module_backend - Is there a crypto backend for a given type?
 * @param type Crypto type, e.g. #APPLICATION_PGP
 * @retval true  Backend is present
 * @retval false Backend is not present
 */
bool crypt_has_module_backend(int type)
{
  if (((WithCrypto & APPLICATION_PGP) != 0) && (type & APPLICATION_PGP) &&
      crypto_module_lookup(APPLICATION_PGP))
  {
    return true;
  }

  if (((WithCrypto & APPLICATION_SMIME) != 0) && (type & APPLICATION_SMIME) &&
      crypto_module_lookup(APPLICATION_SMIME))
  {
    return true;
  }

  return false;
}

/**
 * crypt_pgp_void_passphrase - Wrapper for CryptModuleSpecs::void_passphrase()
 */
void crypt_pgp_void_passphrase(void)
{
  if (CRYPT_MOD_CALL_CHECK(PGP, void_passphrase))
    (CRYPT_MOD_CALL(PGP, void_passphrase))();
}

/**
 * crypt_pgp_valid_passphrase - Wrapper for CryptModuleSpecs::valid_passphrase()
 */
int crypt_pgp_valid_passphrase(void)
{
  if (CRYPT_MOD_CALL_CHECK(PGP, valid_passphrase))
    return (CRYPT_MOD_CALL(PGP, valid_passphrase))();

  return 0;
}

/**
 * crypt_pgp_decrypt_mime - Wrapper for CryptModuleSpecs::decrypt_mime()
 */
int crypt_pgp_decrypt_mime(FILE *a, FILE **b, struct Body *c, struct Body **d)
{
  if (CRYPT_MOD_CALL_CHECK(PGP, decrypt_mime))
    return (CRYPT_MOD_CALL(PGP, decrypt_mime))(a, b, c, d);

  return -1;
}

/**
 * crypt_pgp_application_handler - Wrapper for CryptModuleSpecs::application_handler()
 */
int crypt_pgp_application_handler(struct Body *m, struct State *s)
{
  if (CRYPT_MOD_CALL_CHECK(PGP, application_handler))
    return (CRYPT_MOD_CALL(PGP, application_handler))(m, s);

  return -1;
}

/**
 * crypt_pgp_encrypted_handler - Wrapper for CryptModuleSpecs::encrypted_handler()
 */
int crypt_pgp_encrypted_handler(struct Body *a, struct State *s)
{
  if (CRYPT_MOD_CALL_CHECK(PGP, encrypted_handler))
    return (CRYPT_MOD_CALL(PGP, encrypted_handler))(a, s);

  return -1;
}

/**
 * crypt_pgp_invoke_getkeys - Wrapper for CryptModuleSpecs::pgp_invoke_getkeys()
 */
void crypt_pgp_invoke_getkeys(struct Address *addr)
{
  if (CRYPT_MOD_CALL_CHECK(PGP, pgp_invoke_getkeys))
    (CRYPT_MOD_CALL(PGP, pgp_invoke_getkeys))(addr);
}

/**
 * crypt_pgp_check_traditional - Wrapper for CryptModuleSpecs::pgp_check_traditional()
 */
int crypt_pgp_check_traditional(FILE *fp, struct Body *b, bool just_one)
{
  if (CRYPT_MOD_CALL_CHECK(PGP, pgp_check_traditional))
    return (CRYPT_MOD_CALL(PGP, pgp_check_traditional))(fp, b, just_one);

  return 0;
}

/**
 * crypt_pgp_traditional_encryptsign - Wrapper for CryptModuleSpecs::pgp_traditional_encryptsign()
 */
struct Body *crypt_pgp_traditional_encryptsign(struct Body *a, int flags, char *keylist)
{
  if (CRYPT_MOD_CALL_CHECK(PGP, pgp_traditional_encryptsign))
    return (CRYPT_MOD_CALL(PGP, pgp_traditional_encryptsign))(a, flags, keylist);

  return NULL;
}

/**
 * crypt_pgp_make_key_attachment - Wrapper for CryptModuleSpecs::pgp_make_key_attachment()
 */
struct Body *crypt_pgp_make_key_attachment(void)
{
  if (CRYPT_MOD_CALL_CHECK(PGP, pgp_make_key_attachment))
    return (CRYPT_MOD_CALL(PGP, pgp_make_key_attachment))();

  return NULL;
}

/**
 * crypt_pgp_find_keys - Wrapper for CryptModuleSpecs::find_keys()
 */
char *crypt_pgp_find_keys(struct Address *addrlist, bool oppenc_mode)
{
  if (CRYPT_MOD_CALL_CHECK(PGP, find_keys))
    return (CRYPT_MOD_CALL(PGP, find_keys))(addrlist, oppenc_mode);

  return NULL;
}

/**
 * crypt_pgp_sign_message - Wrapper for CryptModuleSpecs::sign_message()
 */
struct Body *crypt_pgp_sign_message(struct Body *a)
{
  if (CRYPT_MOD_CALL_CHECK(PGP, sign_message))
    return (CRYPT_MOD_CALL(PGP, sign_message))(a);

  return NULL;
}

/**
 * crypt_pgp_encrypt_message - Wrapper for CryptModuleSpecs::pgp_encrypt_message()
 */
struct Body *crypt_pgp_encrypt_message(struct Body *a, char *keylist, bool sign)
{
  if (CRYPT_MOD_CALL_CHECK(PGP, pgp_encrypt_message))
    return (CRYPT_MOD_CALL(PGP, pgp_encrypt_message))(a, keylist, sign);

  return NULL;
}

/**
 * crypt_pgp_invoke_import - Wrapper for CryptModuleSpecs::pgp_invoke_import()
 */
void crypt_pgp_invoke_import(const char *fname)
{
  if (CRYPT_MOD_CALL_CHECK(PGP, pgp_invoke_import))
    (CRYPT_MOD_CALL(PGP, pgp_invoke_import))(fname);
}

/**
 * crypt_pgp_verify_one - Wrapper for CryptModuleSpecs::verify_one()
 */
int crypt_pgp_verify_one(struct Body *sigbdy, struct State *s, const char *tempf)
{
  if (CRYPT_MOD_CALL_CHECK(PGP, verify_one))
    return (CRYPT_MOD_CALL(PGP, verify_one))(sigbdy, s, tempf);

  return -1;
}

/**
 * crypt_pgp_send_menu - Wrapper for CryptModuleSpecs::send_menu()
 */
int crypt_pgp_send_menu(struct Header *msg)
{
  if (CRYPT_MOD_CALL_CHECK(PGP, send_menu))
    return (CRYPT_MOD_CALL(PGP, send_menu))(msg);

  return 0;
}

/**
 * crypt_pgp_extract_key_from_attachment - Wrapper for CryptModuleSpecs::pgp_extract_key_from_attachment()
 */
void crypt_pgp_extract_key_from_attachment(FILE *fp, struct Body *top)
{
  if (CRYPT_MOD_CALL_CHECK(PGP, pgp_extract_key_from_attachment))
    (CRYPT_MOD_CALL(PGP, pgp_extract_key_from_attachment))(fp, top);
}

/**
 * crypt_pgp_set_sender - Wrapper for CryptModuleSpecs::set_sender()
 */
void crypt_pgp_set_sender(const char *sender)
{
  if (CRYPT_MOD_CALL_CHECK(PGP, set_sender))
    (CRYPT_MOD_CALL(PGP, set_sender))(sender);
}

/**
 * crypt_smime_void_passphrase - Wrapper for CryptModuleSpecs::void_passphrase()
 */
void crypt_smime_void_passphrase(void)
{
  if (CRYPT_MOD_CALL_CHECK(SMIME, void_passphrase))
    (CRYPT_MOD_CALL(SMIME, void_passphrase))();
}

/**
 * crypt_smime_valid_passphrase - Wrapper for CryptModuleSpecs::valid_passphrase()
 */
int crypt_smime_valid_passphrase(void)
{
  if (CRYPT_MOD_CALL_CHECK(SMIME, valid_passphrase))
    return (CRYPT_MOD_CALL(SMIME, valid_passphrase))();

  return 0;
}

/**
 * crypt_smime_decrypt_mime - Wrapper for CryptModuleSpecs::decrypt_mime()
 */
int crypt_smime_decrypt_mime(FILE *a, FILE **b, struct Body *c, struct Body **d)
{
  if (CRYPT_MOD_CALL_CHECK(SMIME, decrypt_mime))
    return (CRYPT_MOD_CALL(SMIME, decrypt_mime))(a, b, c, d);

  return -1;
}

/**
 * crypt_smime_application_handler - Wrapper for CryptModuleSpecs::application_handler()
 */
int crypt_smime_application_handler(struct Body *m, struct State *s)
{
  if (CRYPT_MOD_CALL_CHECK(SMIME, application_handler))
    return (CRYPT_MOD_CALL(SMIME, application_handler))(m, s);

  return -1;
}

/**
 * crypt_smime_encrypted_handler - Wrapper for CryptModuleSpecs::encrypted_handler()
 */
void crypt_smime_encrypted_handler(struct Body *a, struct State *s)
{
  if (CRYPT_MOD_CALL_CHECK(SMIME, encrypted_handler))
    (CRYPT_MOD_CALL(SMIME, encrypted_handler))(a, s);
}

/**
 * crypt_smime_getkeys - Wrapper for CryptModuleSpecs::smime_getkeys()
 */
void crypt_smime_getkeys(struct Envelope *env)
{
  if (CRYPT_MOD_CALL_CHECK(SMIME, smime_getkeys))
    (CRYPT_MOD_CALL(SMIME, smime_getkeys))(env);
}

/**
 * crypt_smime_verify_sender - Wrapper for CryptModuleSpecs::smime_verify_sender()
 */
int crypt_smime_verify_sender(struct Header *h)
{
  if (CRYPT_MOD_CALL_CHECK(SMIME, smime_verify_sender))
    return (CRYPT_MOD_CALL(SMIME, smime_verify_sender))(h);

  return 1;
}

/**
 * crypt_smime_find_keys - Wrapper for CryptModuleSpecs::find_keys()
 */
char *crypt_smime_find_keys(struct Address *addrlist, bool oppenc_mode)
{
  if (CRYPT_MOD_CALL_CHECK(SMIME, find_keys))
    return (CRYPT_MOD_CALL(SMIME, find_keys))(addrlist, oppenc_mode);

  return NULL;
}

/**
 * crypt_smime_sign_message - Wrapper for CryptModuleSpecs::sign_message()
 */
struct Body *crypt_smime_sign_message(struct Body *a)
{
  if (CRYPT_MOD_CALL_CHECK(SMIME, sign_message))
    return (CRYPT_MOD_CALL(SMIME, sign_message))(a);

  return NULL;
}

/**
 * crypt_smime_build_smime_entity - Wrapper for CryptModuleSpecs::smime_build_smime_entity()
 */
struct Body *crypt_smime_build_smime_entity(struct Body *a, char *certlist)
{
  if (CRYPT_MOD_CALL_CHECK(SMIME, smime_build_smime_entity))
    return (CRYPT_MOD_CALL(SMIME, smime_build_smime_entity))(a, certlist);

  return NULL;
}

/**
 * crypt_smime_invoke_import - Wrapper for CryptModuleSpecs::smime_invoke_import()
 */
void crypt_smime_invoke_import(char *infile, char *mailbox)
{
  if (CRYPT_MOD_CALL_CHECK(SMIME, smime_invoke_import))
    (CRYPT_MOD_CALL(SMIME, smime_invoke_import))(infile, mailbox);
}

/**
 * crypt_smime_verify_one - Wrapper for CryptModuleSpecs::verify_one()
 */
int crypt_smime_verify_one(struct Body *sigbdy, struct State *s, const char *tempf)
{
  if (CRYPT_MOD_CALL_CHECK(SMIME, verify_one))
    return (CRYPT_MOD_CALL(SMIME, verify_one))(sigbdy, s, tempf);

  return -1;
}

/**
 * crypt_smime_send_menu - Wrapper for CryptModuleSpecs::send_menu()
 */
int crypt_smime_send_menu(struct Header *msg)
{
  if (CRYPT_MOD_CALL_CHECK(SMIME, send_menu))
    return (CRYPT_MOD_CALL(SMIME, send_menu))(msg);

  return 0;
}

/**
 * crypt_smime_set_sender - Wrapper for CryptModuleSpecs::set_sender()
 */
void crypt_smime_set_sender(const char *sender)
{
  if (CRYPT_MOD_CALL_CHECK(SMIME, set_sender))
    (CRYPT_MOD_CALL(SMIME, set_sender))(sender);
}
