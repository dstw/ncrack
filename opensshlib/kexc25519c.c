/* $OpenBSD: kexc25519c.c,v 1.7 2015/01/26 06:10:03 djm Exp $ */
/*
 * Copyright (c) 2001 Markus Friedl.  All rights reserved.
 * Copyright (c) 2010 Damien Miller.  All rights reserved.
 * Copyright (c) 2013 Aris Adamantiadis.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "includes.h"

#include <sys/types.h>

#include <stdio.h>
#include <string.h>
#include <signal.h>

#include "sshkey.h"
#include "cipher.h"
#include "kex.h"
#include "log.h"
#include "packet.h"
#include "ssh2.h"
#include "sshbuf.h"
#include "digest.h"
#include "ssherr.h"

//static int input_kex_c25519_reply(int type, u_int32_t seq, void *ctxt);

int
kexc25519_client(ncrack_ssh_state *nstate)
{
	struct kex *kex = nstate->kex;
	int r;

  //printf("kexc 25519 client\n");

	kexc25519_keygen(kex->c25519_client_key, kex->c25519_client_pubkey);
#ifdef DEBUG_KEXECDH
	dump_digest("client private key:", kex->c25519_client_key,
	    sizeof(kex->c25519_client_key));
#endif
	if ((r = sshpkt_start(nstate, SSH2_MSG_KEX_ECDH_INIT)) != 0 ||
	    (r = sshpkt_put_string(nstate, kex->c25519_client_pubkey,
	    sizeof(kex->c25519_client_pubkey))) != 0 ||
	    (r = sshpkt_send(nstate)) != 0)
		return r;

	debug("expecting SSH2_MSG_KEX_ECDH_REPLY");
	//ssh_dispatch_set(nstate, SSH2_MSG_KEX_ECDH_REPLY, &input_kex_c25519_reply);
	return 0;
}

int
ncrackssh_input_kex_c25519_reply(ncrack_ssh_state *nstate)
{
	//struct ssh *ssh = ctxt;
	struct kex *kex = nstate->kex;
	struct sshkey *server_host_key = NULL;
	struct sshbuf *shared_secret = NULL;
	u_char *server_pubkey = NULL;
	u_char *server_host_key_blob = NULL, *signature = NULL;
	u_char hash[SSH_DIGEST_MAX_LENGTH];
	size_t slen, pklen, sbloblen, hashlen;
	int r;

  //printf("kex c25519 reply\n");

	if (kex->verify_host_key == NULL) {
		r = SSH_ERR_INVALID_ARGUMENT;
		goto out;
	}

	/* hostkey */
	if ((r = sshpkt_get_string(nstate, &server_host_key_blob,
	    &sbloblen)) != 0 ||
	    (r = sshkey_from_blob(server_host_key_blob, sbloblen,
	    &server_host_key)) != 0)
		goto out;
	if (server_host_key->type != kex->hostkey_type ||
	    (kex->hostkey_type == KEY_ECDSA &&
	    server_host_key->ecdsa_nid != kex->hostkey_nid)) {
		r = SSH_ERR_KEY_TYPE_MISMATCH;
		goto out;
	}
	if (kex->verify_host_key(server_host_key, nstate) == -1) {
		r = SSH_ERR_SIGNATURE_INVALID;
		goto out;
	}

	/* Q_S, server public key */
	/* signed H */
	if ((r = sshpkt_get_string(nstate, &server_pubkey, &pklen)) != 0 ||
	    (r = sshpkt_get_string(nstate, &signature, &slen)) != 0 ||
	    (r = sshpkt_get_end(nstate)) != 0)
		goto out;
	if (pklen != CURVE25519_SIZE) {
		r = SSH_ERR_SIGNATURE_INVALID;
		goto out;
	}

#ifdef DEBUG_KEXECDH
	dump_digest("server public key:", server_pubkey, CURVE25519_SIZE);
#endif

	if ((shared_secret = sshbuf_new()) == NULL) {
		r = SSH_ERR_ALLOC_FAIL;
		goto out;
	}
	if ((r = kexc25519_shared_key(kex->c25519_client_key, server_pubkey,
	    shared_secret)) < 0)
		goto out;

	/* calc and verify H */
	hashlen = sizeof(hash);
	if ((r = kex_c25519_hash(
	    kex->hash_alg,
	    kex->client_version_string,
	    kex->server_version_string,
	    sshbuf_ptr(kex->my), sshbuf_len(kex->my),
	    sshbuf_ptr(kex->peer), sshbuf_len(kex->peer),
	    server_host_key_blob, sbloblen,
	    kex->c25519_client_pubkey,
	    server_pubkey,
	    sshbuf_ptr(shared_secret), sshbuf_len(shared_secret),
	    hash, &hashlen)) < 0)
		goto out;

	if ((r = sshkey_verify(server_host_key, signature, slen, hash, hashlen,
	    nstate->compat)) != 0)
		goto out;

	/* save session id */
	if (kex->session_id == NULL) {
		kex->session_id_len = hashlen;
		kex->session_id = malloc(kex->session_id_len);
		if (kex->session_id == NULL) {
			r = SSH_ERR_ALLOC_FAIL;
			goto out;
		}
		memcpy(kex->session_id, hash, kex->session_id_len);
	}

	if ((r = kex_derive_keys(nstate, hash, hashlen, shared_secret)) == 0) {
    //printf("kex send newkeys 25519\n");
		r = kex_send_newkeys(nstate);
  }
out:
	explicit_bzero(hash, sizeof(hash));
	explicit_bzero(kex->c25519_client_key, sizeof(kex->c25519_client_key));
	free(server_host_key_blob);
	free(server_pubkey);
	free(signature);
	sshkey_free(server_host_key);
	sshbuf_free(shared_secret);
	return r;
}
