#include <arpa/inet.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <syslog.h>

#include <sodium.h>

#include "bit.h"

static unsigned char id[2];
static unsigned char question[256];
static size_t lenQuestion;

static uint32_t validIp(const uint32_t ip) {
	const uint8_t b1 = ip & 0xFF;
	const uint8_t b2 = (ip >>  8) & 0xFF;
	const uint8_t b3 = (ip >> 16) & 0xFF;
//	const uint8_t b4 = (ip >> 24) & 0xFF;

	return (
	   (b1 == 0)
	|| (b1 == 10)
	|| (b1 == 100 && b2 >= 64 && b2 <= 127)
	|| (b1 == 127)
	|| (b1 == 169 && b2 == 254)
	|| (b1 == 172 && b2 >= 16 && b2 <= 31)
	|| (b1 == 192 && b2 == 0  && b3 == 0)
	|| (b1 == 192 && b2 == 0  && b3 == 2)
	|| (b1 == 192 && b2 == 88 && b3 == 99)
	|| (b1 == 192 && b2 == 168)
	|| (b1 == 198 && b2 >= 18 && b2 <= 19)
	|| (b1 == 198 && b2 == 51 && b3 == 100)
	|| (b1 == 203 && b2 == 0  && b3 == 113)
	|| (b1 >= 224 && b1 <= 239)
	|| (b1 >= 240)
	) ? 0 : ip;
}

int dnsCreateRequest(unsigned char * const rq, const unsigned char * const domain, const size_t lenDomain, const bool typeMx) {
	lenQuestion = 0;

	// Bytes 1-2: Transaction ID.
	randombytes_buf(id, 2);
	memcpy(rq + 2, id, 2);

	setBit(rq + 4, 1, 0); // Byte 3, Bit 1: QR (Query/Response). 0 = Query, 1 = Response.

	// Byte 3, Bits 2-5 (4 bits): OPCODE (kind of query). 0000 = Standard query.
	setBit(rq + 4, 2, 0);
	setBit(rq + 4, 3, 0);
	setBit(rq + 4, 4, 0);
	setBit(rq + 4, 5, 0);

	// Byte 3: Bits 6-8; Byte 4, Bits 1-4
	setBit(rq + 4, 6, 0); // Byte 3, Bit 6: Authoritative answer. N/A.
	setBit(rq + 4, 7, 0); // Byte 3, Bit 7: Truncated message.
	setBit(rq + 4, 8, 1); // Byte 3, Bit 8: Recursion desired.
	setBit(rq + 5, 1, 0); // Byte 4, Bit 1: Recursion available. N/A.
	setBit(rq + 5, 2, 0); // Byte 4. Bit 2: Reserved. Must be 0.
	setBit(rq + 5, 3, 0); // Byte 4. Bit 3: Reserved. Must be 0.
	setBit(rq + 5, 4, 0); // Byte 4. Bit 4: Reserved. Must be 0.

	// Response code. N/A.
	setBit(rq + 5, 5, 0); // Byte 4. Bit 5.
	setBit(rq + 5, 6, 0); // Byte 4. Bit 6.
	setBit(rq + 5, 7, 0); // Byte 4. Bit 7.
	setBit(rq + 5, 8, 0); // Byte 4. Bit 8.

	// Bytes 5-6: QDCOUNT: Number of entries in the question section.
	rq[6] = 0;
	rq[7] = 1;

	memset(rq +  8, 0, 2); // Bytes 7-8: ANCOUNT: Number of resource records in the answer section. N/A.
	memset(rq + 10, 0, 2); // Bytes 9-10: NSCOUNT: Number of name server resource records in the authority records section. N/A.
	memset(rq + 12, 0, 2); // Bytes 11-12: ARCOUNT: Number of resource records in the additional records section. N/A.

	// Bytes 13+: Question section

	// Convert domain name to question format
	const unsigned char *dom = domain;

	while(1) {
		bool final = false;

		const unsigned char *dot = memchr(dom, '.', (domain + lenDomain) - dom);
		if (dot == NULL) {
			dot = domain + lenDomain;
			final = true;
		}

		size_t sz = dot - dom;

		question[lenQuestion] = sz;
		memcpy(question + lenQuestion + 1, dom, sz);

		lenQuestion += sz + 1;
		dom += sz + 1;

		if (final) break;
	}

	if (typeMx)
		memcpy(question + lenQuestion, "\x00\x00\x0F\x00\x01", 5); // 00: end of question; 000F: MX record; 0001: Internet question class
	else
		memcpy(question + lenQuestion, "\x00\x00\x01\x00\x01", 5); // 00: end of question; 0001: A record;  0001: Internet question class

	lenQuestion += 5;

	memcpy(rq + 14, question, lenQuestion);

	// TCP DNS messages start with a 16 bit integer containing the length of the message (not counting the integer itself)
	rq[0] = 0;
	rq[1] = 17 + lenQuestion;

	return 19 + lenQuestion;
}

int rr_getName(const unsigned char * const msg, const int lenMsg, const int rrOffset, unsigned char * const name, int * const lenName, bool allowPointer) {
	int offset = rrOffset;

	while (offset < lenMsg) {
		switch (msg[offset] & 192) {
			case 192: { // Pointer (ends label)
				if (!allowPointer) {syslog(LOG_ERR, "DNS: Pointer-to-pointer"); return -1;}
				const unsigned char tmp[] = {msg[offset + 1], msg[offset] & 63};
				const uint16_t p = *((uint16_t*)tmp);

				rr_getName(msg, lenMsg, p, name, lenName, false);
				return offset + 2;
			break;}
			case 0: { // Normal
				allowPointer = true;
				if (msg[offset] == 0) return offset + 1; // Label end

				// Label part
				if (*lenName > 0) {
					name[*lenName] = '.';
					(*lenName)++;
				}

				memcpy(name + *lenName, msg + offset + 1, msg[offset]);
				*lenName += msg[offset];
				offset += msg[offset] + 1;
				continue;
			break;}
			default: // 128, 64: reserved
				syslog(LOG_ERR, "128/64");
				return -1;
		}
	}

	syslog(LOG_ERR, "No_End");
	return -1;
}

static int getMx(const unsigned char * const msg, const int lenMsg, int rrOffset, const int answerCount, unsigned char * const mxDomain, int * const lenMxDomain) {
	uint16_t prio = UINT16_MAX;

	for (int i = 0; i < answerCount; i++) {
		unsigned char name[255];
		int lenName = 0;

		const int offset = rr_getName(msg, lenMsg, rrOffset, name, &lenName, true);
		if (offset < 1) {syslog(LOG_ERR, "os=%d", offset); return -1;}
		// TODO: Compare name to requestedName

		if (memcmp(msg + offset + 0, "\x00\x0F", 2) != 0) {syslog(LOG_ERR, "Non_MX"); return -1;} // Non-MX record
		if (memcmp(msg + offset + 2, "\x00\x01", 2) != 0) {syslog(LOG_ERR, "Non_IN"); return -1;} // Non-Internet class
		// +4 TTL (32 bits) ignored

		uint16_t mxLen;
		memcpy((unsigned char*)&mxLen + 0, msg + offset + 9, 1);
		memcpy((unsigned char*)&mxLen + 1, msg + offset + 8, 1);
		if (mxLen < 1) {syslog(LOG_ERR, "mxLen"); return -1;}

		uint16_t newPrio;
		memcpy((unsigned char*)&newPrio + 0, msg + offset + 11, 1);
		memcpy((unsigned char*)&newPrio + 1, msg + offset + 10, 1);

		if (newPrio < prio) {
			*lenMxDomain = 0;
			rr_getName(msg, lenMsg, offset + 12, mxDomain, lenMxDomain, true);
			prio = newPrio;
		}

		rrOffset = offset + 10 + mxLen; // offset is at byte after name-section
	}

	return 0;
}

static uint32_t dnsResponse_GetIp_get(const unsigned char * const rr, const int rrLen) {
	int offset = 0;
	bool pointer = false;

	while (offset < rrLen) {
		uint8_t lenLabel = rr[offset];

		if (pointer || lenLabel == 0) {
			if (!pointer) offset++;
			pointer = false;

			uint16_t lenRecord;
			memcpy((unsigned char*)&lenRecord + 0, rr + offset + 9, 1);
			memcpy((unsigned char*)&lenRecord + 1, rr + offset + 8, 1);

			if (memcmp(rr + offset, "\0\1\0\1", 4) == 0 && lenRecord == 4) { // A Record
				uint32_t ip;
				memcpy(&ip, rr + offset + 10, 4);
				return ip;
			} else {
				offset += 10 + lenRecord;
				continue;
			}
		} else if ((lenLabel & 192) == 192) {
			offset += 2;
			pointer = true;
			continue;
		}

		offset += 1 + lenLabel;
	}

	return 0;
}

uint32_t dnsResponse_GetIp(const unsigned char * const res, const int resLen) {
	if (memcmp(res, id, 2) != 0) {syslog(LOG_ERR, "Invalid ID"); return 0;}
	if ((res[3] & 15) != 0) {syslog(LOG_ERR, "Err=%u", res[3] & 15); return 0;}
	if (memcmp(res + 4, "\0\1", 2) != 0) {syslog(LOG_ERR, "Question count mismatch"); return 0;}
// +8: NSCount
// +10: ARCount
	if (memcmp(res + 12, question, lenQuestion) != 0) {syslog(LOG_ERR, "Question section mismatch"); return 0;}

	uint16_t answerCount;
	memcpy((unsigned char*)&answerCount + 0, res + 7, 1);
	memcpy((unsigned char*)&answerCount + 1, res + 6, 1);
	if (answerCount < 1) return 0;

	return validIp(dnsResponse_GetIp_get(res + 12 + lenQuestion, resLen - 12 - lenQuestion));
}

int dnsResponse_GetMx(const unsigned char * const res, const int resLen, unsigned char * const mxDomain, int * const lenMxDomain) {
	if (memcmp(res, id, 2) != 0) {syslog(LOG_ERR, "Invalid ID"); return 0;}
	if ((res[3] & 15) != 0) {syslog(LOG_ERR, "Err=%u", res[3] & 15); return 0;}
	if (memcmp(res + 4, "\0\1", 2) != 0) {syslog(LOG_ERR, "Question count mismatch"); return 0;}
// +8: NSCount
// +10: ARCount
	if (memcmp(res + 12, question, lenQuestion) != 0) {syslog(LOG_ERR, "Question section mismatch"); return 0;}

	uint16_t answerCount;
	memcpy((unsigned char*)&answerCount + 0, res + 7, 1);
	memcpy((unsigned char*)&answerCount + 1, res + 6, 1);
	if (answerCount < 1) return 0;

	return getMx(res, resLen, 12 + lenQuestion, answerCount, mxDomain, lenMxDomain);
}
