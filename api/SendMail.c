#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#include <sodium.h>
#include <wolfssl/options.h>
#include <wolfssl/ssl.h>
#include <wolfssl/wolfcrypt/rsa.h>
#include <wolfssl/error-ssl.h>

#include "../Global.h"
#include "../Common/memeq.h"
#include "../Common/tls_suites.h"

#include "Error.h"
#include "MessageId.h"

#include "SendMail.h"

static WOLFSSL_CTX *ctx;
static unsigned char ourDomain[AEM_MAXLEN_OURDOMAIN + 1];

static int setKeyShare(WOLFSSL *ssl) {
	for(;;) {
		const int ret = wolfSSL_UseKeyShare(ssl, WOLFSSL_ECC_X25519);
		if (ret == WOLFSSL_SUCCESS) break;
		if (ret != WC_PENDING_E) return -1;
	}

	for(;;) {
		const int ret = wolfSSL_UseKeyShare(ssl, WOLFSSL_ECC_SECP256R1);
		if (ret == WOLFSSL_SUCCESS) break;
		if (ret != WC_PENDING_E) return -1;
	}

	return (wolfSSL_set_groups(ssl, (int[]){WOLFSSL_ECC_X25519, WOLFSSL_ECC_SECP256R1}, 2) == WOLFSSL_SUCCESS) ? 0 : -1;
}

int sendMail_tls_init(const unsigned char * const crt, const size_t lenCrt, const unsigned char * const key, const size_t lenKey, const unsigned char * const domain, const size_t lenDomain) {
	ctx = wolfSSL_CTX_new(wolfSSLv23_client_method());
	if (ctx == NULL) return 10;

	if (wolfSSL_CTX_SetMinVersion(ctx, 1) != WOLFSSL_SUCCESS) return 11;
	if (wolfSSL_CTX_set_cipher_list(ctx, AEM_TLS_CIPHERSUITES_MTA) != WOLFSSL_SUCCESS) return 12;

	if (wolfSSL_CTX_use_certificate_chain_buffer(ctx, crt, lenCrt) != WOLFSSL_SUCCESS) return 14;
	if (wolfSSL_CTX_use_PrivateKey_buffer(ctx, key, lenKey, WOLFSSL_FILETYPE_PEM) != WOLFSSL_SUCCESS) return 15;

	const int err = wolfSSL_CTX_load_verify_locations_ex(ctx, NULL, "/ssl-certs/", WOLFSSL_LOAD_FLAG_IGNORE_ERR);
	if (err != WOLFSSL_SUCCESS) {syslog(LOG_ERR, "Failed loading certs: %d"); return 16;}

	bzero(ourDomain, AEM_MAXLEN_OURDOMAIN + 1);
	memcpy(ourDomain, domain, lenDomain);
	return 0;
}

void sendMail_tls_free(void) {
	wolfSSL_CTX_free(ctx);
}

static int makeSocket(const uint32_t ip) {
	const int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (sock < 0) {syslog(LOG_ERR, "Failed socket(): %m"); return -1;}

	struct sockaddr_in mxAddr;
	mxAddr.sin_family = AF_INET;
	mxAddr.sin_port = htons(25);
	mxAddr.sin_addr.s_addr = ip;

	if (connect(sock, (struct sockaddr*)&mxAddr, sizeof(struct sockaddr_in)) != 0) {
		syslog(LOG_ERR, "Failed connect(): %m");
		close(sock);
		return -1;
	}

	return sock;
}

// RSA-SHA256 signature for DKIM
static size_t rsa_sign_b64(char * const sigB64, const unsigned char * const hash, const unsigned char * const rsaKey, const size_t lenRsaKey) {
	// Setup
	RsaKey rsa;
	if (wc_InitRsaKey(&rsa, NULL) != 0) {syslog(LOG_ERR, "wc_InitRsaKey failed"); return 0;}

	word32 idx = 0;
	if (wc_RsaPrivateKeyDecode(rsaKey, &idx, &rsa, lenRsaKey) != 0) {syslog(LOG_ERR, "wc_RsaPrivateKeyDecode failed"); return 0;}

	const size_t lenSig = wc_RsaEncryptSize(&rsa);

	// Pad, ASN1 encode
	unsigned char sig[lenSig];
	sig[0] = 0;
	sig[1] = RSA_BLOCK_TYPE_1;
	memset(sig + 2, 0xFF, lenSig - crypto_hash_sha256_BYTES - 22);
	sig[lenSig - crypto_hash_sha256_BYTES - 20] = 0; // Separator
	memcpy(sig + lenSig - crypto_hash_sha256_BYTES - 19, (const unsigned char[]){0x30,0x31,0x30,0x0D,0x06,0x09,0x60,0x86,0x48,0x01,0x65,0x03,0x04,0x02,0x01,0x05,0x00,0x04,0x20}, 19); // ASN1
	memcpy(sig + lenSig - crypto_hash_sha256_BYTES, hash, crypto_hash_sha256_BYTES);

	// Sign
	rsa.dataLen = lenSig;
	const int ret = wc_RsaFunction(sig, (word32)lenSig, sig, &rsa.dataLen, RSA_PRIVATE_ENCRYPT, &rsa, NULL);
	wc_FreeRsaKey(&rsa);
	if (ret != 0) return 0;

	// Base64
	sodium_bin2base64(sigB64, sodium_base64_ENCODED_LEN(256, sodium_base64_VARIANT_ORIGINAL), sig, 256, sodium_base64_VARIANT_ORIGINAL);
	return sodium_base64_ENCODED_LEN(256, sodium_base64_VARIANT_ORIGINAL) - 1; // Remove terminating zero-byte
}

static char *createEmail(const struct outEmail * const email, size_t * const lenOut) {
	unsigned char bodyHash[crypto_hash_sha256_BYTES];
	if (crypto_hash_sha256(bodyHash, (unsigned char*)email->body, email->lenBody) != 0) return NULL;

	char bodyHashB64[sodium_base64_ENCODED_LEN(crypto_hash_sha256_BYTES, sodium_base64_VARIANT_ORIGINAL)];
	sodium_bin2base64(bodyHashB64, sodium_base64_ENCODED_LEN(crypto_hash_sha256_BYTES, sodium_base64_VARIANT_ORIGINAL), bodyHash, crypto_hash_sha256_BYTES, sodium_base64_VARIANT_ORIGINAL);

	const uint32_t ts = (uint32_t)time(NULL);

	char msgId[26];
	genMsgId(msgId, ts, email->uid, email->fromAddr32, true);

	const time_t msgTime = ts;
	struct tm ourTime;
	if (localtime_r(&msgTime, &ourTime) == NULL) return NULL;
	char rfctime[64];
	strftime(rfctime, 64, "%a, %d %b %Y %T %z", &ourTime); // Wed, 17 Jun 2020 08:30:21 +0000

// header-hash = SHA256(headers, crlf separated + DKIM-Signature-field with b= empty, no crlf)
	char *final = malloc(2000 + email->lenBody);
	if (final == NULL) {syslog(LOG_ERR, "Failed allocation"); return NULL;}
	bzero(final, 2000 + email->lenBody);

	char ref[544];
	if (strlen(email->replyId) > 5) { // a@b.cd
		sprintf(ref,
			"References: <%s>\r\n"
			"In-Reply-To: <%s>\r\n"
		, email->replyId, email->replyId);
	} else ref[0] = '\0';

	int lenFinal = sprintf(final,
		"%s" // References + In-Reply-To
		"From: %s@%s\r\n"
		"Date: %s\r\n"
		"Message-ID: <%.26s@%s>\r\n"
		"Subject: %s\r\n"
		"To: %s\r\n"
		"DKIM-Signature:"
			" v=1;"
			" a=rsa-sha256;" //ed25519-sha256
			" c=simple/simple;"
			" d=%s;"
			" i=%s@%s;"
			" q=dns/txt;"
			" s=%s;"
			" t=%u;"
			" x=%u;"
			" h="
				// Headers in use
				"References:"
				"In-Reply-To:"
				"From:"
				"Date:"
				"Message-ID:"
				"Subject:"
				"To:"
				// Unused headers
				"Cc:"
				"Content-Type:"
				"MIME-Version:"
				"Reply-To:"
				"Sender;"
			" bh=%s;"
			" b="
	, ref
	, email->addrFrom
	, ourDomain
	, rfctime
	, msgId
	, ourDomain
	, email->subject
	, email->addrTo
	, ourDomain
	, email->addrFrom //i=
	, ourDomain
	, email->isAdmin? "admin" : "users"
	, ts // t=
	, ts + 86400 // x=; expire after a day
	, bodyHashB64
	);

// EdDSA
/*
	unsigned char sig[crypto_sign_BYTES];
	crypto_sign_detached(sig, NULL, headHash, 32, isAdmin? dkim_adm_skey : dkim_usr_skey);

	char sigB64[sodium_base64_ENCODED_LEN(crypto_sign_BYTES, sodium_base64_VARIANT_ORIGINAL)];
	sodium_bin2base64(sigB64, sodium_base64_ENCODED_LEN(crypto_sign_BYTES, sodium_base64_VARIANT_ORIGINAL), sig, crypto_sign_BYTES, sodium_base64_VARIANT_ORIGINAL);
*/

// RSA-SHA256
	unsigned char headHash[crypto_hash_sha256_BYTES];
	if (crypto_hash_sha256(headHash, (unsigned char*)final, lenFinal) != 0) {free(final); return NULL;}

	const size_t lenSigB64 = rsa_sign_b64(final + lenFinal, headHash, email->rsaKey, email->lenRsaKey);
	if (lenSigB64 < 1) {free(final); return NULL;}
	lenFinal += lenSigB64;

	memcpy(final + lenFinal, "\r\n", 2);
	lenFinal += 2;

	const char * const dkim = (const char*)memmem(final, lenFinal, "\nDKIM-Signature:", 16) + 1;
	memcpy(final + lenFinal, final, dkim - final);
	memmove(final, dkim, lenFinal);

	memcpy(final + lenFinal, "\r\n", 2);
	lenFinal += 2;

	// Copy the body, dot-stuffing as necessary
	for (size_t i = 0; i < email->lenBody; i++) {
		if (email->body[i] == '.' && final[lenFinal - 1] == '\n') {
			memcpy(final + lenFinal, "..", 2);
			lenFinal += 2;
		} else {
			final[lenFinal] = email->body[i];
			lenFinal++;
		}
	}

	memcpy(final + lenFinal, ".\r\n", 3);
	lenFinal += 3;

	*lenOut = lenFinal;
	return final;
}

static int smtp_recv(const int sock, WOLFSSL *ssl, char * const buf) {
	return (ssl == NULL) ? recv(sock, buf, 1024, 0) : wolfSSL_read(ssl, buf, 1024);
}

static int smtp_send(const int sock, WOLFSSL *ssl, const char * const data, const size_t lenData) {
	if (lenData < 1) return 0;

	size_t sent = 0;
	while (sent < lenData) {
		const int ret = (ssl == NULL) ? send(sock, data + sent, lenData - sent, 0) : wolfSSL_write(ssl, data + sent, lenData - sent);
		if (ret < 0) return ret;
		sent += ret;
	}

	return sent;
}

static void smtp_quit(const int sock, WOLFSSL *ssl) {
	if (smtp_send(sock, ssl, "QUIT\r\n", 6) == 6) {
		char buf[1024];
		smtp_recv(sock, ssl, buf);
		// if (len < 4 || !memeq(buf, "221 ", 4)) // 221 should be received here
	}

	if (ssl != NULL) {
		wolfSSL_shutdown(ssl);
		wolfSSL_free(ssl);
	}

	close(sock);
}

static bool smtpCommand(const int sock, WOLFSSL *ssl, struct outInfo * const info, char * const buf, size_t * const lenBuf, const char * const sendText, const size_t lenSendText, const char * const expectedResponse) {
	if (smtp_send(sock, ssl, sendText, lenSendText) != (int)lenSendText) {
		if (ssl != NULL) {
			wolfSSL_shutdown(ssl);
			wolfSSL_free(ssl);
		}

		close(sock);
		return false;
	}

	const int len = smtp_recv(sock, ssl, buf);
	info->lenStatus = MIN(len, 127);
	memcpy(info->status, buf, info->lenStatus);

	if (len < 6 || !memeq(buf, expectedResponse, strlen(expectedResponse)) || !memeq(buf + len - 2, "\r\n", 2)) {
		smtp_quit(sock, ssl);
		return false;
	}

	*lenBuf = len;
	buf[len] = '\0';
	return true;
}

unsigned char sendMail(const struct outEmail * const email, struct outInfo * const info) {
	WOLFSSL *ssl = wolfSSL_new(ctx);
	if (ssl == NULL) {
		int err = wolfSSL_get_error(ssl, 0);
		char buffer[1024];
		syslog(LOG_ERR, "ssl_new error %d: %s\n", err, wolfSSL_ERR_error_string(err, buffer));
		return AEM_API_ERR_INTERNAL;
	}

	int sock = makeSocket(email->ip);
	if (sock < 1) {syslog(LOG_ERR, "sendMail: Failed makeSocket()"); return AEM_API_ERR_INTERNAL;}

	char buf[1025];
	size_t lenBuf;
	if (!smtpCommand(sock, NULL, info, buf, &lenBuf, NULL, 0, "220 ")) return AEM_API_ERR_MESSAGE_CREATE_SENDMAIL_GREET;

	// Copy greeting from between '220 ' and '\r\n'
	info->lenGreeting = MIN(lenBuf - 6, 256);
	memcpy(info->greeting, buf + 4, info->lenGreeting);

	char ehlo[256];
	sprintf(ehlo, "EHLO %s\r\n", ourDomain);

	if (!smtpCommand(sock, NULL, info, buf, &lenBuf, ehlo, strlen(ehlo), "250")) return AEM_API_ERR_MESSAGE_CREATE_SENDMAIL_EHLO;
	if (strcasestr(buf, "STARTTLS") == NULL) {smtp_quit(sock, NULL); return AEM_API_ERR_MESSAGE_CREATE_SENDMAIL_NOTLS;}
	if (!smtpCommand(sock, NULL, info, buf, &lenBuf, "STARTTLS\r\n", 10, "220")) return AEM_API_ERR_MESSAGE_CREATE_SENDMAIL_STLS;

	setKeyShare(ssl);

	if (
	   wolfSSL_set_fd(ssl, sock) != WOLFSSL_SUCCESS
	|| wolfSSL_connect(ssl) != WOLFSSL_SUCCESS
	|| wolfSSL_state(ssl) != 0) {
		int err = wolfSSL_get_error(ssl, 0);
		char buffer[1024];
		syslog(LOG_ERR, "wolfSSL_handshake error %d, %s\n", err, wolfSSL_ERR_error_string(err, buffer));
		smtp_quit(sock, NULL);
		return AEM_API_ERR_MESSAGE_CREATE_SENDMAIL_SHAKE;
	}

//	info->tls_ciphersuite = ...
//	info->tls_version = ...

//	if (...) {syslog(LOG_ERR, "SendMail: Failed verifying cert"); closeTls(sock); return AEM_API_ERR_MESSAGE_CREATE_SENDMAIL_STLS;}

	char send_fr[512]; sprintf(send_fr, "MAIL FROM: <%s@%s>\r\n", email->addrFrom, ourDomain);
	char send_to[512]; sprintf(send_to, "RCPT TO: <%s>\r\n", email->addrTo);

	if (!smtpCommand(sock, ssl, info, buf, &lenBuf, ehlo, strlen(ehlo),       "250")) return AEM_API_ERR_MESSAGE_CREATE_SENDMAIL_EHLO;
	if (!smtpCommand(sock, ssl, info, buf, &lenBuf, send_fr, strlen(send_fr), "250")) return AEM_API_ERR_MESSAGE_CREATE_SENDMAIL_MAIL;
	if (!smtpCommand(sock, ssl, info, buf, &lenBuf, send_to, strlen(send_to), "250")) return AEM_API_ERR_MESSAGE_CREATE_SENDMAIL_RCPT;
	if (!smtpCommand(sock, ssl, info, buf, &lenBuf, "DATA\r\n", 6,            "354")) return AEM_API_ERR_MESSAGE_CREATE_SENDMAIL_DATA;

	size_t lenMsg = 0;
	char * const msg = createEmail(email, &lenMsg);
	if (msg == NULL) {
		smtp_quit(sock, ssl);
		return AEM_API_ERR_INTERNAL;
	}

	if (!smtpCommand(sock, ssl, info, buf, &lenBuf, msg, lenMsg, "250")) {
		free(msg);
		return AEM_API_ERR_MESSAGE_CREATE_SENDMAIL_BODY;
	}

	free(msg);
	smtp_quit(sock, ssl);
	return AEM_API_STATUS_OK;
}
