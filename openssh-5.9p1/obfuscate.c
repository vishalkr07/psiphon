#include "includes.h"
#include <openssl/evp.h>
#include <openssl/rc4.h>
#include <unistd.h>

#include "atomicio.h"
#include "xmalloc.h"
#include "log.h"
#include "obfuscate.h"
#include <string.h>

// PSIPHON: HTTP-PREFIX
int use_obfuscation_prefix = 0;

static RC4_KEY rc4_input;
static RC4_KEY rc4_output;

static const char *obfuscate_keyword = NULL;

#define OBFUSCATE_KEY_LENGTH 	16
#define OBFUSCATE_SEED_LENGTH	16
#define OBFUSCATE_HASH_ITERATIONS 6000
#define OBFUSCATE_MAX_PADDING	8192
#define OBFUSCATE_MAGIC_VALUE	0x0BF5CA7E

struct seed_msg {
	u_char seed_buffer[OBFUSCATE_SEED_LENGTH];
	u_int32_t magic;
	u_int32_t padding_length;
	u_char padding[];
};

static void generate_key_pair(const u_char *, u_char *, u_char *, int);
static void generate_key(const u_char *, const u_char *, u_int, u_char *, int);
static void set_keys(const u_char *, const u_char *);
static void initialize(const u_char *, int, int);
static void read_forever(int);

// PSIPHON: HTTP-PREFIX
int skip_prefix(int sock_in, u_char* previously_read_bytes, u_int previously_read_len)
{
	// Note: return value is offset into previously_read_bytes after prefix ends,
	// which indicates to obfuscate_receive_seed how many bytes it needs to read
	// to complete the seed message.

	int i;
	int has_method = 0;
	u_int previously_read_offset;
	const char* prefix_terminator = "\r\n\r\n";
	int prefix_terminator_offset = 0;
	u_char next_byte;

	const char* methods[] = {
		"OPTIONS", "GET", "HEAD", "POST", "PUT", "DELETE", "TRACE", "CONNECT"
	};

	// longest method is 7 bytes
	if (previously_read_len < 7) {
		return 0;
	}

	// backwards compatibility special case
	if (previously_read_len >= 16 &&
		0 == memcmp(previously_read_bytes, "GET / HTTP/1.0\r\n", 16)) {
		return 0;
	}

	for (i = 0; i < sizeof(methods)/sizeof(const char*); i++) {
		u_int method_len = strlen(methods[i]);
		if (0 == memcmp(previously_read_bytes, methods[i], method_len)) {
			has_method = 1;
			previously_read_offset = method_len;
			break;
		}
	}

	if (!has_method) {
		return 0;
	}

	// Skip all bytes up to and including the prefix terminator, <CR><LF><CR><LF>
	// Check in previously read buffer first, then read more bytes

	// Behavior when client sends an infinite prefix is the same as when
	// the client sends an invalie obfuscation message (read_forever)

	for (;;) {

		if (previously_read_offset < previously_read_len) {
			next_byte = previously_read_bytes[previously_read_offset++];
		} else {
			if (1 != atomicio(read, sock_in, &next_byte, 1)) {
				fatal("skip_prefix: read failed");
			}
		}

		if (next_byte == prefix_terminator[prefix_terminator_offset]) {
			prefix_terminator_offset++;
			if (prefix_terminator_offset == strlen(prefix_terminator)) {
				break;
			}
		} else {
			prefix_terminator_offset = 0;
		}
	}

	// server should prefix its response, as client will now expect it
	use_obfuscation_prefix = 1;

	return previously_read_offset;
}


/*
 * Server calls this
 */
void
obfuscate_receive_seed(int sock_in)
{
	struct seed_msg seed;

	u_char padding_drain[OBFUSCATE_MAX_PADDING];
	u_int len;
	u_int32_t padding_length;
	u_int32_t test_magic;
	u_int offset, remaining;

	len = atomicio(read, sock_in, &seed, sizeof(struct seed_msg));

	debug2("obfuscate_receive_seed: read %d byte seed message from client", len);
	if(len != sizeof(struct seed_msg))
		fatal("obfuscate_receive_seed: read failed");

	// PSIPHON: HTTP-PREFIX
	offset = skip_prefix(sock_in, (u_char*)&seed, sizeof(struct seed_msg));
	if (offset > 0)
	{
		remaining = len - offset;
		memmove(&seed, ((unsigned char*)&seed) + offset, remaining);
		len = remaining + atomicio(read, sock_in, ((unsigned char*)&seed) + remaining, sizeof(struct seed_msg) - remaining);

		debug2("obfuscate_receive_seed: read %d byte seed message from client", len);
		if(len != sizeof(struct seed_msg))
			fatal("obfuscate_receive_seed: reread failed");
	}

	initialize(seed.seed_buffer, 1, 1); // try fixed key pair first

	// create a copy of seed.magic because obfuscate_input(..) destroys it
	test_magic = seed.magic;
	obfuscate_input((u_char *)&test_magic, 4);
	if(OBFUSCATE_MAGIC_VALUE != ntohl(test_magic)) {
		debug2("trying ossh backwards compatibility mode");
		initialize(seed.seed_buffer, 1, 0); // try backwards compatible key pair
		obfuscate_input((u_char *)&seed.magic, 4);
		if(OBFUSCATE_MAGIC_VALUE != ntohl(seed.magic)) {
			logit("Magic value check failed (%u) on obfuscated handshake.", ntohl(seed.magic));
			read_forever(sock_in);
		}
	} else {
		seed.magic = test_magic;
	}

	obfuscate_input((u_char *)&seed.padding_length, 4);
	padding_length = ntohl(seed.padding_length);
	if(padding_length > OBFUSCATE_MAX_PADDING) {
		logit("Illegal padding length %d for obfuscated handshake", ntohl(seed.padding_length));
		read_forever(sock_in);
	}
	len = atomicio(read, sock_in, padding_drain, padding_length);
	if(len != padding_length)
		fatal("obfuscate_receive_seed: read failed");
	debug2("obfuscate_receive_seed: read %d bytes of padding from client.", len);
	obfuscate_input(padding_drain, padding_length);
}

/*
 * Client calls this
 */
void
obfuscate_send_seed(int sock_out)
{
	// PSIPHON: HTTP-PREFIX
	const char* prefix = "POST / HTTP/1.1\r\n\r\n";
	u_int prefix_length = strlen(prefix);

	struct seed_msg *seed;
	int i;
	u_int32_t rnd = 0;
	u_int message_length;
	u_int padding_length;

	padding_length = arc4random() % OBFUSCATE_MAX_PADDING;
	message_length = padding_length + sizeof(struct seed_msg);
	seed = xmalloc(message_length);

	for(i = 0; i < OBFUSCATE_SEED_LENGTH; i++) {
		if(i % 4 == 0)
			rnd = arc4random();
		seed->seed_buffer[i] = rnd & 0xff;
		rnd >>= 8;
	}
	seed->magic = htonl(OBFUSCATE_MAGIC_VALUE);
	seed->padding_length = htonl(padding_length);
	for(i = 0; i < (int)padding_length; i++) {
		if(i % 4 == 0)
			rnd = arc4random();
		seed->padding[i] = rnd & 0xff;
	}
	initialize(seed->seed_buffer, 0, 1); //always generate fixed key pair
	obfuscate_output(((u_char *)seed) + OBFUSCATE_SEED_LENGTH,
		message_length - OBFUSCATE_SEED_LENGTH);
	debug2("obfuscate_send_seed: Sending seed message with %d bytes of padding", padding_length);

	atomicio(vwrite, sock_out, (char*)prefix, prefix_length);
	atomicio(vwrite, sock_out, seed, message_length);

	xfree(seed);
}

void
obfuscate_set_keyword(const char *keyword)
{
	debug2("obfuscate_set_keyword: Setting obfuscation keyword to '%s'", keyword);
	obfuscate_keyword = keyword;
}

void
obfuscate_input(u_char *buffer, u_int buffer_len)
{
	RC4(&rc4_input, buffer_len, buffer, buffer);
}

void
obfuscate_output(u_char *buffer, u_int buffer_len)
{
	RC4(&rc4_output, buffer_len, buffer, buffer);
}

static void
initialize(const u_char *seed, int server, int ossh_key_fix)
{
	u_char client_to_server_key[OBFUSCATE_KEY_LENGTH];
	u_char server_to_client_key[OBFUSCATE_KEY_LENGTH];
	
	generate_key_pair(seed, client_to_server_key, server_to_client_key, ossh_key_fix);

	if(server)
		set_keys(client_to_server_key, server_to_client_key);
	else
		set_keys(server_to_client_key, client_to_server_key);
}

static void
generate_key_pair(const u_char *seed, u_char *client_to_server_key, u_char *server_to_client_key, int ossh_key_fix)
{
	generate_key(seed, "client_to_server", strlen("client_to_server"), client_to_server_key, ossh_key_fix);
	generate_key(seed, "server_to_client", strlen("server_to_client"), server_to_client_key, ossh_key_fix);
}

static void
generate_key(const u_char *seed, const u_char *iv, u_int iv_len, u_char *key_data, int ossh_key_fix)
{
	EVP_MD_CTX ctx;
	u_char md_output[EVP_MAX_MD_SIZE];
	int md_len;
	int i;
	u_char *buffer;
	u_char *p;
	u_int buffer_length;

	buffer_length = OBFUSCATE_SEED_LENGTH + iv_len;
	if(obfuscate_keyword)
		buffer_length += strlen(obfuscate_keyword);

	p = buffer = xmalloc(buffer_length);

	memcpy(p, seed, OBFUSCATE_SEED_LENGTH);
	p += OBFUSCATE_SEED_LENGTH;

	if(obfuscate_keyword) {
		memcpy(p, obfuscate_keyword, strlen(obfuscate_keyword));
		p += strlen(obfuscate_keyword);
	}
	memcpy(p, iv, iv_len);

	EVP_DigestInit(&ctx, EVP_sha1());
	if(ossh_key_fix)
	{
		EVP_DigestUpdate(&ctx, buffer, buffer_length);
	}
	else
	{
		EVP_DigestUpdate(&ctx, buffer, OBFUSCATE_SEED_LENGTH + iv_len);
	}
	EVP_DigestFinal(&ctx, md_output, &md_len);

	xfree(buffer);

	for(i = 0; i < OBFUSCATE_HASH_ITERATIONS; i++) {
		EVP_DigestInit(&ctx, EVP_sha1());
		EVP_DigestUpdate(&ctx, md_output, md_len);
		EVP_DigestFinal(&ctx, md_output, &md_len);
	}

	if(md_len < OBFUSCATE_KEY_LENGTH) 
		fatal("Cannot derive obfuscation keys from hash length of %d", md_len);

	memcpy(key_data, md_output, OBFUSCATE_KEY_LENGTH);
}

static void
set_keys(const u_char *input_key, const u_char *output_key)
{
	RC4_set_key(&rc4_input, OBFUSCATE_KEY_LENGTH, input_key);
	RC4_set_key(&rc4_output, OBFUSCATE_KEY_LENGTH, output_key);
}

static void
read_forever(int sock_in)
{
	u_char discard_buffer[1024];

	while(atomicio(read, sock_in, discard_buffer, sizeof(discard_buffer)) > 0)
		;
	cleanup_exit(255);
}
