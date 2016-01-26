/*
 * Copyright (C) 1996-2016 The Squid Software Foundation and contributors
 *
 * Squid software is distributed under GPLv2+ license and includes
 * contributions from numerous individuals and organizations.
 * Please see the COPYING and CONTRIBUTORS files for details.
 */

#ifndef SQUID_SRC_SECURITY_SESSION_H
#define SQUID_SRC_SECURITY_SESSION_H

// LockingPointer.h instead of TidyPointer.h for CtoCpp1()
#include "security/LockingPointer.h"

#if USE_OPENSSL
#if HAVE_OPENSSL_SSL_H
#include <openssl/ssl.h>
#endif
#endif

#if USE_GNUTLS
#if HAVE_GNUTLS_GNUTLS_H
#include <gnutls/gnutls.h>
#endif
#endif

/*
 * NOTE: we use TidyPointer for sessions. OpenSSL provides explicit reference
 * locking mechanisms, but GnuTLS only provides init/deinit. To ensure matching
 * behaviour we cannot use LockingPointer (yet) and must ensure that there is
 * no possibility of double-free being used on the raw pointers. That is
 * currently done by using a TidyPointer in the global fde table so its
 * lifetime matched the connection.
 */

namespace Security {

#if USE_OPENSSL
typedef SSL* SessionPtr;
CtoCpp1(SSL_free, SSL *);
typedef TidyPointer<SSL, Security::SSL_free_cpp> SessionPointer;

#elif USE_GNUTLS
typedef gnutls_session_t SessionPtr;
CtoCpp1(gnutls_deinit, gnutls_session_t);
typedef TidyPointer<struct gnutls_session_int, Security::gnutls_deinit_cpp> SessionPointer;

#else
// use void* so we can check against NULL
typedef void* SessionPtr;
typedef TidyPointer<void, nullptr> SessionPointer;

#endif

} // namespace Security

#endif /* SQUID_SRC_SECURITY_SESSION_H */

