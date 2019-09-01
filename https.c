#define _GNU_SOURCE // for memmem

#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <ctype.h>
#include <errno.h>

#include <sodium.h>

#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/error.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/ssl.h"

#include "aem_file.h"

#include "Includes/SixBit.h"
#include "Database.h"
#include "Message.h"

#include "https.h"

#define AEM_FILETYPE_CSS 1
#define AEM_FILETYPE_IMG 2
#define AEM_FILETYPE_JS  3

#define AEM_HTTPS_TIMEOUT 30
#define AEM_HTTPS_MAXREQSIZE 8192

#define AEM_MAXMSGTOTALSIZE 1048576 // 1 MiB. Max size of inbox response. TODO: Move this to config

#define AEM_HTTPS_REQUEST_INVALID -1
#define AEM_HTTPS_REQUEST_GET 0
// POST: body size (Content-Length)

static const int https_ciphersuites[] = {
	MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384,
	MBEDTLS_TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384,
	MBEDTLS_TLS_DHE_RSA_WITH_AES_256_GCM_SHA384,
	MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_256_CCM,
	MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_256_CCM_8,
	MBEDTLS_TLS_DHE_RSA_WITH_AES_256_CCM,
	MBEDTLS_TLS_DHE_RSA_WITH_AES_256_CCM_8,
	MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256,
	MBEDTLS_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256,
	MBEDTLS_TLS_DHE_RSA_WITH_AES_128_GCM_SHA256,
	MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_128_CCM,
	MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_128_CCM_8,
	MBEDTLS_TLS_DHE_RSA_WITH_AES_128_CCM,
	MBEDTLS_TLS_DHE_RSA_WITH_AES_128_CCM_8,
	MBEDTLS_TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256,
	MBEDTLS_TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256,
	MBEDTLS_TLS_DHE_RSA_WITH_CHACHA20_POLY1305_SHA256,
0};

static const mbedtls_ecp_group_id https_curves[] = {
	MBEDTLS_ECP_DP_CURVE448,
	MBEDTLS_ECP_DP_CURVE25519,
	MBEDTLS_ECP_DP_SECP521R1,
	MBEDTLS_ECP_DP_SECP384R1,
MBEDTLS_ECP_DP_NONE};

static const int https_hashes[] = {
	MBEDTLS_SSL_HASH_SHA512,
MBEDTLS_MD_NONE};

static void sendData(mbedtls_ssl_context * const ssl, const char * const data, const size_t lenData) {
	size_t sent = 0;

	while (sent < lenData) {
		int ret;
		do {ret = mbedtls_ssl_write(ssl, (unsigned char*)(data + sent), lenData - sent);} while (ret == MBEDTLS_ERR_SSL_WANT_WRITE);
		if (ret < 0) {printf("[HTTPS] Failed to send data: %d\n", ret); return;}
		sent += ret;
	}
}

static void send204(mbedtls_ssl_context * const ssl) {
	sendData(ssl,
		"HTTP/1.1 204 aem\r\n"
		"Tk: N\r\n"
		"Strict-Transport-Security: max-age=94672800; includeSubDomains\r\n"
		"Expect-CT: enforce; max-age=94672800\r\n"
		"Connection: close\r\n"
		"Content-Length: 0\r\n"
		"Access-Control-Allow-Origin: *\r\n"
		"\r\n"
	, 199);
}

static void respond_https_html(mbedtls_ssl_context * const ssl, const char * const name, const size_t lenName, const struct aem_file * const files, const int fileCount, const char * const domain, const size_t lenDomain) {
	int reqNum = -1;

	for (int i = 0; i < fileCount; i++) {
		if (strlen(files[i].filename) == lenName && memcmp(files[i].filename, name, lenName) == 0) {
			reqNum = i;
			break;
		}
	}

	if (reqNum < 0) return;

	if (files[reqNum].lenData > 99999) return;

	char data[1298 + (lenDomain * 4) + files[reqNum].lenData];
	sprintf(data,
		"HTTP/1.1 200 aem\r\n"
		"Tk: N\r\n"
		"Strict-Transport-Security: max-age=94672800; includeSubDomains; preload\r\n"
		"Connection: close\r\n"
		"Content-Encoding: br\r\n"
		"Content-Type: text/html; charset=utf-8\r\n"
		"Content-Length: %zd\r\n"

		"Content-Security-Policy:"
			"connect-src"     " https://%.*s/api/;"
			"img-src"         " https://%.*s/img/;"
			"script-src"      " https://%.*s/js/ https://cdn.jsdelivr.net/gh/google/brotli@1.0.7/js/decode.min.js https://cdnjs.cloudflare.com/ajax/libs/js-nacl/1.3.2/nacl_factory.min.js;"
			"style-src"       " https://%.*s/css/;"

			"base-uri"        " 'none';"
			"child-src"       " 'none';"
			"default-src"     " 'none';"
			"font-src"        " 'none';"
			"form-action"     " 'none';"
			"frame-ancestors" " 'none';"
			"frame-src"       " 'none';"
			"manifest-src"    " 'none';"
			"media-src"       " 'none';"
			"object-src"      " 'none';"
			"prefetch-src"    " 'none';"
			"worker-src"      " 'none';"

			"block-all-mixed-content;"
			"sandbox allow-scripts allow-same-origin;"
		"\r\n"

		"Feature-Policy:"
			"autoplay"             " 'none';"
			"accelerometer"        " 'none';"
			"ambient-light-sensor" " 'none';"
			"camera"               " 'none';"
			"cookie"               " 'none';"
			"display-capture"      " 'none';"
			"document-domain"      " 'none';"
			"docwrite"             " 'none';"
			"encrypted-media"      " 'none';"
			"fullscreen"           " 'none';"
			"geolocation"          " 'none';"
			"gyroscope"            " 'none';"
			"magnetometer"         " 'none';"
			"microphone"           " 'none';"
			"midi"                 " 'none';"
			"payment"              " 'none';"
			"picture-in-picture"   " 'none';"
			"speaker"              " 'none';"
			"sync-xhr"             " 'none';"
			"usb"                  " 'none';"
			"vr"                   " 'none';"
		"\r\n"

		"Expect-CT: enforce; max-age=94672800\r\n"
		"Referrer-Policy: no-referrer\r\n"
		"X-Content-Type-Options: nosniff\r\n"
		"X-Frame-Options: deny\r\n"
		"X-XSS-Protection: 1; mode=block\r\n"
		"\r\n"
	, files[reqNum].lenData, (int)lenDomain, domain, (int)lenDomain, domain, (int)lenDomain, domain, (int)lenDomain, domain);

	size_t lenHeaders = strlen(data);
	memcpy(data + lenHeaders, files[reqNum].data, files[reqNum].lenData);

	sendData(ssl, data, lenHeaders + files[reqNum].lenData);
}

// Javascript, CSS, images etc
static void respond_https_file(mbedtls_ssl_context * const ssl, const char * const name, const size_t lenName, const int fileType, const struct aem_file * const files, const int fileCount) {
	int reqNum = -1;

	for (int i = 0; i < fileCount; i++) {
		if (strlen(files[i].filename) == lenName && memcmp(files[i].filename, name, lenName) == 0) {
			reqNum = i;
			break;
		}
	}

	if (reqNum < 0) return;

	if (files[reqNum].lenData > 999999) return;

	char *mediatype;
	int mtLen;
	switch (fileType) {
		case AEM_FILETYPE_IMG:
			mediatype = "image/webp";
			mtLen = 10;
			break;
		case AEM_FILETYPE_JS:
			mediatype = "application/javascript; charset=utf-8";
			mtLen = 37;
			break;
		case AEM_FILETYPE_CSS:
			mediatype = "text/css; charset=utf-8";
			mtLen = 23;
			break;
		default:
			return;
	}

	char headers[287 + mtLen];
	sprintf(headers,
		"HTTP/1.1 200 aem\r\n"
		"Tk: N\r\n"
		"Strict-Transport-Security: max-age=94672800; includeSubDomains\r\n"
		"Expect-CT: enforce; max-age=94672800\r\n"
		"Connection: close\r\n"
		"%s"
		"Content-Type: %.*s\r\n"
		"Content-Length: %zd\r\n"
		"X-Content-Type-Options: nosniff\r\n"
		"Cross-Origin-Resource-Policy: same-origin\r\n"
		"\r\n"
	, (fileType == AEM_FILETYPE_CSS || fileType == AEM_FILETYPE_JS) ? "Content-Encoding: br\r\n" : "", mtLen, mediatype, files[reqNum].lenData);

	const size_t lenHeaders = strlen(headers);

	char data[lenHeaders + files[reqNum].lenData];
	memcpy(data, headers, lenHeaders);
	memcpy(data + lenHeaders, files[reqNum].data, files[reqNum].lenData);

	sendData(ssl, data, lenHeaders + files[reqNum].lenData);
}

static int numDigits(double number) {
	int digits = 0;
	while (number > 1) {number /= 10; digits++;}
	return digits;
}

// Web login (get settings and messages)
// TODO: Support multiple pages
static void respond_https_login(mbedtls_ssl_context * const ssl, const int64_t upk64, char * const * const decrypted, const size_t lenDecrypted) {
	if (lenDecrypted != 17 || memcmp(*decrypted, "AllEars:Web.Login", 17) != 0) {sodium_free(*decrypted); return;}
	sodium_free(*decrypted);

	unsigned char *noteData;
	unsigned char *addrData;
	unsigned char *gkData;
	const int lenNote = AEM_NOTEDATA_LEN + crypto_box_SEALBYTES;
	uint16_t lenAddr;
	uint16_t lenGk;
	uint8_t msgCount;
	uint8_t level;

	const int ret = getUserInfo(upk64, &level, &noteData, &addrData, &lenAddr, &gkData, &lenGk);
	if (ret != 0) return;

	const size_t lenAdmin = (level == 3) ? AEM_ADMINDATA_LEN : 0;
	unsigned char *adminData;
	if (level == 3) getAdminData(&adminData);

	const size_t lenMsg = (level == 3) ? AEM_MAXMSGTOTALSIZE : AEM_MAXMSGTOTALSIZE - AEM_ADMINDATA_LEN;
	unsigned char * const msgData = getUserMessages(upk64, &msgCount, lenMsg);
	if (msgData == NULL) {free(addrData); free(noteData); free(gkData); if (level == 3) {free(adminData);} return;}

	const size_t lenBody = 6 + lenNote + lenAddr + lenGk + lenAdmin + lenMsg;
	const size_t lenHead = 198 + numDigits(lenBody);
	const size_t lenResponse = lenHead + lenBody;

	char * const data = malloc(lenResponse);
	sprintf(data,
		"HTTP/1.1 200 aem\r\n"
		"Tk: N\r\n"
		"Strict-Transport-Security: max-age=94672800; includeSubDomains\r\n"
		"Expect-CT: enforce; max-age=94672800\r\n"
		"Connection: close\r\n"
		"Content-Length: %zd\r\n"
		"Access-Control-Allow-Origin: *\r\n"
		"\r\n"
	, lenBody);

	memcpy(data + lenHead + 0, &level,    1);
	memcpy(data + lenHead + 1, &msgCount, 1);
	memcpy(data + lenHead + 2, &lenAddr,  2);
	memcpy(data + lenHead + 4, &lenGk,    2);

	size_t s = lenHead + 6;
	memcpy(data + s, noteData,  lenNote);  s += lenNote;
	memcpy(data + s, addrData,  lenAddr);  s += lenAddr;
	memcpy(data + s, gkData,    lenGk);    s += lenGk;
	if (level == 3) {memcpy(data + s, adminData, lenAdmin); s += lenAdmin;}
	memcpy(data + s, msgData,   lenMsg);   s += lenMsg;

	free(noteData);
	free(addrData);
	free(gkData);
	if (level == 3) free(adminData);
	free(msgData);

	sendData(ssl, data, lenResponse);
	free(data);
}

static int sendIntMsg(const unsigned char * const addrKey, const char * const addrFrom, const size_t lenFrom, const char * const addrTo, const size_t lenTo,
char * const * const decrypted, const size_t bodyBegin, const size_t lenDecrypted, const unsigned char * const sender_pk, const char senderCopy) {
	if (addrFrom == NULL || addrTo == NULL || lenFrom < 1 || lenTo < 1) return -1;

	unsigned char binFrom[18];
	int ret = addr2bin(addrFrom, lenFrom, binFrom);
	if (ret < 1) return -1;

	unsigned char binTo[18];
	ret = addr2bin(addrTo, lenTo, binTo);
	if (ret < 1) return -1;

	unsigned char pk[crypto_box_PUBLICKEYBYTES];
	unsigned char flags;
	ret = getPublicKeyFromAddress(binTo, pk, addrKey, &flags);
	if (ret != 0 || !(flags & AEM_FLAGS_ACC_INTMSG) || memcmp(pk, sender_pk, crypto_box_PUBLICKEYBYTES) == 0) return -1;

	int64_t sender_pk64;
	memcpy(&sender_pk64, sender_pk, 8);
	const int memberLevel = getUserLevel(sender_pk64);

	size_t bodyLen = lenDecrypted - bodyBegin;
	unsigned char *boxSet = makeMsg_Int(pk, binFrom, binTo, *decrypted + bodyBegin, &bodyLen, memberLevel);
	const size_t bsLen = AEM_HEADBOX_SIZE + crypto_box_SEALBYTES + bodyLen + crypto_box_SEALBYTES;
	if (boxSet == NULL) return -1;

	int64_t upk64;
	memcpy(&upk64, pk, 8);
	addUserMessage(upk64, boxSet, bsLen);
	free(boxSet);

	if (senderCopy == 'Y') {
		bodyLen = lenDecrypted - bodyBegin;
		boxSet = makeMsg_Int(sender_pk, binFrom, binTo, *decrypted + bodyBegin, &bodyLen, memberLevel);
		if (boxSet == NULL) return -1;

		memcpy(&upk64, sender_pk, 8);
		addUserMessage(upk64, boxSet, bsLen);
		free(boxSet);
	}

	return 0;
}

// Message sending
static void respond_https_send(mbedtls_ssl_context * const ssl, const unsigned char * const upk, const char * const domain, const size_t lenDomain, char * const * const decrypted, const size_t lenDecrypted, const unsigned char * const addrKey) {
/* Format:
	(From)\n
	(To)\n
	(Title)\n
	(Body)
*/
	const char senderCopy = *decrypted[0];

	const char *addrFrom = *decrypted + 1;
	const char *endFrom = strchr(addrFrom, '\n');
	if (endFrom == NULL) {sodium_free(*decrypted); return;}
	const size_t lenFrom = endFrom - addrFrom;

	const char *addrTo = endFrom + 1;
	const char *endTo = strchr(addrTo, '\n');
	if (endTo == NULL) {sodium_free(*decrypted); return;}
	const size_t lenTo = endTo - addrTo;

	int ret;
	if (lenTo > lenDomain + 1 && addrTo[lenTo - lenDomain - 1] == '@' && memcmp(addrTo + lenTo - lenDomain, domain, lenDomain) == 0) {
		ret = sendIntMsg(addrKey, addrFrom, lenFrom, addrTo, lenTo - lenDomain - 1, decrypted, (endTo + 1) - *decrypted, lenDecrypted, upk, senderCopy);
	} else {
		const char * const domainAt = strchr(addrTo, '@');
		if (domainAt == NULL) {
			sodium_free(*decrypted);
			return;
		}

		const size_t lenExtDomain = lenTo - (domainAt - addrTo) - 1;
		char extDomain[lenExtDomain + 1];
		memcpy(extDomain, domainAt + 1, lenExtDomain);
		extDomain[lenExtDomain] = '\0';

		// TODO: ExtMsg (email)
		return;
	}

	sodium_free(*decrypted);
	if (ret == 0) send204(ssl);
}

static void respond_https_addnote(mbedtls_ssl_context * const ssl, unsigned char * const upk, char * const * const decrypted, const size_t lenDecrypted, const bool isFileNote) {
	if (lenDecrypted > (262146 + crypto_box_SEALBYTES) || (lenDecrypted - crypto_box_SEALBYTES) % 1026 != 0) return; // 256 KiB max size; padded to nearest 1024 prior to encryption (2 first bytes store padding length)

	// TODO: Move to Message.c
	// HeadBox format for notes: [1B] SenderInfo, [4B] Timestamp (uint32_t), 36 bytes unused (zeroed)
	unsigned char header[AEM_HEADBOX_SIZE];
	bzero(header, AEM_HEADBOX_SIZE);

	if (isFileNote)
		header[0] |= AEM_FLAG_MSGTYPE_FILENOTE;
	else
		header[0] |= AEM_FLAG_MSGTYPE_TEXTNOTE;

	const uint32_t t = (uint32_t)time(NULL);
	memcpy(header + 1, &t, 4);

	const size_t bsLen = AEM_HEADBOX_SIZE + crypto_box_SEALBYTES + lenDecrypted;
	unsigned char * const boxset = malloc(bsLen);

	crypto_box_seal(boxset, header, AEM_HEADBOX_SIZE, upk);
	memcpy(boxset + AEM_HEADBOX_SIZE + crypto_box_SEALBYTES, *decrypted, lenDecrypted);

	int64_t upk64;
	memcpy(&upk64, upk, 8);
	addUserMessage(upk64, boxset, bsLen);
	free(boxset);

	send204(ssl);
}

static char *openWebBox(const unsigned char * const post, const size_t lenPost, unsigned char * const upk, size_t * const lenDecrypted, const unsigned char * const ssk) {
	const size_t skipBytes = crypto_box_NONCEBYTES + crypto_box_PUBLICKEYBYTES;

	if (lenPost <= skipBytes) return NULL;

	unsigned char nonce[crypto_box_NONCEBYTES];
	memcpy(nonce, post, crypto_box_NONCEBYTES);

	memcpy(upk, post + crypto_box_NONCEBYTES, crypto_box_PUBLICKEYBYTES);

	int64_t upk64;
	memcpy(&upk64, upk, 8);
	if (!upk64Exists(upk64)) return NULL;

	char * const decrypted = sodium_malloc(lenPost);
	if (decrypted == NULL) return NULL;

	const int ret = crypto_box_open_easy((unsigned char*)decrypted, post + skipBytes, lenPost - skipBytes, nonce, upk, ssk);
	if (ret != 0) {sodium_free(decrypted); return NULL;}

	sodium_mprotect_readonly(decrypted);
	*lenDecrypted = lenPost - skipBytes - crypto_box_MACBYTES;

	return decrypted;
}

static void respond_https_addr_add(mbedtls_ssl_context * const ssl, const int64_t upk64, char * const * const decrypted, const size_t lenDecrypted, const unsigned char * const addrKey) {
	unsigned char addr[18];
	if (lenDecrypted == 6 && memcmp(*decrypted, "SHIELD", 6) == 0) {
		sodium_free(*decrypted);

		randombytes_buf(addr, 18);
		if (isNormalBinAddress(addr)) return;
	} else {
		if (lenDecrypted > 24) return;
		int ret = addr2bin(*decrypted, lenDecrypted, addr);
		sodium_free(*decrypted);
		if (ret < 1) return;
	}

	const int64_t hash = addressToHash(addr, addrKey);
	if (addAddress(upk64, hash) != 0) return;

	char data[226];
	memcpy(data,
		"HTTP/1.1 200 aem\r\n"
		"Tk: N\r\n"
		"Strict-Transport-Security: max-age=94672800; includeSubDomains\r\n"
		"Expect-CT: enforce; max-age=94672800\r\n"
		"Connection: close\r\n"
		"Content-Length: 26\r\n"
		"Access-Control-Allow-Origin: *\r\n"
		"\r\n"
	, 200);
	memcpy(data + 200, &hash, 8);
	memcpy(data + 208, addr, 18);
	sendData(ssl, data, 226);
}

static void respond_https_addr_del(mbedtls_ssl_context * const ssl, const int64_t upk64, char * const * const decrypted, const size_t lenDecrypted) {
	if (lenDecrypted < 9) {free(*decrypted); return;}
	int64_t hash;
	memcpy(&hash, *decrypted, 8);

	const int ret = deleteAddress(upk64, hash, (unsigned char*)((*decrypted) + 8), lenDecrypted - 8);
	sodium_free(*decrypted);
	if (ret == 0) send204(ssl);
}

static void respond_https_addr_upd(mbedtls_ssl_context * const ssl, const int64_t upk64, char * const * const decrypted, const size_t lenDecrypted) {
	if (lenDecrypted < 1) {free(*decrypted); return;}

	const int ret = updateAddress(upk64, (unsigned char*)(*decrypted), lenDecrypted);
	sodium_free(*decrypted);
	if (ret == 0) send204(ssl);
}

static void respond_https_addr_set(mbedtls_ssl_context * const ssl, const int64_t upk64, char * const * const decrypted, const size_t lenDecrypted) {
	if (lenDecrypted < 1 || lenDecrypted % 9 != 0) {free(*decrypted); return;}

	const unsigned int addressCount = lenDecrypted / 9; // unsigned to avoid GCC warning
	unsigned char addrFlags[addressCount];
	int64_t addrHash[addressCount];

	for (unsigned int i = 0; i < addressCount; i++) {
		memcpy(addrFlags + i, (*decrypted) + i * 9, 1);
		memcpy(addrHash + i, (*decrypted) + i * 9 + 1, 8);
	}

	sodium_free(*decrypted);

	const int ret = updateAddressSettings(upk64, addrHash, addrFlags, addressCount);
	if (ret == 0) send204(ssl);
}

static void respond_https_delmsg(mbedtls_ssl_context * const ssl, const int64_t upk64, char * const * const decrypted, const size_t lenDecrypted) {
	uint8_t ids[lenDecrypted]; // 1 byte per ID
	for (size_t i = 0; i < lenDecrypted; i++) {
		ids[i] = (uint8_t)((*decrypted)[i]);
	}

	const int ret = deleteMessages(upk64, ids, (int)lenDecrypted);
	sodium_free(*decrypted);
	if (ret == 0) send204(ssl);
}

static void respond_https_gatekeeper(mbedtls_ssl_context * const ssl, const unsigned char * const upk, char * const * const decrypted, const size_t lenDecrypted, const unsigned char * const hashKey) {
	const int ret = updateGatekeeper(upk, *decrypted, lenDecrypted, hashKey);
	sodium_free(*decrypted);
	if (ret == 0) send204(ssl);
}

static void respond_https_notedata(mbedtls_ssl_context * const ssl, const int64_t upk64, char * const * const decrypted, const size_t lenDecrypted) {
	if (lenDecrypted != AEM_NOTEDATA_LEN + crypto_box_SEALBYTES) return;

	const int ret = updateNoteData(upk64, (unsigned char*)*decrypted);
	sodium_free(*decrypted);
	if (ret == 0) send204(ssl);
}

static void respond_https_addaccount(mbedtls_ssl_context * const ssl, const int64_t upk64, char * const * const decrypted, const size_t lenDecrypted) {
	if (lenDecrypted != crypto_box_PUBLICKEYBYTES) {sodium_free(*decrypted); return;}
	if (getUserLevel(upk64) < 3) {sodium_free(*decrypted); return;}

	const int ret = addAccount((unsigned char*)*decrypted);
	sodium_free(*decrypted);
	if (ret == 0) send204(ssl);
}

static void respond_https_destroyaccount(mbedtls_ssl_context * const ssl, const int64_t upk64, char * const * const decrypted, const size_t lenDecrypted) {
	if (lenDecrypted != 16) {sodium_free(*decrypted); return;}
	if (getUserLevel(upk64) < 3) {sodium_free(*decrypted); return;}

	unsigned char targetPk[8];
	const int ret = sodium_hex2bin(targetPk, 8, *decrypted, 16, NULL, NULL, NULL);
	sodium_free(*decrypted);
	if (ret != 0) return;

	if (destroyAccount(*((int64_t*)targetPk)) == 0) send204(ssl);
}

static void respond_https_accountlevel(mbedtls_ssl_context * const ssl, const int64_t upk64, char * const * const decrypted, const size_t lenDecrypted) {
	if (lenDecrypted != 17) {sodium_free(*decrypted); return;}
	if (getUserLevel(upk64) < 3) {sodium_free(*decrypted); return;}

	const int level = strtol(*decrypted + 16, NULL, 10);

	unsigned char targetPk[8];
	int ret = sodium_hex2bin(targetPk, 8, *decrypted, 16, NULL, NULL, NULL);
	sodium_free(*decrypted);
	if (ret != 0) return;

	ret = setAccountLevel(*((int64_t*)targetPk), level);
	if (ret == 0) send204(ssl);
}

static void handleGet(mbedtls_ssl_context * const ssl, const char * const url, const size_t lenUrl, const struct aem_fileSet * const fileSet, const char * const domain, const size_t lenDomain) {
	if (lenUrl == 0) return respond_https_html(ssl, "index.html", 10, fileSet->htmlFiles, fileSet->htmlCount, domain, lenDomain);
	if (lenUrl > 5 && memcmp(url + lenUrl - 5, ".html", 5) == 0) return respond_https_html(ssl, url, lenUrl, fileSet->htmlFiles, fileSet->htmlCount, domain, lenDomain);

	if (lenUrl > 4 && memcmp(url, "css/", 4) == 0) return respond_https_file(ssl, url + 4, lenUrl - 4, AEM_FILETYPE_CSS, fileSet->cssFiles, fileSet->cssCount);
	if (lenUrl > 4 && memcmp(url, "img/", 4) == 0) return respond_https_file(ssl, url + 4, lenUrl - 4, AEM_FILETYPE_IMG, fileSet->imgFiles, fileSet->imgCount);
	if (lenUrl > 3 && memcmp(url, "js/",  3) == 0) return respond_https_file(ssl, url + 3, lenUrl - 3, AEM_FILETYPE_JS,  fileSet->jsFiles,  fileSet->jsCount);
}

static void handlePost(mbedtls_ssl_context * const ssl, const unsigned char * const ssk, const unsigned char * const addrKey,
const char * const domain, const size_t lenDomain, const char * const url, const size_t lenUrl, const unsigned char * const post, const size_t lenPost) {
	if (lenUrl < 8) return;

	unsigned char upk[crypto_box_PUBLICKEYBYTES];
	size_t lenDecrypted;
	char * const decrypted = openWebBox(post, lenPost, upk, &lenDecrypted, ssk);
	if (decrypted == NULL) return;

	int64_t upk64;
	memcpy(&upk64, upk, 8);

	if (lenUrl ==  9 && memcmp(url, "api/login", 9) == 0) return respond_https_login(ssl, upk64, &decrypted, lenDecrypted);
	if (lenUrl ==  8 && memcmp(url, "api/send", 8) == 0) return respond_https_send(ssl, upk, domain, lenDomain, &decrypted, lenDecrypted, addrKey);
	if (lenUrl == 12 && memcmp(url, "api/textnote", 12) == 0) return respond_https_addnote(ssl, upk, &decrypted, lenDecrypted, false);
	if (lenUrl == 12 && memcmp(url, "api/filenote", 12) == 0) return respond_https_addnote(ssl, upk, &decrypted, lenDecrypted, true);

	if (lenUrl == 12 && memcmp(url, "api/addr/del", 12) == 0) return respond_https_addr_del(ssl, upk64, &decrypted, lenDecrypted);
	if (lenUrl == 12 && memcmp(url, "api/addr/add", 12) == 0) return respond_https_addr_add(ssl, upk64, &decrypted, lenDecrypted, addrKey);
	if (lenUrl == 12 && memcmp(url, "api/addr/upd", 12) == 0) return respond_https_addr_upd(ssl, upk64, &decrypted, lenDecrypted);
	if (lenUrl == 12 && memcmp(url, "api/addr/set", 12) == 0) return respond_https_addr_set(ssl, upk64, &decrypted, lenDecrypted);

	if (lenUrl == 10 && memcmp(url, "api/delmsg",     10) == 0) return respond_https_delmsg    (ssl, upk64, &decrypted, lenDecrypted);
	if (lenUrl == 12 && memcmp(url, "api/notedata",   12) == 0) return respond_https_notedata  (ssl, upk64, &decrypted, lenDecrypted);
	if (lenUrl == 14 && memcmp(url, "api/gatekeeper", 14) == 0) return respond_https_gatekeeper(ssl, upk, &decrypted, lenDecrypted, addrKey);

	if (lenUrl == 14 && memcmp(url, "api/addaccount", 14) == 0) return respond_https_addaccount(ssl, upk64, &decrypted, lenDecrypted);
	if (lenUrl == 16 && memcmp(url, "api/accountlevel", 16) == 0) return respond_https_accountlevel(ssl, upk64, &decrypted, lenDecrypted);
	if (lenUrl == 18 && memcmp(url, "api/destroyaccount", 18) == 0) return respond_https_destroyaccount(ssl, upk64, &decrypted, lenDecrypted);
}

int getRequestType(char * const req, size_t lenReq, const char * const domain, const size_t lenDomain) {
	if (lenReq < 14) return AEM_HTTPS_REQUEST_INVALID;

	char * const reqEnd = memmem(req, lenReq, "\r\n\r\n", 4);
	if (reqEnd == NULL) return AEM_HTTPS_REQUEST_INVALID;

	lenReq = reqEnd - req + 2; // Include \r\n at end
	if (memchr(req, '\0', lenReq) != NULL) return AEM_HTTPS_REQUEST_INVALID;
	reqEnd[2] = '\0';

	char header[11 + lenDomain];
	memcpy(header, "\r\nHost: ", 8);
	memcpy(header + 8, domain, lenDomain);
	strcpy(header + 8 + lenDomain, "\r\n");
	if (strcasestr(req, header) == NULL) return AEM_HTTPS_REQUEST_INVALID;

	if (strcasestr(req, " HTTP/1.1\r\n") == NULL) return AEM_HTTPS_REQUEST_INVALID;

	// Brotli compression support is required
	const char * const ae = strcasestr(req, "\r\nAccept-Encoding: ");
	if (ae == NULL) return AEM_HTTPS_REQUEST_INVALID;
	const char * const aeEnd = strpbrk(ae + 19, "\r\n");
	const char * const br = strcasestr(ae + 19, "br");
	if (br == NULL || br > aeEnd) return AEM_HTTPS_REQUEST_INVALID;
	if (*(br + 2) != ',' && *(br + 2) != ' ' && *(br + 2) != '\r') return AEM_HTTPS_REQUEST_INVALID;
	const char * const br1 = ae + (br - ae - 1); // br - 1
	if (*br1 != ',' && *br1 != ' ') return AEM_HTTPS_REQUEST_INVALID;

	// Forbidden request headers
	if (strcasestr(req, "\r\nAuthorization:") != NULL) return AEM_HTTPS_REQUEST_INVALID;
	if (strcasestr(req, "\r\nCookie:") != NULL) return AEM_HTTPS_REQUEST_INVALID;
	if (strcasestr(req, "\r\nExpect:") != NULL) return AEM_HTTPS_REQUEST_INVALID;
	if (strcasestr(req, "\r\nRange:") != NULL) return AEM_HTTPS_REQUEST_INVALID;
	if (strcasestr(req, "\r\nSec-Fetch-Site: same-site") != NULL) return AEM_HTTPS_REQUEST_INVALID;

	// These are only for preflighted requests which All-Ears doesn't use
	if (strcasestr(req, "\r\nAccess-Control-Request-Method:") != NULL) return AEM_HTTPS_REQUEST_INVALID;
	if (strcasestr(req, "\r\nAccess-Control-Request-Headers:") != NULL) return AEM_HTTPS_REQUEST_INVALID;

	if (memcmp(req, "GET /", 5) == 0) {
		if (strcasestr(req, "\r\nSec-Fetch-Mode: cors") != NULL) return AEM_HTTPS_REQUEST_INVALID;
		if (strcasestr(req, "\r\nSec-Fetch-Mode: websocket") != NULL) return AEM_HTTPS_REQUEST_INVALID;
		if (strcasestr(req, "\r\nSec-Fetch-Site: cross-site") != NULL) return AEM_HTTPS_REQUEST_INVALID;

		if (strcasestr(req, "\r\nContent-Length:") != NULL) return AEM_HTTPS_REQUEST_INVALID;
		if (strcasestr(req, "\r\nOrigin:") != NULL) return AEM_HTTPS_REQUEST_INVALID;
		if (strcasestr(req, "\r\nX-Requested-With:") != NULL) return AEM_HTTPS_REQUEST_INVALID;

		return AEM_HTTPS_REQUEST_GET;
	}

	if (memcmp(req, "POST /api/", 10) == 0) {
		const char * const cl = strcasestr(req, "\r\nContent-Length: ");
		if (cl == NULL) return AEM_HTTPS_REQUEST_INVALID;

		const int lenPost = strtol(cl + 18, NULL, 10);
		if (lenPost < 1) return AEM_HTTPS_REQUEST_INVALID;

		reqEnd[2] = '\r';
		return lenPost;
	}

	return AEM_HTTPS_REQUEST_INVALID;
}

int respond_https(int sock, mbedtls_x509_crt * const srvcert, mbedtls_pk_context * const pkey, const unsigned char * const ssk, const unsigned char * const addrKey,
const unsigned char * const seed, const char * const domain, const size_t lenDomain, const struct aem_fileSet * const fileSet) {
	// Setting up the SSL
	mbedtls_ssl_config conf;
	mbedtls_ssl_config_init(&conf);

	int ret;
	if ((ret = mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_SERVER, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT)) != 0) {
		printf("[HTTPS] mbedtls_ssl_config_defaults returned %d\n\n", ret);
		return -1;
	}

	mbedtls_ssl_conf_min_version(&conf, MBEDTLS_SSL_MAJOR_VERSION_3, MBEDTLS_SSL_MINOR_VERSION_3); // Require TLS v1.2+
	mbedtls_ssl_conf_read_timeout(&conf, AEM_HTTPS_TIMEOUT);
	mbedtls_ssl_conf_ciphersuites(&conf, https_ciphersuites);
	mbedtls_ssl_conf_curves(&conf, https_curves);
	mbedtls_ssl_conf_sig_hashes(&conf, https_hashes);

	// Seed the RNG
	mbedtls_ctr_drbg_context ctr_drbg;
	mbedtls_ctr_drbg_init(&ctr_drbg);

	mbedtls_entropy_context entropy;
	mbedtls_entropy_init(&entropy);

	if ((ret = mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy, seed, 16)) != 0) {
		printf("[HTTPS] mbedtls_ctr_drbg_seed returned %d\n", ret);
		return -1;
	}

	mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &ctr_drbg);

	mbedtls_ssl_conf_ca_chain(&conf, srvcert->next, NULL);
	if ((ret = mbedtls_ssl_conf_own_cert(&conf, srvcert, pkey)) != 0) {
		printf("[HTTPS] mbedtls_ssl_conf_own_cert returned %d\n", ret);
		return -1;
	}

	mbedtls_ssl_context ssl;
	mbedtls_ssl_init(&ssl);

	if ((ret = mbedtls_ssl_setup(&ssl, &conf)) != 0) {
		printf("[HTTPS] mbedtls_ssl_setup returned %d\n", ret);
		return -1;
	}

	mbedtls_ssl_set_bio(&ssl, &sock, mbedtls_net_send, mbedtls_net_recv, NULL);

	// Handshake
	while ((ret = mbedtls_ssl_handshake(&ssl)) != 0) {
		if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
			char errorBuf[100];
			mbedtls_strerror(ret, errorBuf, sizeof(errorBuf));
			printf("[HTTPS] mbedtls_ssl_handshake returned %d: %s\n", ret, errorBuf);
			mbedtls_ssl_free(&ssl);
			return -1;
		}
	}

	unsigned char * const req = malloc(AEM_HTTPS_MAXREQSIZE);
	int lenReq;
	do {lenReq = mbedtls_ssl_read(&ssl, req, AEM_HTTPS_MAXREQSIZE);} while (lenReq == MBEDTLS_ERR_SSL_WANT_READ);

	if (lenReq > 0) {
		const int lenReqBody = getRequestType((char*)req, lenReq, domain, lenDomain);

		if (lenReqBody >= AEM_HTTPS_REQUEST_GET) {
			const char * const reqUrl = (char*)(req + ((lenReqBody == AEM_HTTPS_REQUEST_GET) ? 5 : 6));
			const char * const ruEnd = strchr(reqUrl, ' ');
			const size_t lenReqUrl = (ruEnd == NULL) ? 0 : ruEnd - reqUrl;

			if (lenReqBody == AEM_HTTPS_REQUEST_GET) {
				handleGet(&ssl, (char*)reqUrl, lenReqUrl, fileSet, domain, lenDomain);
			} else { // POST
				const unsigned char *post = memmem(req + lenReqUrl + 11, lenReq, "\r\n\r\n", 4);

				if (post != NULL) {
					post += 4;

					if ((post - req) + lenReqBody < AEM_HTTPS_MAXREQSIZE) {
						int lenPost = lenReq - (post - req);

						ret = 1;

						while (lenPost < lenReqBody) {
							do {ret = mbedtls_ssl_read(&ssl, req + lenReq, AEM_HTTPS_MAXREQSIZE - lenReq);} while (ret == MBEDTLS_ERR_SSL_WANT_READ);
							lenPost += ret;
							lenReq += ret;
						}

						if (ret > 0)
							handlePost(&ssl, ssk, addrKey, domain, lenDomain, reqUrl, lenReqUrl, post, lenPost);
					}
				}
			}
		} else puts("[HTTPS] Invalid connection attempt");
	}

	free(req);
	mbedtls_entropy_free(&entropy);
	mbedtls_ctr_drbg_free(&ctr_drbg);
	mbedtls_ssl_config_free(&conf);
	mbedtls_ssl_free(&ssl);
	return 0;
}
