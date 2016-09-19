/*
 * Copyright (C) 1996-2016 The Squid Software Foundation and contributors
 *
 * Squid software is distributed under GPLv2+ license and includes
 * contributions from numerous individuals and organizations.
 * Please see the COPYING and CONTRIBUTORS files for details.
 */

/* DEBUG: section 83    SSL accelerator support */

#include "squid.h"

/* MS Visual Studio Projects are monolithic, so we need the following
 * #if to exclude the SSL code from compile process when not needed.
 */
#if USE_OPENSSL

#include "acl/FilledChecklist.h"
#include "anyp/PortCfg.h"
#include "fatal.h"
#include "fd.h"
#include "fde.h"
#include "globals.h"
#include "ipc/MemMap.h"
#include "security/CertError.h"
#include "SquidConfig.h"
#include "SquidTime.h"
#include "ssl/bio.h"
#include "ssl/Config.h"
#include "ssl/ErrorDetail.h"
#include "ssl/gadgets.h"
#include "ssl/support.h"
#include "URL.h"

#include <cerrno>

// TODO: Move ssl_ex_index_* global variables from global.cc here.
int ssl_ex_index_ssl_untrusted_chain = -1;

Ipc::MemMap *Ssl::SessionCache = NULL;
const char *Ssl::SessionCacheName = "ssl_session_cache";

static Ssl::CertsIndexedList SquidUntrustedCerts;

const EVP_MD *Ssl::DefaultSignHash = NULL;

const char *Ssl::BumpModeStr[] = {
    "none",
    "client-first",
    "server-first",
    "peek",
    "stare",
    "bump",
    "splice",
    "terminate",
    /*"err",*/
    NULL
};

/**
 \defgroup ServerProtocolSSLInternal Server-Side SSL Internals
 \ingroup ServerProtocolSSLAPI
 */

/// \ingroup ServerProtocolSSLInternal
static int
ssl_ask_password_cb(char *buf, int size, int rwflag, void *userdata)
{
    FILE *in;
    int len = 0;
    char cmdline[1024];

    snprintf(cmdline, sizeof(cmdline), "\"%s\" \"%s\"", Config.Program.ssl_password, (const char *)userdata);
    in = popen(cmdline, "r");

    if (fgets(buf, size, in))

        len = strlen(buf);

    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
        --len;

    buf[len] = '\0';

    pclose(in);

    return len;
}

/// \ingroup ServerProtocolSSLInternal
static void
ssl_ask_password(SSL_CTX * context, const char * prompt)
{
    if (Config.Program.ssl_password) {
        SSL_CTX_set_default_passwd_cb(context, ssl_ask_password_cb);
        SSL_CTX_set_default_passwd_cb_userdata(context, (void *)prompt);
    }
}

/// \ingroup ServerProtocolSSLInternal
static RSA *
ssl_temp_rsa_cb(SSL * ssl, int anInt, int keylen)
{
    static RSA *rsa_512 = NULL;
    static RSA *rsa_1024 = NULL;
    RSA *rsa = NULL;
    int newkey = 0;

    switch (keylen) {

    case 512:

        if (!rsa_512) {
            rsa_512 = RSA_generate_key(512, RSA_F4, NULL, NULL);
            newkey = 1;
        }

        rsa = rsa_512;
        break;

    case 1024:

        if (!rsa_1024) {
            rsa_1024 = RSA_generate_key(1024, RSA_F4, NULL, NULL);
            newkey = 1;
        }

        rsa = rsa_1024;
        break;

    default:
        debugs(83, DBG_IMPORTANT, "ssl_temp_rsa_cb: Unexpected key length " << keylen);
        return NULL;
    }

    if (rsa == NULL) {
        debugs(83, DBG_IMPORTANT, "ssl_temp_rsa_cb: Failed to generate key " << keylen);
        return NULL;
    }

    if (newkey) {
        if (Debug::Enabled(83, 5))
            PEM_write_RSAPrivateKey(debug_log, rsa, NULL, NULL, 0, NULL, NULL);

        debugs(83, DBG_IMPORTANT, "Generated ephemeral RSA key of length " << keylen);
    }

    return rsa;
}

int Ssl::asn1timeToString(ASN1_TIME *tm, char *buf, int len)
{
    BIO *bio;
    int write = 0;
    bio = BIO_new(BIO_s_mem());
    if (bio) {
        if (ASN1_TIME_print(bio, tm))
            write = BIO_read(bio, buf, len-1);
        BIO_free(bio);
    }
    buf[write]='\0';
    return write;
}

int Ssl::matchX509CommonNames(X509 *peer_cert, void *check_data, int (*check_func)(void *check_data,  ASN1_STRING *cn_data))
{
    assert(peer_cert);

    X509_NAME *name = X509_get_subject_name(peer_cert);

    for (int i = X509_NAME_get_index_by_NID(name, NID_commonName, -1); i >= 0; i = X509_NAME_get_index_by_NID(name, NID_commonName, i)) {

        ASN1_STRING *cn_data = X509_NAME_ENTRY_get_data(X509_NAME_get_entry(name, i));

        if ( (*check_func)(check_data, cn_data) == 0)
            return 1;
    }

    STACK_OF(GENERAL_NAME) * altnames;
    altnames = (STACK_OF(GENERAL_NAME)*)X509_get_ext_d2i(peer_cert, NID_subject_alt_name, NULL, NULL);

    if (altnames) {
        int numalts = sk_GENERAL_NAME_num(altnames);
        for (int i = 0; i < numalts; ++i) {
            const GENERAL_NAME *check = sk_GENERAL_NAME_value(altnames, i);
            if (check->type != GEN_DNS) {
                continue;
            }
            ASN1_STRING *cn_data = check->d.dNSName;

            if ( (*check_func)(check_data, cn_data) == 0) {
                sk_GENERAL_NAME_pop_free(altnames, GENERAL_NAME_free);
                return 1;
            }
        }
        sk_GENERAL_NAME_pop_free(altnames, GENERAL_NAME_free);
    }
    return 0;
}

static int check_domain( void *check_data, ASN1_STRING *cn_data)
{
    char cn[1024];
    const char *server = (const char *)check_data;

    if (cn_data->length == 0)
        return 1; // zero length cn, ignore

    if (cn_data->length > (int)sizeof(cn) - 1)
        return 1; //if does not fit our buffer just ignore

    char *s = reinterpret_cast<char*>(cn_data->data);
    char *d = cn;
    for (int i = 0; i < cn_data->length; ++i, ++d, ++s) {
        if (*s == '\0')
            return 1; // always a domain mismatch. contains 0x00
        *d = *s;
    }
    cn[cn_data->length] = '\0';
    debugs(83, 4, "Verifying server domain " << server << " to certificate name/subjectAltName " << cn);
    return matchDomainName(server, (cn[0] == '*' ? cn + 1 : cn), mdnRejectSubsubDomains);
}

bool Ssl::checkX509ServerValidity(X509 *cert, const char *server)
{
    return matchX509CommonNames(cert, (void *)server, check_domain);
}

/// \ingroup ServerProtocolSSLInternal
static int
ssl_verify_cb(int ok, X509_STORE_CTX * ctx)
{
    // preserve original ctx->error before SSL_ calls can overwrite it
    Security::ErrorCode error_no = ok ? SSL_ERROR_NONE : ctx->error;

    char buffer[256] = "";
    SSL *ssl = (SSL *)X509_STORE_CTX_get_ex_data(ctx, SSL_get_ex_data_X509_STORE_CTX_idx());
    Security::ContextPtr sslctx = SSL_get_SSL_CTX(ssl);
    SBuf *server = (SBuf *)SSL_get_ex_data(ssl, ssl_ex_index_server);
    void *dont_verify_domain = SSL_CTX_get_ex_data(sslctx, ssl_ctx_ex_index_dont_verify_domain);
    ACLChecklist *check = (ACLChecklist*)SSL_get_ex_data(ssl, ssl_ex_index_cert_error_check);
    X509 *peeked_cert = (X509 *)SSL_get_ex_data(ssl, ssl_ex_index_ssl_peeked_cert);
    Security::CertPointer peer_cert;
    peer_cert.resetAndLock(ctx->cert);

    X509_NAME_oneline(X509_get_subject_name(peer_cert.get()), buffer, sizeof(buffer));

    // detect infinite loops
    uint32_t *validationCounter = static_cast<uint32_t *>(SSL_get_ex_data(ssl, ssl_ex_index_ssl_validation_counter));
    if (!validationCounter) {
        validationCounter = new uint32_t(1);
        SSL_set_ex_data(ssl, ssl_ex_index_ssl_validation_counter, validationCounter);
    } else {
        // overflows allowed if SQUID_CERT_VALIDATION_ITERATION_MAX >= UINT32_MAX
        (*validationCounter)++;
    }

    if ((*validationCounter) >= SQUID_CERT_VALIDATION_ITERATION_MAX) {
        ok = 0; // or the validation loop will never stop
        error_no = SQUID_X509_V_ERR_INFINITE_VALIDATION;
        debugs(83, 2, "SQUID_X509_V_ERR_INFINITE_VALIDATION: " <<
               *validationCounter << " iterations while checking " << buffer);
    }

    if (ok) {
        debugs(83, 5, "SSL Certificate signature OK: " << buffer);

        // Check for domain mismatch only if the current certificate is the peer certificate.
        if (!dont_verify_domain && server && peer_cert.get() == X509_STORE_CTX_get_current_cert(ctx)) {
            if (!Ssl::checkX509ServerValidity(peer_cert.get(), server->c_str())) {
                debugs(83, 2, "SQUID_X509_V_ERR_DOMAIN_MISMATCH: Certificate " << buffer << " does not match domainname " << server);
                ok = 0;
                error_no = SQUID_X509_V_ERR_DOMAIN_MISMATCH;
            }
        }
    }

    if (ok && peeked_cert) {
        // Check whether the already peeked certificate matches the new one.
        if (X509_cmp(peer_cert.get(), peeked_cert) != 0) {
            debugs(83, 2, "SQUID_X509_V_ERR_CERT_CHANGE: Certificate " << buffer << " does not match peeked certificate");
            ok = 0;
            error_no =  SQUID_X509_V_ERR_CERT_CHANGE;
        }
    }

    if (!ok) {
        Security::CertPointer broken_cert;
        broken_cert.resetAndLock(X509_STORE_CTX_get_current_cert(ctx));
        if (!broken_cert)
            broken_cert = peer_cert;

        Security::CertErrors *errs = static_cast<Security::CertErrors *>(SSL_get_ex_data(ssl, ssl_ex_index_ssl_errors));
        const int depth = X509_STORE_CTX_get_error_depth(ctx);
        if (!errs) {
            errs = new Security::CertErrors(Security::CertError(error_no, broken_cert, depth));
            if (!SSL_set_ex_data(ssl, ssl_ex_index_ssl_errors,  (void *)errs)) {
                debugs(83, 2, "Failed to set ssl error_no in ssl_verify_cb: Certificate " << buffer);
                delete errs;
                errs = NULL;
            }
        } else // remember another error number
            errs->push_back_unique(Security::CertError(error_no, broken_cert, depth));

        if (const char *err_descr = Ssl::GetErrorDescr(error_no))
            debugs(83, 5, err_descr << ": " << buffer);
        else
            debugs(83, DBG_IMPORTANT, "SSL unknown certificate error " << error_no << " in " << buffer);

        // Check if the certificate error can be bypassed.
        // Infinity validation loop errors can not bypassed.
        if (error_no != SQUID_X509_V_ERR_INFINITE_VALIDATION) {
            if (check) {
                ACLFilledChecklist *filledCheck = Filled(check);
                assert(!filledCheck->sslErrors);
                filledCheck->sslErrors = new Security::CertErrors(Security::CertError(error_no, broken_cert));
                filledCheck->serverCert = peer_cert;
                if (check->fastCheck() == ACCESS_ALLOWED) {
                    debugs(83, 3, "bypassing SSL error " << error_no << " in " << buffer);
                    ok = 1;
                } else {
                    debugs(83, 5, "confirming SSL error " << error_no);
                }
                delete filledCheck->sslErrors;
                filledCheck->sslErrors = NULL;
                filledCheck->serverCert.reset();
            }
            // If the certificate validator is used then we need to allow all errors and
            // pass them to certficate validator for more processing
            else if (Ssl::TheConfig.ssl_crt_validator) {
                ok = 1;
            }
        }
    }

    if (Ssl::TheConfig.ssl_crt_validator) {
        // Check if we have stored certificates chain. Store if not.
        if (!SSL_get_ex_data(ssl, ssl_ex_index_ssl_cert_chain)) {
            STACK_OF(X509) *certStack = X509_STORE_CTX_get1_chain(ctx);
            if (certStack && !SSL_set_ex_data(ssl, ssl_ex_index_ssl_cert_chain, certStack))
                sk_X509_pop_free(certStack, X509_free);
        }
    }

    if (!ok && !SSL_get_ex_data(ssl, ssl_ex_index_ssl_error_detail) ) {

        // Find the broken certificate. It may be intermediate.
        Security::CertPointer broken_cert(peer_cert); // reasonable default if search fails
        // Our SQUID_X509_V_ERR_DOMAIN_MISMATCH implies peer_cert is at fault.
        if (error_no != SQUID_X509_V_ERR_DOMAIN_MISMATCH) {
            if (auto *last_used_cert = X509_STORE_CTX_get_current_cert(ctx))
                broken_cert.resetAndLock(last_used_cert);
        }

        auto *errDetail = new Ssl::ErrorDetail(error_no, peer_cert.get(), broken_cert.get());
        if (!SSL_set_ex_data(ssl, ssl_ex_index_ssl_error_detail, errDetail)) {
            debugs(83, 2, "Failed to set Ssl::ErrorDetail in ssl_verify_cb: Certificate " << buffer);
            delete errDetail;
        }
    }

    return ok;
}

// "dup" function for SSL_get_ex_new_index("cert_err_check")
static int
ssl_dupAclChecklist(CRYPTO_EX_DATA *, CRYPTO_EX_DATA *, void *,
                    int, long, void *)
{
    // We do not support duplication of ACLCheckLists.
    // If duplication is needed, we can count copies with cbdata.
    assert(false);
    return 0;
}

// "free" function for SSL_get_ex_new_index("cert_err_check")
static void
ssl_freeAclChecklist(void *, void *ptr, CRYPTO_EX_DATA *,
                     int, long, void *)
{
    delete static_cast<ACLChecklist *>(ptr); // may be NULL
}

// "free" function for SSL_get_ex_new_index("ssl_error_detail")
static void
ssl_free_ErrorDetail(void *, void *ptr, CRYPTO_EX_DATA *,
                     int, long, void *)
{
    Ssl::ErrorDetail  *errDetail = static_cast <Ssl::ErrorDetail *>(ptr);
    delete errDetail;
}

static void
ssl_free_SslErrors(void *, void *ptr, CRYPTO_EX_DATA *,
                   int, long, void *)
{
    Security::CertErrors *errs = static_cast <Security::CertErrors*>(ptr);
    delete errs;
}

// "free" function for SSL_get_ex_new_index("ssl_ex_index_ssl_validation_counter")
static void
ssl_free_int(void *, void *ptr, CRYPTO_EX_DATA *,
             int, long, void *)
{
    uint32_t *counter = static_cast <uint32_t *>(ptr);
    delete counter;
}

/// \ingroup ServerProtocolSSLInternal
/// Callback handler function to release STACK_OF(X509) "ex" data stored
/// in an SSL object.
static void
ssl_free_CertChain(void *, void *ptr, CRYPTO_EX_DATA *,
                   int, long, void *)
{
    STACK_OF(X509) *certsChain = static_cast <STACK_OF(X509) *>(ptr);
    sk_X509_pop_free(certsChain,X509_free);
}

// "free" function for X509 certificates
static void
ssl_free_X509(void *, void *ptr, CRYPTO_EX_DATA *,
              int, long, void *)
{
    X509  *cert = static_cast <X509 *>(ptr);
    X509_free(cert);
}

// "free" function for SBuf
static void
ssl_free_SBuf(void *, void *ptr, CRYPTO_EX_DATA *,
              int, long, void *)
{
    SBuf  *buf = static_cast <SBuf *>(ptr);
    delete buf;
}

void
Ssl::Initialize(void)
{
    static bool initialized = false;
    if (initialized)
        return;
    initialized = true;

    SSL_load_error_strings();
    SSLeay_add_ssl_algorithms();

#if HAVE_OPENSSL_ENGINE_H
    if (::Config.SSL.ssl_engine) {
        ENGINE *e;
        if (!(e = ENGINE_by_id(::Config.SSL.ssl_engine)))
            fatalf("Unable to find SSL engine '%s'\n", ::Config.SSL.ssl_engine);

        if (!ENGINE_set_default(e, ENGINE_METHOD_ALL)) {
            const int ssl_error = ERR_get_error();
            fatalf("Failed to initialise SSL engine: %s\n", ERR_error_string(ssl_error, NULL));
        }
    }
#else
    if (::Config.SSL.ssl_engine)
        fatalf("Your OpenSSL has no SSL engine support\n");
#endif

    const char *defName = ::Config.SSL.certSignHash ? ::Config.SSL.certSignHash : SQUID_SSL_SIGN_HASH_IF_NONE;
    Ssl::DefaultSignHash = EVP_get_digestbyname(defName);
    if (!Ssl::DefaultSignHash)
        fatalf("Sign hash '%s' is not supported\n", defName);

    ssl_ex_index_server = SSL_get_ex_new_index(0, (void *) "server", NULL, NULL, ssl_free_SBuf);
    ssl_ctx_ex_index_dont_verify_domain = SSL_CTX_get_ex_new_index(0, (void *) "dont_verify_domain", NULL, NULL, NULL);
    ssl_ex_index_cert_error_check = SSL_get_ex_new_index(0, (void *) "cert_error_check", NULL, &ssl_dupAclChecklist, &ssl_freeAclChecklist);
    ssl_ex_index_ssl_error_detail = SSL_get_ex_new_index(0, (void *) "ssl_error_detail", NULL, NULL, &ssl_free_ErrorDetail);
    ssl_ex_index_ssl_peeked_cert  = SSL_get_ex_new_index(0, (void *) "ssl_peeked_cert", NULL, NULL, &ssl_free_X509);
    ssl_ex_index_ssl_errors =  SSL_get_ex_new_index(0, (void *) "ssl_errors", NULL, NULL, &ssl_free_SslErrors);
    ssl_ex_index_ssl_cert_chain = SSL_get_ex_new_index(0, (void *) "ssl_cert_chain", NULL, NULL, &ssl_free_CertChain);
    ssl_ex_index_ssl_validation_counter = SSL_get_ex_new_index(0, (void *) "ssl_validation_counter", NULL, NULL, &ssl_free_int);
    ssl_ex_index_ssl_untrusted_chain = SSL_get_ex_new_index(0, (void *) "ssl_untrusted_chain", NULL, NULL, &ssl_free_CertChain);
}

#if defined(SSL3_FLAGS_NO_RENEGOTIATE_CIPHERS)
static void
ssl_info_cb(const SSL *ssl, int where, int ret)
{
    (void)ret;
    if ((where & SSL_CB_HANDSHAKE_DONE) != 0) {
        // disable renegotiation (CVE-2009-3555)
        ssl->s3->flags |= SSL3_FLAGS_NO_RENEGOTIATE_CIPHERS;
    }
}
#endif

static bool
configureSslContext(Security::ContextPtr sslContext, AnyP::PortCfg &port)
{
    int ssl_error;
    SSL_CTX_set_options(sslContext, port.secure.parsedOptions);

#if defined(SSL3_FLAGS_NO_RENEGOTIATE_CIPHERS)
    SSL_CTX_set_info_callback(sslContext, ssl_info_cb);
#endif

    if (port.sslContextSessionId)
        SSL_CTX_set_session_id_context(sslContext, (const unsigned char *)port.sslContextSessionId, strlen(port.sslContextSessionId));

    if (port.secure.parsedFlags & SSL_FLAG_NO_SESSION_REUSE) {
        SSL_CTX_set_session_cache_mode(sslContext, SSL_SESS_CACHE_OFF);
    }

    if (Config.SSL.unclean_shutdown) {
        debugs(83, 5, "Enabling quiet SSL shutdowns (RFC violation).");

        SSL_CTX_set_quiet_shutdown(sslContext, 1);
    }

    if (!port.secure.sslCipher.isEmpty()) {
        debugs(83, 5, "Using chiper suite " << port.secure.sslCipher << ".");

        if (!SSL_CTX_set_cipher_list(sslContext, port.secure.sslCipher.c_str())) {
            ssl_error = ERR_get_error();
            debugs(83, DBG_CRITICAL, "ERROR: Failed to set SSL cipher suite '" << port.secure.sslCipher << "': " << ERR_error_string(ssl_error, NULL));
            return false;
        }
    }

    debugs(83, 9, "Setting RSA key generation callback.");
    SSL_CTX_set_tmp_rsa_callback(sslContext, ssl_temp_rsa_cb);

    port.secure.updateContextEecdh(sslContext);
    port.secure.updateContextCa(sslContext);

    if (port.clientCA.get()) {
        ERR_clear_error();
        if (STACK_OF(X509_NAME) *clientca = SSL_dup_CA_list(port.clientCA.get())) {
            SSL_CTX_set_client_CA_list(sslContext, clientca);
        } else {
            ssl_error = ERR_get_error();
            debugs(83, DBG_CRITICAL, "ERROR: Failed to dupe the client CA list: " << ERR_error_string(ssl_error, NULL));
            return false;
        }

        if (port.secure.parsedFlags & SSL_FLAG_DELAYED_AUTH) {
            debugs(83, 9, "Not requesting client certificates until acl processing requires one");
            SSL_CTX_set_verify(sslContext, SSL_VERIFY_NONE, NULL);
        } else {
            debugs(83, 9, "Requiring client certificates.");
            SSL_CTX_set_verify(sslContext, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, ssl_verify_cb);
        }

        port.secure.updateContextCrl(sslContext);

    } else {
        debugs(83, 9, "Not requiring any client certificates");
        SSL_CTX_set_verify(sslContext, SSL_VERIFY_NONE, NULL);
    }

    if (port.secure.parsedFlags & SSL_FLAG_DONT_VERIFY_DOMAIN)
        SSL_CTX_set_ex_data(sslContext, ssl_ctx_ex_index_dont_verify_domain, (void *) -1);

    Ssl::SetSessionCallbacks(sslContext);

    return true;
}

bool
Ssl::InitServerContext(const Security::ContextPointer &ctx, AnyP::PortCfg &port)
{
    if (!ctx)
        return false;

    if (!SSL_CTX_use_certificate(ctx.get(), port.signingCert.get())) {
        const int ssl_error = ERR_get_error();
        const auto &keys = port.secure.certs.front();
        debugs(83, DBG_CRITICAL, "ERROR: Failed to acquire TLS certificate '" << keys.certFile << "': " << ERR_error_string(ssl_error, NULL));
        return false;
    }

    if (!SSL_CTX_use_PrivateKey(ctx.get(), port.signPkey.get())) {
        const int ssl_error = ERR_get_error();
        const auto &keys = port.secure.certs.front();
        debugs(83, DBG_CRITICAL, "ERROR: Failed to acquire TLS private key '" << keys.privateKeyFile << "': " << ERR_error_string(ssl_error, NULL));
        return false;
    }

    Ssl::addChainToSslContext(ctx.get(), port.certsToChain.get());

    /* Alternate code;
        debugs(83, DBG_IMPORTANT, "Using certificate in " << certfile);

        if (!SSL_CTX_use_certificate_chain_file(ctx.get(), certfile)) {
            ssl_error = ERR_get_error();
            debugs(83, DBG_CRITICAL, "ERROR: Failed to acquire SSL certificate '" << certfile << "': " << ERR_error_string(ssl_error, NULL));
            return false;
        }

        debugs(83, DBG_IMPORTANT, "Using private key in " << keyfile);
        ssl_ask_password(ctx.get(), keyfile);

        if (!SSL_CTX_use_PrivateKey_file(ctx.get(), keyfile, SSL_FILETYPE_PEM)) {
            ssl_error = ERR_get_error();
            debugs(83, DBG_CRITICAL, "ERROR: Failed to acquire SSL private key '" << keyfile << "': " << ERR_error_string(ssl_error, NULL));
            return false;
        }

        debugs(83, 5, "Comparing private and public SSL keys.");

        if (!SSL_CTX_check_private_key(ctx.get())) {
            ssl_error = ERR_get_error();
            debugs(83, DBG_CRITICAL, "ERROR: SSL private key '" << certfile << "' does not match public key '" <<
                   keyfile << "': " << ERR_error_string(ssl_error, NULL));
            return false;
        }
    */

    if (!configureSslContext(ctx.get(), port)) {
        debugs(83, DBG_CRITICAL, "ERROR: Configuring static SSL context");
        return false;
    }

    return true;
}

bool
Ssl::InitClientContext(Security::ContextPointer &ctx, Security::PeerOptions &peer, long options, long fl)
{
    if (!ctx)
        return false;

    SSL_CTX_set_options(ctx.get(), options);

#if defined(SSL3_FLAGS_NO_RENEGOTIATE_CIPHERS)
    SSL_CTX_set_info_callback(ctx.get(), ssl_info_cb);
#endif

    if (!peer.sslCipher.isEmpty()) {
        debugs(83, 5, "Using chiper suite " << peer.sslCipher << ".");

        const char *cipher = peer.sslCipher.c_str();
        if (!SSL_CTX_set_cipher_list(ctx.get(), cipher)) {
            const int ssl_error = ERR_get_error();
            fatalf("Failed to set SSL cipher suite '%s': %s\n",
                   cipher, ERR_error_string(ssl_error, NULL));
        }
    }

    if (!peer.certs.empty()) {
        // TODO: support loading multiple cert/key pairs
        auto &keys = peer.certs.front();
        if (!keys.certFile.isEmpty()) {
            debugs(83, DBG_IMPORTANT, "Using certificate in " << keys.certFile);

            const char *certfile = keys.certFile.c_str();
            if (!SSL_CTX_use_certificate_chain_file(ctx.get(), certfile)) {
                const int ssl_error = ERR_get_error();
                fatalf("Failed to acquire SSL certificate '%s': %s\n",
                       certfile, ERR_error_string(ssl_error, NULL));
            }

            debugs(83, DBG_IMPORTANT, "Using private key in " << keys.privateKeyFile);
            const char *keyfile = keys.privateKeyFile.c_str();
            ssl_ask_password(ctx.get(), keyfile);

            if (!SSL_CTX_use_PrivateKey_file(ctx.get(), keyfile, SSL_FILETYPE_PEM)) {
                const int ssl_error = ERR_get_error();
                fatalf("Failed to acquire SSL private key '%s': %s\n",
                       keyfile, ERR_error_string(ssl_error, NULL));
            }

            debugs(83, 5, "Comparing private and public SSL keys.");

            if (!SSL_CTX_check_private_key(ctx.get())) {
                const int ssl_error = ERR_get_error();
                fatalf("SSL private key '%s' does not match public key '%s': %s\n",
                       certfile, keyfile, ERR_error_string(ssl_error, NULL));
            }
        }
    }

    debugs(83, 9, "Setting RSA key generation callback.");
    SSL_CTX_set_tmp_rsa_callback(ctx.get(), ssl_temp_rsa_cb);

    if (fl & SSL_FLAG_DONT_VERIFY_PEER) {
        debugs(83, 2, "NOTICE: Peer certificates are not verified for validity!");
        SSL_CTX_set_verify(ctx.get(), SSL_VERIFY_NONE, NULL);
    } else {
        debugs(83, 9, "Setting certificate verification callback.");
        SSL_CTX_set_verify(ctx.get(), SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, ssl_verify_cb);
    }

    return true;
}

/// \ingroup ServerProtocolSSLInternal
int
ssl_read_method(int fd, char *buf, int len)
{
    auto ssl = fd_table[fd].ssl.get();

#if DONT_DO_THIS

    if (!SSL_is_init_finished(ssl)) {
        errno = ENOTCONN;
        return -1;
    }

#endif

    int i = SSL_read(ssl, buf, len);

    if (i > 0 && SSL_pending(ssl) > 0) {
        debugs(83, 2, "SSL FD " << fd << " is pending");
        fd_table[fd].flags.read_pending = true;
    } else
        fd_table[fd].flags.read_pending = false;

    return i;
}

/// \ingroup ServerProtocolSSLInternal
int
ssl_write_method(int fd, const char *buf, int len)
{
    auto ssl = fd_table[fd].ssl.get();
    if (!SSL_is_init_finished(ssl)) {
        errno = ENOTCONN;
        return -1;
    }

    int i = SSL_write(ssl, buf, len);
    return i;
}

void
ssl_shutdown_method(SSL *ssl)
{
    SSL_shutdown(ssl);
}

/// \ingroup ServerProtocolSSLInternal
static const char *
ssl_get_attribute(X509_NAME * name, const char *attribute_name)
{
    static char buffer[1024];
    buffer[0] = '\0';

    if (strcmp(attribute_name, "DN") == 0) {
        X509_NAME_oneline(name, buffer, sizeof(buffer));
    } else {
        int nid = OBJ_txt2nid(const_cast<char *>(attribute_name));
        if (nid == 0) {
            debugs(83, DBG_IMPORTANT, "WARNING: Unknown SSL attribute name '" << attribute_name << "'");
            return nullptr;
        }
        X509_NAME_get_text_by_NID(name, nid, buffer, sizeof(buffer));
    }

    return *buffer ? buffer : nullptr;
}

/// \ingroup ServerProtocolSSLInternal
const char *
Ssl::GetX509UserAttribute(X509 * cert, const char *attribute_name)
{
    X509_NAME *name;
    const char *ret;

    if (!cert)
        return NULL;

    name = X509_get_subject_name(cert);

    ret = ssl_get_attribute(name, attribute_name);

    return ret;
}

const char *
Ssl::GetX509Fingerprint(X509 * cert, const char *)
{
    static char buf[1024];
    if (!cert)
        return NULL;

    unsigned int n;
    unsigned char md[EVP_MAX_MD_SIZE];
    if (!X509_digest(cert, EVP_sha1(), md, &n))
        return NULL;

    assert(3 * n + 1 < sizeof(buf));

    char *s = buf;
    for (unsigned int i=0; i < n; ++i, s += 3) {
        const char term = (i + 1 < n) ? ':' : '\0';
        snprintf(s, 4, "%02X%c", md[i], term);
    }

    return buf;
}

/// \ingroup ServerProtocolSSLInternal
const char *
Ssl::GetX509CAAttribute(X509 * cert, const char *attribute_name)
{

    X509_NAME *name;
    const char *ret;

    if (!cert)
        return NULL;

    name = X509_get_issuer_name(cert);

    ret = ssl_get_attribute(name, attribute_name);

    return ret;
}

const char *sslGetUserAttribute(SSL *ssl, const char *attribute_name)
{
    if (!ssl)
        return NULL;

    X509 *cert = SSL_get_peer_certificate(ssl);

    const char *attr = Ssl::GetX509UserAttribute(cert, attribute_name);

    X509_free(cert);
    return attr;
}

const char *sslGetCAAttribute(SSL *ssl, const char *attribute_name)
{
    if (!ssl)
        return NULL;

    X509 *cert = SSL_get_peer_certificate(ssl);

    const char *attr = Ssl::GetX509CAAttribute(cert, attribute_name);

    X509_free(cert);
    return attr;
}

const char *
sslGetUserEmail(SSL * ssl)
{
    return sslGetUserAttribute(ssl, "emailAddress");
}

const char *
sslGetUserCertificatePEM(SSL *ssl)
{
    X509 *cert;
    BIO *mem;
    static char *str = NULL;
    char *ptr;
    long len;

    safe_free(str);

    if (!ssl)
        return NULL;

    cert = SSL_get_peer_certificate(ssl);

    if (!cert)
        return NULL;

    mem = BIO_new(BIO_s_mem());

    PEM_write_bio_X509(mem, cert);

    len = BIO_get_mem_data(mem, &ptr);

    str = (char *)xmalloc(len + 1);

    memcpy(str, ptr, len);

    str[len] = '\0';

    X509_free(cert);

    BIO_free(mem);

    return str;
}

const char *
sslGetUserCertificateChainPEM(SSL *ssl)
{
    STACK_OF(X509) *chain;
    BIO *mem;
    static char *str = NULL;
    char *ptr;
    long len;
    int i;

    safe_free(str);

    if (!ssl)
        return NULL;

    chain = SSL_get_peer_cert_chain(ssl);

    if (!chain)
        return sslGetUserCertificatePEM(ssl);

    mem = BIO_new(BIO_s_mem());

    for (i = 0; i < sk_X509_num(chain); ++i) {
        X509 *cert = sk_X509_value(chain, i);
        PEM_write_bio_X509(mem, cert);
    }

    len = BIO_get_mem_data(mem, &ptr);

    str = (char *)xmalloc(len + 1);
    memcpy(str, ptr, len);
    str[len] = '\0';

    BIO_free(mem);

    return str;
}

/// Create SSL context and apply ssl certificate and private key to it.
Security::ContextPtr
Ssl::createSSLContext(Security::CertPointer & x509, Ssl::EVP_PKEY_Pointer & pkey, AnyP::PortCfg &port)
{
#if (OPENSSL_VERSION_NUMBER >= 0x10100000L)
    Security::ContextPointer sslContext(SSL_CTX_new(TLS_server_method()));
#else
    Security::ContextPointer sslContext(SSL_CTX_new(SSLv23_server_method()));
#endif

    if (!SSL_CTX_use_certificate(sslContext.get(), x509.get()))
        return NULL;

    if (!SSL_CTX_use_PrivateKey(sslContext.get(), pkey.get()))
        return NULL;

    if (!configureSslContext(sslContext.get(), port))
        return NULL;

    return sslContext.release();
}

Security::ContextPtr
Ssl::generateSslContextUsingPkeyAndCertFromMemory(const char * data, AnyP::PortCfg &port)
{
    Security::CertPointer cert;
    Ssl::EVP_PKEY_Pointer pkey;
    if (!readCertAndPrivateKeyFromMemory(cert, pkey, data) || !cert || !pkey)
        return nullptr;

    return createSSLContext(cert, pkey, port);
}

Security::ContextPtr
Ssl::generateSslContext(CertificateProperties const &properties, AnyP::PortCfg &port)
{
    Security::CertPointer cert;
    Ssl::EVP_PKEY_Pointer pkey;
    if (!generateSslCertificate(cert, pkey, properties) || !cert || !pkey)
        return nullptr;

    return createSSLContext(cert, pkey, port);
}

void
Ssl::chainCertificatesToSSLContext(SSL_CTX *sslContext, AnyP::PortCfg &port)
{
    assert(sslContext != NULL);
    // Add signing certificate to the certificates chain
    X509 *signingCert = port.signingCert.get();
    if (SSL_CTX_add_extra_chain_cert(sslContext, signingCert)) {
        // increase the certificate lock
        CRYPTO_add(&(signingCert->references),1,CRYPTO_LOCK_X509);
    } else {
        const int ssl_error = ERR_get_error();
        debugs(33, DBG_IMPORTANT, "WARNING: can not add signing certificate to SSL context chain: " << ERR_error_string(ssl_error, NULL));
    }
    Ssl::addChainToSslContext(sslContext, port.certsToChain.get());
}

void
Ssl::configureUnconfiguredSslContext(SSL_CTX *sslContext, Ssl::CertSignAlgorithm signAlgorithm,AnyP::PortCfg &port)
{
    if (sslContext && signAlgorithm == Ssl::algSignTrusted) {
        Ssl::chainCertificatesToSSLContext(sslContext, port);
    }
}

bool
Ssl::configureSSL(SSL *ssl, CertificateProperties const &properties, AnyP::PortCfg &port)
{
    Security::CertPointer cert;
    Ssl::EVP_PKEY_Pointer pkey;
    if (!generateSslCertificate(cert, pkey, properties))
        return false;

    if (!cert)
        return false;

    if (!pkey)
        return false;

    if (!SSL_use_certificate(ssl, cert.get()))
        return false;

    if (!SSL_use_PrivateKey(ssl, pkey.get()))
        return false;

    return true;
}

bool
Ssl::configureSSLUsingPkeyAndCertFromMemory(SSL *ssl, const char *data, AnyP::PortCfg &port)
{
    Security::CertPointer cert;
    Ssl::EVP_PKEY_Pointer pkey;
    if (!readCertAndPrivateKeyFromMemory(cert, pkey, data))
        return false;

    if (!cert || !pkey)
        return false;

    if (!SSL_use_certificate(ssl, cert.get()))
        return false;

    if (!SSL_use_PrivateKey(ssl, pkey.get()))
        return false;

    return true;
}

bool Ssl::verifySslCertificate(Security::ContextPtr sslContext, CertificateProperties const &properties)
{
    // SSL_get_certificate is buggy in openssl versions 1.0.1d and 1.0.1e
    // Try to retrieve certificate directly from Security::ContextPtr object
#if SQUID_USE_SSLGETCERTIFICATE_HACK
    X509 ***pCert = (X509 ***)sslContext->cert;
    X509 * cert = pCert && *pCert ? **pCert : NULL;
#elif SQUID_SSLGETCERTIFICATE_BUGGY
    X509 * cert = NULL;
    assert(0);
#else
    // Temporary ssl for getting X509 certificate from SSL_CTX.
    Security::SessionPointer ssl(SSL_new(sslContext));
    X509 * cert = SSL_get_certificate(ssl.get());
#endif
    if (!cert)
        return false;
    ASN1_TIME * time_notBefore = X509_get_notBefore(cert);
    ASN1_TIME * time_notAfter = X509_get_notAfter(cert);
    bool ret = (X509_cmp_current_time(time_notBefore) < 0 && X509_cmp_current_time(time_notAfter) > 0);
    if (!ret)
        return false;

    return certificateMatchesProperties(cert, properties);
}

bool
Ssl::setClientSNI(SSL *ssl, const char *fqdn)
{
    //The SSL_CTRL_SET_TLSEXT_HOSTNAME is a openssl macro which indicates
    // if the TLS servername extension (SNI) is enabled in openssl library.
#if defined(SSL_CTRL_SET_TLSEXT_HOSTNAME)
    if (!SSL_set_tlsext_host_name(ssl, fqdn)) {
        const int ssl_error = ERR_get_error();
        debugs(83, 3,  "WARNING: unable to set TLS servername extension (SNI): " <<
               ERR_error_string(ssl_error, NULL) << "\n");
        return false;
    }
    return true;
#else
    debugs(83, 7,  "no support for TLS servername extension (SNI)\n");
    return false;
#endif
}

void Ssl::addChainToSslContext(Security::ContextPtr sslContext, STACK_OF(X509) *chain)
{
    if (!chain)
        return;

    for (int i = 0; i < sk_X509_num(chain); ++i) {
        X509 *cert = sk_X509_value(chain, i);
        if (SSL_CTX_add_extra_chain_cert(sslContext, cert)) {
            // increase the certificate lock
            CRYPTO_add(&(cert->references),1,CRYPTO_LOCK_X509);
        } else {
            const int ssl_error = ERR_get_error();
            debugs(83, DBG_IMPORTANT, "WARNING: can not add certificate to SSL context chain: " << ERR_error_string(ssl_error, NULL));
        }
    }
}

static const char *
hasAuthorityInfoAccessCaIssuers(X509 *cert)
{
    AUTHORITY_INFO_ACCESS *info;
    if (!cert)
        return nullptr;
    info = static_cast<AUTHORITY_INFO_ACCESS *>(X509_get_ext_d2i(cert, NID_info_access, NULL, NULL));
    if (!info)
        return nullptr;

    static char uri[MAX_URL];
    uri[0] = '\0';

    for (int i = 0; i < sk_ACCESS_DESCRIPTION_num(info); i++) {
        ACCESS_DESCRIPTION *ad = sk_ACCESS_DESCRIPTION_value(info, i);
        if (OBJ_obj2nid(ad->method) == NID_ad_ca_issuers) {
            if (ad->location->type == GEN_URI) {
                xstrncpy(uri, reinterpret_cast<char *>(ASN1_STRING_data(ad->location->d.uniformResourceIdentifier)), sizeof(uri));
            }
            break;
        }
    }
    AUTHORITY_INFO_ACCESS_free(info);
    return uri[0] != '\0' ? uri : nullptr;
}

bool
Ssl::loadCerts(const char *certsFile, Ssl::CertsIndexedList &list)
{
    BIO *in = BIO_new_file(certsFile, "r");
    if (!in) {
        debugs(83, DBG_IMPORTANT, "Failed to open '" << certsFile << "' to load certificates");
        return false;
    }

    X509 *aCert;
    while((aCert = PEM_read_bio_X509(in, NULL, NULL, NULL))) {
        static char buffer[2048];
        X509_NAME_oneline(X509_get_subject_name(aCert), buffer, sizeof(buffer));
        list.insert(std::pair<SBuf, X509 *>(SBuf(buffer), aCert));
    }
    debugs(83, 4, "Loaded " << list.size() << " certificates from file: '" << certsFile << "'");
    BIO_free(in);
    return true;
}

/// quickly find the issuer certificate of a certificate cert in the
/// Ssl::CertsIndexedList list
static X509 *
findCertIssuerFast(Ssl::CertsIndexedList &list, X509 *cert)
{
    static char buffer[2048];

    if (X509_NAME *issuerName = X509_get_issuer_name(cert))
        X509_NAME_oneline(issuerName, buffer, sizeof(buffer));
    else
        return NULL;

    const auto ret = list.equal_range(SBuf(buffer));
    for (Ssl::CertsIndexedList::iterator it = ret.first; it != ret.second; ++it) {
        X509 *issuer = it->second;
        if (X509_check_issued(cert, issuer)) {
            return issuer;
        }
    }
    return NULL;
}

/// slowly find the issuer certificate of a given cert using linear search
static bool
findCertIssuer(Security::CertList const &list, X509 *cert)
{
    for (Security::CertList::const_iterator it = list.begin(); it != list.end(); ++it) {
        if (X509_check_issued(it->get(), cert) == X509_V_OK)
            return true;
    }
    return false;
}

const char *
Ssl::uriOfIssuerIfMissing(X509 *cert, Security::CertList const &serverCertificates)
{
    if (!cert || !serverCertificates.size())
        return nullptr;

    if (!findCertIssuer(serverCertificates, cert)) {
        //if issuer is missing
        if (!findCertIssuerFast(SquidUntrustedCerts, cert)) {
            // and issuer not found in local untrusted certificates database
            if (const char *issuerUri = hasAuthorityInfoAccessCaIssuers(cert)) {
                // There is a URI where we can download a certificate.
                return issuerUri;
            }
        }
    }
    return nullptr;
}

void
Ssl::missingChainCertificatesUrls(std::queue<SBuf> &URIs, Security::CertList const &serverCertificates)
{
    if (!serverCertificates.size())
        return;

    for (const auto &i : serverCertificates) {
        if (const char *issuerUri = uriOfIssuerIfMissing(i.get(), serverCertificates))
            URIs.push(SBuf(issuerUri));
    }
}

void
Ssl::SSL_add_untrusted_cert(SSL *ssl, X509 *cert)
{
    STACK_OF(X509) *untrustedStack = static_cast <STACK_OF(X509) *>(SSL_get_ex_data(ssl, ssl_ex_index_ssl_untrusted_chain));
    if (!untrustedStack) {
        untrustedStack = sk_X509_new_null();
        if (!SSL_set_ex_data(ssl, ssl_ex_index_ssl_untrusted_chain, untrustedStack)) {
            sk_X509_pop_free(untrustedStack, X509_free);
            throw TextException("Failed to attach untrusted certificates chain");
        }
    }
    sk_X509_push(untrustedStack, cert);
}

/// Search for the issuer certificate of cert in sk list.
static X509 *
sk_x509_findIssuer(STACK_OF(X509) *sk, X509 *cert)
{
    if (!sk)
        return NULL;

    const int skItemsNum = sk_X509_num(sk);
    for (int i = 0; i < skItemsNum; ++i) {
        X509 *issuer = sk_X509_value(sk, i);
        if (X509_check_issued(issuer, cert) == X509_V_OK)
            return issuer;
    }
    return NULL;
}

/// add missing issuer certificates to untrustedCerts
static void
completeIssuers(X509_STORE_CTX *ctx, STACK_OF(X509) *untrustedCerts)
{
    debugs(83, 2,  "completing " << sk_X509_num(untrustedCerts) << " OpenSSL untrusted certs using " << SquidUntrustedCerts.size() << " configured untrusted certificates");

    int depth = ctx->param->depth;
    X509 *current = ctx->cert;
    int i = 0;
    for (i = 0; current && (i < depth); ++i) {
        if (X509_check_issued(current, current)) {
            // either ctx->cert is itself self-signed or untrustedCerts
            // aready contain the self-signed current certificate
            break;
        }

        // untrustedCerts is short, not worth indexing
        X509 *issuer = sk_x509_findIssuer(untrustedCerts, current);
        if (!issuer) {
            if ((issuer = findCertIssuerFast(SquidUntrustedCerts, current)))
                sk_X509_push(untrustedCerts, issuer);
        }
        current = issuer;
    }

    if (i >= depth)
        debugs(83, 2,  "exceeded the maximum certificate chain length: " << depth);
}

/// OpenSSL certificate validation callback.
static int
untrustedToStoreCtx_cb(X509_STORE_CTX *ctx,void *data)
{
    debugs(83, 4,  "Try to use pre-downloaded intermediate certificates\n");

    SSL *ssl = static_cast<SSL *>(X509_STORE_CTX_get_ex_data(ctx, SSL_get_ex_data_X509_STORE_CTX_idx()));
    STACK_OF(X509) *sslUntrustedStack = static_cast <STACK_OF(X509) *>(SSL_get_ex_data(ssl, ssl_ex_index_ssl_untrusted_chain));

    // OpenSSL already maintains ctx->untrusted but we cannot modify
    // internal OpenSSL list directly. We have to give OpenSSL our own
    // list, but it must include certificates on the OpenSSL ctx->untrusted
    STACK_OF(X509) *oldUntrusted = ctx->untrusted;
    STACK_OF(X509) *sk = sk_X509_dup(oldUntrusted); // oldUntrusted is always not NULL

    for (int i = 0; i < sk_X509_num(sslUntrustedStack); ++i) {
        X509 *cert = sk_X509_value(sslUntrustedStack, i);
        sk_X509_push(sk, cert);
    }

    // If the local untrusted certificates internal database is used
    // run completeIssuers to add missing certificates if possible.
    if (SquidUntrustedCerts.size() > 0)
        completeIssuers(ctx, sk);

    X509_STORE_CTX_set_chain(ctx, sk); // No locking/unlocking, just sets ctx->untrusted
    int ret = X509_verify_cert(ctx);
    X509_STORE_CTX_set_chain(ctx, oldUntrusted); // Set back the old untrusted list
    sk_X509_free(sk); // Release sk list
    return ret;
}

void
Ssl::useSquidUntrusted(SSL_CTX *sslContext)
{
    SSL_CTX_set_cert_verify_callback(sslContext, untrustedToStoreCtx_cb, NULL);
}

bool
Ssl::loadSquidUntrusted(const char *path)
{
    return Ssl::loadCerts(path, SquidUntrustedCerts);
}

void
Ssl::unloadSquidUntrusted()
{
    if (SquidUntrustedCerts.size()) {
        for (Ssl::CertsIndexedList::iterator it = SquidUntrustedCerts.begin(); it != SquidUntrustedCerts.end(); ++it) {
            X509_free(it->second);
        }
        SquidUntrustedCerts.clear();
    }
}

/**
 \ingroup ServerProtocolSSLInternal
 * Read certificate from file.
 * See also: static readSslX509Certificate function, gadgets.cc file
 */
static X509 * readSslX509CertificatesChain(char const * certFilename,  STACK_OF(X509)* chain)
{
    if (!certFilename)
        return NULL;
    Ssl::BIO_Pointer bio(BIO_new(BIO_s_file_internal()));
    if (!bio)
        return NULL;
    if (!BIO_read_filename(bio.get(), certFilename))
        return NULL;
    X509 *certificate = PEM_read_bio_X509(bio.get(), NULL, NULL, NULL);

    if (certificate && chain) {

        if (X509_check_issued(certificate, certificate) == X509_V_OK)
            debugs(83, 5, "Certificate is self-signed, will not be chained");
        else {
            // and add to the chain any other certificate exist in the file
            while (X509 *ca = PEM_read_bio_X509(bio.get(), NULL, NULL, NULL)) {
                if (!sk_X509_push(chain, ca))
                    debugs(83, DBG_IMPORTANT, "WARNING: unable to add CA certificate to cert chain");
            }
        }
    }

    return certificate;
}

void Ssl::readCertChainAndPrivateKeyFromFiles(Security::CertPointer & cert, EVP_PKEY_Pointer & pkey, X509_STACK_Pointer & chain, char const * certFilename, char const * keyFilename)
{
    if (keyFilename == NULL)
        keyFilename = certFilename;

    if (certFilename == NULL)
        certFilename = keyFilename;

    debugs(83, DBG_IMPORTANT, "Using certificate in " << certFilename);

    if (!chain)
        chain.reset(sk_X509_new_null());
    if (!chain)
        debugs(83, DBG_IMPORTANT, "WARNING: unable to allocate memory for cert chain");
    // XXX: ssl_ask_password_cb needs SSL_CTX_set_default_passwd_cb_userdata()
    // so this may not fully work iff Config.Program.ssl_password is set.
    pem_password_cb *cb = ::Config.Program.ssl_password ? &ssl_ask_password_cb : NULL;
    pkey.resetWithoutLocking(readSslPrivateKey(keyFilename, cb));
    cert.resetWithoutLocking(readSslX509CertificatesChain(certFilename, chain.get()));
    if (!pkey || !cert || !X509_check_private_key(cert.get(), pkey.get())) {
        pkey.reset();
        cert.reset();
    }
}

bool Ssl::generateUntrustedCert(Security::CertPointer &untrustedCert, EVP_PKEY_Pointer &untrustedPkey, Security::CertPointer const  &cert, EVP_PKEY_Pointer const & pkey)
{
    // Generate the self-signed certificate, using a hard-coded subject prefix
    Ssl::CertificateProperties certProperties;
    if (const char *cn = CommonHostName(cert.get())) {
        certProperties.commonName = "Not trusted by \"";
        certProperties.commonName += cn;
        certProperties.commonName += "\"";
    } else if (const char *org = getOrganization(cert.get())) {
        certProperties.commonName =  "Not trusted by \"";
        certProperties.commonName += org;
        certProperties.commonName += "\"";
    } else
        certProperties.commonName =  "Not trusted";
    certProperties.setCommonName = true;
    // O, OU, and other CA subject fields will be mimicked
    // Expiration date and other common properties will be mimicked
    certProperties.signAlgorithm = Ssl::algSignSelf;
    certProperties.signWithPkey.resetAndLock(pkey.get());
    certProperties.mimicCert.resetAndLock(cert.get());
    return Ssl::generateSslCertificate(untrustedCert, untrustedPkey, certProperties);
}

static bool
SslCreate(Security::ContextPtr sslContext, const Comm::ConnectionPointer &conn, Ssl::Bio::Type type, const char *squidCtx)
{
    if (!Comm::IsConnOpen(conn)) {
        debugs(83, DBG_IMPORTANT, "Gone connection");
        return false;
    }

    const char *errAction = NULL;
    int errCode = 0;
    if (auto ssl = SSL_new(sslContext)) {
        const int fd = conn->fd;
        // without BIO, we would call SSL_set_fd(ssl, fd) instead
        if (BIO *bio = Ssl::Bio::Create(fd, type)) {
            Ssl::Bio::Link(ssl, bio); // cannot fail

            fd_table[fd].ssl.resetWithoutLocking(ssl);
            fd_table[fd].read_method = &ssl_read_method;
            fd_table[fd].write_method = &ssl_write_method;
            fd_note(fd, squidCtx);
            return true;
        }
        errCode = ERR_get_error();
        errAction = "failed to initialize I/O";
        SSL_free(ssl);
    } else {
        errCode = ERR_get_error();
        errAction = "failed to allocate handle";
    }

    debugs(83, DBG_IMPORTANT, "ERROR: " << squidCtx << ' ' << errAction <<
           ": " << ERR_error_string(errCode, NULL));
    return false;
}

bool
Ssl::CreateClient(Security::ContextPtr sslContext, const Comm::ConnectionPointer &c, const char *squidCtx)
{
    return SslCreate(sslContext, c, Ssl::Bio::BIO_TO_SERVER, squidCtx);
}

bool
Ssl::CreateServer(Security::ContextPtr sslContext, const Comm::ConnectionPointer &c, const char *squidCtx)
{
    return SslCreate(sslContext, c, Ssl::Bio::BIO_TO_CLIENT, squidCtx);
}

static int
store_session_cb(SSL *ssl, SSL_SESSION *session)
{
    if (!Ssl::SessionCache)
        return 0;

    debugs(83, 5, "Request to store SSL Session ");

    SSL_SESSION_set_timeout(session, Config.SSL.session_ttl);

    unsigned char *id = session->session_id;
    unsigned int idlen = session->session_id_length;
    unsigned char key[MEMMAP_SLOT_KEY_SIZE];
    // Session ids are of size 32bytes. They should always fit to a
    // MemMap::Slot::key
    assert(idlen <= MEMMAP_SLOT_KEY_SIZE);
    memset(key, 0, sizeof(key));
    memcpy(key, id, idlen);
    int pos;
    Ipc::MemMap::Slot *slotW = Ssl::SessionCache->openForWriting((const cache_key*)key, pos);
    if (slotW) {
        int lenRequired =  i2d_SSL_SESSION(session, NULL);
        if (lenRequired <  MEMMAP_SLOT_DATA_SIZE) {
            unsigned char *p = (unsigned char *)slotW->p;
            lenRequired = i2d_SSL_SESSION(session, &p);
            slotW->set(key, NULL, lenRequired, squid_curtime + Config.SSL.session_ttl);
        }
        Ssl::SessionCache->closeForWriting(pos);
        debugs(83, 5, "wrote an ssl session entry of size " << lenRequired << " at pos " << pos);
    }
    return 0;
}

static void
remove_session_cb(SSL_CTX *, SSL_SESSION *sessionID)
{
    if (!Ssl::SessionCache)
        return ;

    debugs(83, 5, "Request to remove corrupted or not valid SSL Session ");
    int pos;
    Ipc::MemMap::Slot const *slot = Ssl::SessionCache->openForReading((const cache_key*)sessionID, pos);
    if (slot == NULL)
        return;
    Ssl::SessionCache->closeForReading(pos);
    // TODO:
    // What if we are not able to remove the session?
    // Maybe schedule a job to remove it later?
    // For now we just have an invalid entry in cache until will be expired
    // The openSSL will reject it when we try to use it
    Ssl::SessionCache->free(pos);
}

static SSL_SESSION *
get_session_cb(SSL *, unsigned char *sessionID, int len, int *copy)
{
    if (!Ssl::SessionCache)
        return NULL;

    SSL_SESSION *session = NULL;
    const unsigned int *p;
    p = (unsigned int *)sessionID;
    debugs(83, 5, "Request to search for SSL Session of len:" <<
           len << p[0] << ":" << p[1]);

    int pos;
    Ipc::MemMap::Slot const *slot = Ssl::SessionCache->openForReading((const cache_key*)sessionID, pos);
    if (slot != NULL) {
        if (slot->expire > squid_curtime) {
            const unsigned char *ptr = slot->p;
            session = d2i_SSL_SESSION(NULL, &ptr, slot->pSize);
            debugs(83, 5, "Session retrieved from cache at pos " << pos);
        } else
            debugs(83, 5, "Session in cache expired");
        Ssl::SessionCache->closeForReading(pos);
    }

    if (!session)
        debugs(83, 5, "Failed to retrieved from cache\n");

    // With the parameter copy the callback can require the SSL engine
    // to increment the reference count of the SSL_SESSION object, Normally
    // the reference count is not incremented and therefore the session must
    // not be explicitly freed with SSL_SESSION_free(3).
    *copy = 0;
    return session;
}

void
Ssl::SetSessionCallbacks(Security::ContextPtr ctx)
{
    if (Ssl::SessionCache) {
        SSL_CTX_set_session_cache_mode(ctx, SSL_SESS_CACHE_SERVER|SSL_SESS_CACHE_NO_INTERNAL);
        SSL_CTX_sess_set_new_cb(ctx, store_session_cb);
        SSL_CTX_sess_set_remove_cb(ctx, remove_session_cb);
        SSL_CTX_sess_set_get_cb(ctx, get_session_cb);
    }
}

#endif /* USE_OPENSSL */

