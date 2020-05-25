// CertCrypt: Encrypts TLS cert/key files for All-Ears Mail

#include <ctype.h> // for isxdigit
#include <fcntl.h> // for open
#include <stdio.h>
#include <string.h>
#include <unistd.h> // for write

#include <sodium.h>

#include "GetKey.h"

#define AEM_MAXSIZE_FILE 8192

unsigned char master[crypto_secretbox_KEYBYTES];

int main(int argc, char *argv[]) {
	puts("CertCrypt: Encrypt TLS certificate and private key files for All-Ears Mail");

	if (argc < 2 || strlen(argv[1]) < 5 || (strcmp(argv[1] + strlen(argv[1]) - 4, ".key") != 0 && strcmp(argv[1] + strlen(argv[1]) - 4, ".crt") != 0)) {
		puts("Terminating: Use .key or .crt file as parameter");
		return EXIT_FAILURE;
	}

	if (sodium_init() < 0) {
		puts("Terminating: Failed initializing libsodium");
		return EXIT_FAILURE;
	}

	if (getKey(master) != 0) {
		puts("Terminating: Failed reading key");
		return EXIT_FAILURE;
	}

	int fd = open(argv[1], O_RDONLY);
	if (fd < 0) {
		sodium_memzero(master, crypto_secretbox_KEYBYTES);
		puts("Terminating: Failed opening file");
		return EXIT_FAILURE;
	}

	unsigned char buf[AEM_MAXSIZE_FILE + 1];
	off_t bytes = read(fd, buf, AEM_MAXSIZE_FILE);
	close(fd);

	if (bytes < 1) {
		sodium_memzero(master, crypto_secretbox_KEYBYTES);
		puts("Terminating: Failed reading file");
		return EXIT_FAILURE;
	}

	buf[bytes] = '\0';
	bytes++;

	const size_t lenEncrypted = bytes + crypto_secretbox_MACBYTES;
	unsigned char encrypted[lenEncrypted];

	unsigned char nonce[crypto_secretbox_NONCEBYTES];
	randombytes_buf(nonce, crypto_secretbox_NONCEBYTES);

	crypto_secretbox_easy(encrypted, buf, bytes, nonce, master);
	sodium_memzero(master, crypto_secretbox_KEYBYTES);
	sodium_memzero(buf, bytes);

	char path[strlen(argv[1]) + 5];
	sprintf(path, "%s.enc", argv[1]);

	fd = open(path, O_WRONLY | O_CREAT | O_EXCL, S_IRUSR);
	if (fd < 0) {
		printf("Terminating: Failed creating %s\n", path);
		return EXIT_FAILURE;
	}

	ssize_t ret = write(fd, nonce, crypto_secretbox_NONCEBYTES);
	ret += write(fd, encrypted, lenEncrypted);
	close(fd);

	if ((unsigned long)ret != crypto_secretbox_NONCEBYTES + lenEncrypted) {
		printf("Terminating: Failed writing %s\n", path);
		return EXIT_FAILURE;
	}

	printf("Created %s\n", path);
	return EXIT_SUCCESS;
}
