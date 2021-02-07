#include "../Global.h"

#include "../Data/domain.h"
#include "../Data/tls.h"

#include <mbedtls/certs.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509.h>

static mbedtls_x509_crt tlsCrt;
static mbedtls_pk_context tlsKey;

static mbedtls_ssl_context ssl;
static mbedtls_ssl_config conf;
static mbedtls_entropy_context entropy;
static mbedtls_ctr_drbg_context ctr_drbg;

#if defined(AEM_API_SMTP) || defined(AEM_ENQUIRY)
static mbedtls_x509_crt cacert;
#endif

#include "../Common/tls_suites.h"
#ifdef AEM_MTA
static const int tls_ciphersuites[] = {AEM_TLS_CIPHERSUITES_MTA};
#elif defined(AEM_API_SMTP)
static const int tls_ciphersuites[] = {AEM_TLS_CIPHERSUITES_OUT};
static const mbedtls_ecp_group_id tls_curves[] = {AEM_TLS_CURVES_OUT};
static const int tls_hashes[] = {AEM_TLS_HASHES_OUT};
#else
static const int tls_ciphersuites[] = {AEM_TLS_CIPHERSUITES_HIGH};
static const mbedtls_ecp_group_id tls_curves[] = {AEM_TLS_CURVES_HIGH};
static const int tls_hashes[] = {AEM_TLS_HASHES_HIGH};
#endif

#if defined(AEM_MTA) || defined(AEM_API_SMTP)
#define AEM_TLS_MINOR MBEDTLS_SSL_MINOR_VERSION_1 // TLS v1.0+
#else
#define AEM_TLS_MINOR MBEDTLS_SSL_MINOR_VERSION_3 // TLS v1.2+
#endif

#if defined(AEM_MTA) || defined(AEM_API_SMTP)
__attribute__((warn_unused_result))
static uint8_t getTlsVersion(const mbedtls_ssl_context * const tls) {
	if (tls == NULL) return 0;

	const char * const c = mbedtls_ssl_get_version(tls);
	if (c == NULL || strncmp(c, "TLSv1.", 6) != 0) return 0;

	switch(c[6]) {
		case '0': return 1;
		case '1': return 2;
		case '2': return 3;
		case '3': return 4;
	}

	return 0;
}
#endif

#if defined(AEM_API_HTTP) || defined(AEM_WEB)
static int sni(void * const empty, mbedtls_ssl_context * const ssl2, const unsigned char * const hostname, const size_t len) {
	if (empty != NULL || ssl2 != &ssl) return -1;
	if (len == 0) return 0;

	return (hostname != NULL && ((len == AEM_DOMAIN_LEN && memcmp(hostname, AEM_DOMAIN, AEM_DOMAIN_LEN) == 0)
#ifdef AEM_WEB
	|| (len == AEM_DOMAIN_LEN + 8 && memcmp(hostname, "mta-sts.", 8) == 0 && memcmp(hostname + 8, AEM_DOMAIN, AEM_DOMAIN_LEN) == 0)
#endif
	)) ? 0 : -1;
}
#endif

#ifdef AEM_API_SMTP
int tlsSetup_sendmail(void) {
#else
int tlsSetup(void) {
#endif
	mbedtls_x509_crt_init(&tlsCrt);
	int ret = mbedtls_x509_crt_parse(&tlsCrt, AEM_TLS_CRT_DATA, AEM_TLS_CRT_SIZE);
	if (ret != 0) {syslog(LOG_ERR, "mbedtls_x509_crt_parse failed: %x", -ret); return -1;}

	mbedtls_pk_init(&tlsKey);
	ret = mbedtls_pk_parse_key(&tlsKey, AEM_TLS_KEY_DATA, AEM_TLS_KEY_SIZE, NULL, 0);
	if (ret != 0) {syslog(LOG_ERR, "mbedtls_pk_parse_key failed: %x", -ret); return -1;}

	mbedtls_ssl_init(&ssl);
	mbedtls_ssl_config_init(&conf);
	mbedtls_entropy_init(&entropy);
	mbedtls_ctr_drbg_init(&ctr_drbg);

#if defined(AEM_API_SMTP) || defined(AEM_ENQUIRY)
	ret = mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
#else
	ret = mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_SERVER, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
#endif
	if (ret != 0) {syslog(LOG_ERR, "mbedtls_ssl_config_defaults failed: %x", -ret); return -1;}

#ifndef AEM_MTA
	mbedtls_ssl_conf_curves(&conf, tls_curves);
	mbedtls_ssl_conf_sig_hashes(&conf, tls_hashes);
#endif

#if defined(AEM_API_HTTP) || defined(AEM_WEB)
	mbedtls_ssl_conf_dhm_min_bitlen(&conf, 2048);
	mbedtls_ssl_conf_sni(&conf, sni, NULL);
#endif

#if defined(AEM_API_SMTP) || defined(AEM_ENQUIRY)
	mbedtls_x509_crt_init(&cacert);
	ret = mbedtls_x509_crt_parse_path(&cacert, "/ssl-certs/");
	if (ret != 0) {syslog(LOG_ERR, "mbedtls_x509_crt_parse_path failed: %x", -ret); return -1;}
	mbedtls_ssl_conf_ca_chain(&conf, &cacert, NULL);
#endif

#ifdef AEM_MTA
	mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_OPTIONAL);
#else
	mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_NONE);
#endif

	mbedtls_ssl_conf_ciphersuites(&conf, tls_ciphersuites);
	mbedtls_ssl_conf_fallback(&conf, MBEDTLS_SSL_IS_NOT_FALLBACK);
	mbedtls_ssl_conf_min_version(&conf, MBEDTLS_SSL_MAJOR_VERSION_3, AEM_TLS_MINOR);
	mbedtls_ssl_conf_read_timeout(&conf, AEM_TLS_TIMEOUT);
	mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &ctr_drbg);
	mbedtls_ssl_conf_session_tickets(&conf, MBEDTLS_SSL_SESSION_TICKETS_DISABLED);

	ret = mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy, NULL, 0);
	if (ret != 0) {syslog(LOG_ERR, "mbedtls_ctr_drbg_seed failed: %x", -ret); return -1;}

	ret = mbedtls_ssl_conf_own_cert(&conf, &tlsCrt, &tlsKey);
	if (ret != 0) {syslog(LOG_ERR, "mbedtls_ssl_conf_own_cert failed: %x", -ret); return -1;}

	ret = mbedtls_ssl_setup(&ssl, &conf);
	if (ret != 0) {syslog(LOG_ERR, "mbedtls_ssl_setup failed: %x", -ret); return -1;}

	return 0;
}

#ifdef AEM_API_SMTP
void tlsFree_sendmail(void) {
#else
void tlsFree(void) {
#endif
	mbedtls_ssl_free(&ssl);
	mbedtls_ssl_config_free(&conf);
	mbedtls_entropy_free(&entropy);
	mbedtls_ctr_drbg_free(&ctr_drbg);
	mbedtls_x509_crt_free(&tlsCrt);
	mbedtls_pk_free(&tlsKey);
}
