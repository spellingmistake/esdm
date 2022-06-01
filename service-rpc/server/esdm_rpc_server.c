/*
 * Copyright (C) 2022, Stephan Mueller <smueller@chronox.de>
 *
 * License: see LICENSE file in root directory
 *
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE, ALL OF
 * WHICH ARE HEREBY DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT
 * OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE
 * USE OF THIS SOFTWARE, EVEN IF NOT ADVISED OF THE POSSIBILITY OF SUCH
 * DAMAGE.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <protobuf-c/protobuf-c.h>
#include <semaphore.h>
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/shm.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <string.h>
#include <unistd.h>

#include "atomic.h"
#include "conv_be_le.h"
#include "config.h"
#include "esdm_config.h"
#include "esdm_rpc_protocol.h"
#include "esdm_rpc_server.h"
#include "esdm_rpc_service.h"
#include "helper.c"
#include "logger.h"
#include "memset_secure.h"
#include "privileges.h"
#include "ret_checkers.h"
#include "threading_support.h"

struct esdm_rpcs {
	ProtobufCService *service;
	int server_listening_fd;
};

struct esdm_rpcs_connection {
	struct esdm_rpcs *proto;
	int child_fd;
	ProtobufCAllocator *rpc_allocator;
	uint32_t method_index;
	uint32_t request_id;
};

struct esdm_rpcs_write_buf {
	ProtobufCBuffer base;
	struct esdm_rpcs_connection *rpc_conn;
};

static DECLARE_WAIT_QUEUE(esdm_rpc_thread_init_wait);

static pid_t server_pid = -1;
static atomic_t server_exit = ATOMIC_INIT(0);

static void esdm_rpcs_stale_socket(const char *path, struct sockaddr *addr,
				   unsigned addr_len)
{
	struct stat statbuf;
	int fd;

	if (stat(path, &statbuf) < 0)
		return;
	if (!S_ISSOCK(statbuf.st_mode))
		return;

	fd = socket(PF_UNIX, SOCK_STREAM, 0);
	if (fd < 0)
		return;
	set_fd_nonblocking(fd);
	if (connect(fd, addr, addr_len) < 0)
	{
		if (errno == EINPROGRESS) {
			close(fd);
			return;
		}
	} else {
		close(fd);
		return;
	}

	/* ok, we should delete the stale socket */
	close(fd);
	unlink(path);
}

static int esdm_rpcs_write_data(struct esdm_rpcs_connection *rpc_conn,
				const uint8_t *data, size_t len)
{
	size_t written = 0;
	ssize_t ret;

	if (rpc_conn->child_fd < 0)
		return -EINVAL;

	do {
		ret = write(rpc_conn->child_fd, data, len);
		if (ret < 0) {
			int errsv = errno;

			logger(LOGGER_VERBOSE, LOGGER_C_RPC,
			       "Writting of data to file descriptor %d failed: %s\n",
			       rpc_conn->child_fd, strerror(errsv));
			return -errsv;
		}

		written += (size_t)ret;
	} while (written < len);

	logger(LOGGER_DEBUG2, LOGGER_C_ANY, "%zu bytes written\n", len);

	return 0;
}

static void esdm_rpcs_append_data(ProtobufCBuffer *buffer, size_t len,
				  const uint8_t *data)
{
	struct esdm_rpcs_write_buf *buf = (struct esdm_rpcs_write_buf *) buffer;

	esdm_rpcs_write_data(buf->rpc_conn, data, len);
}

static int esdm_rpcs_pack(const ProtobufCMessage *message,
			  struct esdm_rpcs_connection *rpc_conn)
{
	struct esdm_rpc_proto_sc_header sc_header;
	struct esdm_rpcs_write_buf tmp = { 0 };
	size_t message_length;
	int ret;

	if (!protobuf_c_message_check(message)) {
		sc_header.status_code =
			le_bswap32(PROTOBUF_C_RPC_STATUS_CODE_SERVICE_FAILED);
		sc_header.method_index = le_bswap32(rpc_conn->method_index);
		sc_header.message_length = 0;
		sc_header.request_id = le_bswap32(rpc_conn->request_id);
		return esdm_rpcs_write_data(rpc_conn, (uint8_t *)&sc_header,
					    sizeof(sc_header));
	}

	message_length = protobuf_c_message_get_packed_size(message);
	tmp.base.append = esdm_rpcs_append_data;
	tmp.rpc_conn = rpc_conn;

	sc_header.status_code = le_bswap32(PROTOBUF_C_RPC_STATUS_CODE_SUCCESS);
	sc_header.method_index = le_bswap32(rpc_conn->method_index);
	sc_header.message_length = le_bswap32(message_length);
	sc_header.request_id = le_bswap32(rpc_conn->request_id);

	logger(LOGGER_DEBUG, LOGGER_C_RPC,
	       "Server sending: server status %u, message length %u, message index %u, request ID %u\n",
	       sc_header.status_code, sc_header.message_length,
	       sc_header.method_index, sc_header.request_id);

	CKINT(esdm_rpcs_write_data(rpc_conn,
				   (uint8_t *)&sc_header, sizeof(sc_header)));

	if (protobuf_c_message_pack_to_buffer(message, &tmp.base) !=
	    message_length) {
		logger(LOGGER_VERBOSE, LOGGER_C_RPC,
		       "Short write of data to file descriptor \n");
		ret = -EFAULT;
	}

out:
	return ret;
}

bool esdm_rpc_client_is_privileged(void *closure_data)
{
	struct esdm_rpcs_connection *rpc_conn = closure_data;
	struct ucred cred;
	socklen_t len = sizeof(cred);

	if (getsockopt(rpc_conn->child_fd, SOL_SOCKET, SO_PEERCRED, &cred,
		       &len) < 0)
		return false;

	if (cred.uid == 0) {
		logger(LOGGER_DEBUG, LOGGER_C_ANY,
		       "Remote client is privileged\n");
		return true;
	}

	logger(LOGGER_DEBUG, LOGGER_C_ANY,
	       "Remote client is not privileged\n");
	return false;
}

static void esdm_rpcs_response_closure(const ProtobufCMessage *message,
				       void *closure_data)
{
	struct esdm_rpcs_connection *rpc_conn = closure_data;
	int ret;

	CKINT_LOG(esdm_rpcs_pack(message, rpc_conn),
		  "Failed to serialize response: %d\n", ret);

out:
	return;
}

static int esdm_rpcs_unpack(struct esdm_rpcs_connection *rpc_conn,
			    struct esdm_rpc_proto_cs *received_data)
{
	const ProtobufCMessageDescriptor *desc;
	struct esdm_rpcs *proto = rpc_conn->proto;
	ProtobufCService *service = proto->service;
	ProtobufCMessage *message = NULL;
	struct esdm_rpc_proto_cs_header *header = &received_data->header;
	uint32_t method_index = header->method_index;
	int ret;

	CKINT(esdm_rpc_proto_get_descriptor(service, received_data, &desc));
	message = protobuf_c_message_unpack(desc, rpc_conn->rpc_allocator,
					    header->message_length,
					    received_data->data);

	CKNULL(message, -ENOMEM);

	rpc_conn->method_index = method_index;
	rpc_conn->request_id = header->request_id;

	/* Invoke the RPC call */
	service->invoke(service, method_index, message,
			esdm_rpcs_response_closure, rpc_conn);

out:
	if (message)
		protobuf_c_message_free_unpacked(message,
						 rpc_conn->rpc_allocator);

	return ret;
}

static int esdm_rpcs_read(struct esdm_rpcs_connection *rpc_conn)
{
	ProtobufCAllocator esdm_rpc_allocator = {
		.alloc = &esdm_rpc_alloc,
		.free = &esdm_rpc_free,
		.allocator_data = NULL,
	};
	struct timeval timeout = { .tv_sec = 2, .tv_usec = 0 };
	BUFFER_INIT(tls);
	struct esdm_rpc_proto_cs *received_data;
	uint8_t buf[ESDM_RPC_MAX_MSG_SIZE + sizeof(*received_data)]
						__aligned(sizeof(uint64_t));
	uint8_t unpacked[ESDM_RPC_MAX_MSG_SIZE + 128]
						__aligned(sizeof(uint64_t));
	fd_set fds;
	size_t total_received = 0;
	ssize_t received;
	uint32_t data_to_fetch = 0;
	int ret;
	uint8_t *buf_p = buf;

	if (rpc_conn->child_fd < 0)
		return -EINVAL;

	thread_set_name(rpc_handler, (uint32_t)rpc_conn->child_fd);

	tls.buf = unpacked;
	tls.len = sizeof(unpacked);
	esdm_rpc_allocator.allocator_data = &tls;
	rpc_conn->rpc_allocator = &esdm_rpc_allocator;

	/* The cast is appropriate as the buffer is aligned to 64 bits. */
	received_data = (struct esdm_rpc_proto_cs *)buf;

	/* Read the data into the thread-local storage */
	do {
		FD_ZERO(&fds);
		FD_SET(rpc_conn->child_fd, &fds);

		/*
		 * The reason for using select here is to only wait for a
		 * given amount of time for activity on the FD. After the
		 * timeout, the file descriptor is closed. If an attacker
		 * starts connections, he could leave them open and thus
		 * starve other callers. By timing out on a read the server
		 * tries to avert such attack scenarios. This is the price
		 * to pay for not using malloc and a thread-local storage
		 * buffer.
		 */
		ret = select((rpc_conn->child_fd + 1), &fds, NULL, NULL,
			     &timeout);
		if (ret == 0) {
			ret = -EAGAIN;
			goto out;
		} else if (ret == -1) {
			ret = -errno;
			goto out;
		}

		received = read(rpc_conn->child_fd, buf_p,
				sizeof(buf) - total_received);
		if (received < 0) {
			ret = -errno;
			goto out;
		}

		/* Received EOF */
		if (received == 0) {
			ret = -EAGAIN;
			goto out;
		}

		total_received += (size_t)received;
		buf_p += (size_t)received;

		logger(LOGGER_DEBUG, LOGGER_C_ANY,
		       "Reading %zd bytes, already consumed %zu bytes\n",
		       received, total_received);

		if (total_received < sizeof(*received_data))
			continue;

		if (!data_to_fetch) {
			struct esdm_rpc_proto_cs_header *header =
				&received_data->header;

			/* Convert incoming data to LE */
			header->message_length =
				le_bswap32(header->message_length);
			header->method_index = le_bswap32(header->method_index);
			header->request_id = le_bswap32(header->request_id);

			logger(LOGGER_DEBUG, LOGGER_C_RPC,
			       "Server received: message length %u, message index %u, request ID %u\n",
			       header->message_length,
			       header->method_index, header->request_id);

			/*
			 * Truncate the buffer length if client specified
			 * too much buffer data.
			 */
			if (header->message_length > ESDM_RPC_MAX_MSG_SIZE)
				header->message_length = ESDM_RPC_MAX_MSG_SIZE;

			/* How much data are we expecting to fetch? */
			data_to_fetch = header->message_length;

			/* If we are not expecting anything, simply stop now */
			if (!data_to_fetch)
				break;

			/*
			 * To allow comparison with total_received, let us
			 * add the header length to the data to fetch value.
			 */
			data_to_fetch += sizeof(*received_data);
		}

		/* Now, we received enough and can stop the reading */
		if (total_received >= data_to_fetch)
			break;

	} while (total_received < sizeof(buf));

	/*
	 * We now have a filled buffer that has a header and received
	 * as much data as the header defined. We also start the
	 * processing of data and the subsequent submission of the answer here.
	 */
	CKINT(esdm_rpcs_unpack(rpc_conn, received_data));

out:
	/* Clear the memory after processing one request. */
	memset_secure(buf, 0, total_received);
	memset_secure(tls.buf, 0, tls.consumed);
	return ret;
}

static int esdm_rpcs_handler(void *args)
{
	struct esdm_rpcs_connection *rpc_conn = args;
	int ret;

	/*
	 * Loop reusing the existing connection. When an error is received,
	 * the communication is considered to be severed and the child FD can
	 * be released.
	 */
	do {
		ret = esdm_rpcs_read(rpc_conn);
	} while (!ret);

	close(rpc_conn->child_fd);
	free(rpc_conn);
	return 0;
}

static int esdm_rpcs_workerloop(struct esdm_rpcs *proto)
{
	struct esdm_rpcs_connection *rpc_conn = NULL;
	struct sockaddr addr;
	socklen_t addr_len = sizeof (addr);
	int ret;

	if (proto->server_listening_fd < 0)
		return -EINVAL;
	CKNULL(proto->service, -EINVAL);

	for(;;) {
		/*
		 * Allocate the memory for the thread invocation. This is done
		 * before the accept() call as now we should have time but
		 * after the accept() call, we want to be fast.
		 *
		 * Note, valgrind will report this buffer as leaked. We accept
		 * this leak of one block.
		 */
		rpc_conn = calloc(1, sizeof(struct esdm_rpcs_connection));
		if (!rpc_conn) {
			/* If we are out of memory, terminate our server */
			if (errno == ENOMEM)
				return -errno;

			/* Other errors are handled as transient errors */
			continue;
		}

		rpc_conn->proto = proto;

		/* Wait for incoming connection */
		rpc_conn->child_fd = accept(proto->server_listening_fd, &addr,
					    &addr_len);
		if (rpc_conn->child_fd < 0) {
			free(rpc_conn);
			logger(LOGGER_WARN, LOGGER_C_ANY,
			       "Accepting incoming connections failed: %s\n",
			       strerror(errno));
			continue;
		}

		logger(LOGGER_DEBUG, LOGGER_C_RPC,
		       "Processing new incoming connection\n");

		/* Handle new incoming connection */
		if (thread_start(esdm_rpcs_handler, rpc_conn, 0, NULL)) {
			logger(LOGGER_ERR, LOGGER_C_RPC,
			       "Starting new thread for incoming conneection failed\n");
			free(rpc_conn);
		}
		rpc_conn = NULL;
	}

out:
	if (rpc_conn)
		free(rpc_conn);
	return ret;
}

static int esdm_rpcs_start(const char *unix_socket, uint16_t tcp_port,
			   ProtobufCService *service,
			   struct esdm_rpcs *proto)
{
	struct sockaddr_un addr_un;
	struct sockaddr_in addr_in;
	struct sockaddr *address;
	int fd = -1;
	int protocol_family;
	socklen_t address_len;

	if (unix_socket) {
		protocol_family = PF_UNIX;
		memset(&addr_un, 0, sizeof(addr_un));
		addr_un.sun_family = AF_UNIX;
		strncpy(addr_un.sun_path, unix_socket,
			sizeof(addr_un.sun_path));
		address_len = sizeof(addr_un);
		address = (struct sockaddr *)(&addr_un);

		esdm_rpcs_stale_socket(unix_socket, address, address_len);
	} else if (tcp_port) {
		protocol_family = PF_INET;
		memset (&addr_in, 0, sizeof(addr_in));
		addr_in.sin_family = AF_INET;
		addr_in.sin_port = htons(tcp_port);
		address_len = sizeof(addr_in);
		address = (struct sockaddr *)(&addr_in);
	} else {
		return -EINVAL;
	}

	fd = socket(protocol_family, SOCK_STREAM, 0);
	if (fd < 0)
		return -errno;

	if (bind(fd, address, address_len) < 0) {
		close(fd);
		return -errno;
	}

	if (listen(fd, 255) < 0) {
		close(fd);
		return -errno;
	}

	proto->server_listening_fd = fd;
	proto->service = service;

	return 0;
}

static void eesdm_rpcs_stop(struct esdm_rpcs *proto)
{
	if (proto->server_listening_fd >= 0) {
		close(proto->server_listening_fd);
		proto->server_listening_fd = -1;
	}
}

/* Initialize one thread handling an unprivileged interface instance */
static int esdm_rpcs_unpriv_init(void *args)
{
	struct esdm_rpcs unpriv_proto;
	ProtobufCService *unpriv_service =
				(ProtobufCService *)&unpriv_access_service;
	int ret;

	(void)args;

	thread_set_name(rpc_unpriv_server, 0);
	memset(&unpriv_proto, 0, sizeof(unpriv_proto));

	unpriv_proto.server_listening_fd = -1;

	/* Create server handler for privileged interface in main thread */
	CKINT(esdm_rpcs_start(ESDM_RPC_UNPRIV_SOCKET, 0, unpriv_service,
			      &unpriv_proto));

	/* Make unprivileged socket available for all users */
	if (chmod(ESDM_RPC_UNPRIV_SOCKET,
		  S_IRUSR | S_IWUSR |
		  S_IRGRP | S_IWGRP |
		  S_IROTH | S_IWOTH) == -1) {
		ret = -errno;

		logger(LOGGER_ERR, LOGGER_C_ANY,
		       "Failed to set permissions for Unix domain socket %s: %s\n",
		       ESDM_RPC_UNPRIV_SOCKET, strerror(errno));
		goto out;
	}

	/* Wait for the mother to drop the privileges. */
	thread_wait_event(&esdm_rpc_thread_init_wait, getuid());
	logger(LOGGER_DEBUG, LOGGER_C_RPC,
	       "Unprivileged server thread for %s available\n",
	       ESDM_RPC_UNPRIV_SOCKET);

	/* Server handing unprivileged interface in current thread */
	CKINT(esdm_rpcs_workerloop(&unpriv_proto))
	;
	return 0;

out:
	eesdm_rpcs_stop(&unpriv_proto);

	return ret;
}

static int esdm_rpcs_unpriv_init_threads(void)
{
	if (thread_start(esdm_rpcs_unpriv_init, NULL,
			 ESDM_THREAD_RPC_UNPRIV_GROUP, NULL)) {
		logger(LOGGER_ERR, LOGGER_C_RPC,
		       "Starting server thread failed\n");
	}

	return 0;
}

static int esdm_rpcs_interfaces_init(const char *username)
{
	struct esdm_rpcs priv_proto;
	ProtobufCService *priv_service =
				(ProtobufCService *)&priv_access_service;
	int ret;

	priv_proto.server_listening_fd = -1;

	thread_set_name(rpc_priv_server, 0);
	memset(&priv_proto, 0, sizeof(priv_proto));

	/* Create server handler for privileged interface in main thread */
	CKINT(esdm_rpcs_start(ESDM_RPC_PRIV_SOCKET, 0, priv_service,
			      &priv_proto));

	/* Make privileged socket available for root only */
	if (chmod(ESDM_RPC_PRIV_SOCKET, S_IRUSR | S_IWUSR) == -1) {
		int errsv = errno;

		logger(LOGGER_ERR, LOGGER_C_ANY,
		       "Failed to set permissions for Unix domain socket %s: %s\n",
		       ESDM_RPC_PRIV_SOCKET, strerror(errsv));
		ret = -errsv;
		goto out;
	}

	/* Spawn all threads handling the unprivileged interface */
	CKINT(esdm_rpcs_unpriv_init_threads());

	/* Permanently drop all privileges */
	CKINT(drop_privileges_permanent(username ? username : "nobody"));

	/* Notify all unpriv handler threads that they can become active */
	thread_wake_all(&esdm_rpc_thread_init_wait);
	logger(LOGGER_DEBUG, LOGGER_C_RPC,
	       "Privileged server thread for %s available\n",
	       ESDM_RPC_PRIV_SOCKET);

	/* Server handing privileged interface in current thread */
	CKINT(esdm_rpcs_workerloop(&priv_proto));

	return 0;

out:
	eesdm_rpcs_stop(&priv_proto);
	return ret;
}

static void esdm_rpcs_cleanup(void)
{
	int esdm_shmid;
	key_t key = esdm_ftok(ESDM_SHM_NAME, ESDM_SHM_STATUS);

	/* Clean up all unprivileged Unix domain socket */
	if (unlink(ESDM_RPC_UNPRIV_SOCKET) < 0) {
		logger(LOGGER_ERR, LOGGER_C_SERVER,
			"ESDM Unix domain socket %s cannot be deleted: %s\n",
			ESDM_RPC_UNPRIV_SOCKET, strerror(errno));
	} else {
		logger(LOGGER_DEBUG, LOGGER_C_SERVER,
		       "ESDM Unix domain socket %s deleted\n",
		       ESDM_RPC_UNPRIV_SOCKET);
	}

	/* Clean up the privileged Unix domain socket */
	if (unlink(ESDM_RPC_PRIV_SOCKET) < 0) {
		logger(LOGGER_ERR, LOGGER_C_SERVER,
		       "ESDM Unix domain socket %s cannot be deleted: %s\n",
		       ESDM_RPC_PRIV_SOCKET, strerror(errno));
	} else {
		logger(LOGGER_DEBUG, LOGGER_C_SERVER,
		       "ESDM Unix domain socket %s deleted\n",
		       ESDM_RPC_PRIV_SOCKET);
	}

	/* Clean up the status shared memory segment */
	esdm_shmid = shmget(key, sizeof(struct esdm_shm_status),
			    S_IRUSR | S_IRGRP | S_IROTH);
	if (esdm_shmid < 0) {
		logger(LOGGER_ERR, LOGGER_C_SERVER,
		       "ESDM shared memory segment attachment for deletion failed: %s\n",
		       strerror(errno));
	} else {
		if (shmctl(esdm_shmid, IPC_RMID, NULL) < 0) {
			logger(LOGGER_ERR, LOGGER_C_SERVER,
			       "ESDM shared memory segment cannot be deleted: %s\n",
			       strerror(errno));
		} else {
			logger(LOGGER_DEBUG, LOGGER_C_SERVER,
			       "ESDM shared memory segment deleted\n");
		}
	}

	/* Clean up the status semaphore */
	if (sem_unlink(ESDM_SEM_NAME)) {
		logger(LOGGER_VERBOSE, LOGGER_C_SERVER,
		       "Cannot unlink semaphore: %s\n", strerror(errno));
	} else {
		logger(LOGGER_DEBUG, LOGGER_C_SERVER, "ESDM semaphore deleted\n");
	}
}

static void esdm_rpcs_cleanup_signals(void (*sighandler)(int))
{
	signal(SIGHUP, sighandler);
	signal(SIGINT, sighandler);
	signal(SIGQUIT, sighandler);
	signal(SIGTERM, sighandler);
}

static void esdm_rpcs_cleanup_term(int sig)
{
	(void)sig;

	esdm_rpcs_cleanup_signals(SIG_DFL);

	if (server_pid > 0)
		kill(server_pid, sig);
}

int esdm_rpc_server_init(const char *username)
{
	pid_t pid;
	int ret = 0;

	/* One thread group */
	CKINT(thread_init(1));

	pid = fork();
	if (pid < 0) {
		logger(LOGGER_ERR, LOGGER_C_SERVER,
		       "Cannot fork interface process\n");
		exit(1);
	} else if (pid == 0) {
		/* Fork the server process */
		esdm_rpcs_interfaces_init(username);
	} else {
		/*
		 * This is the cleanup process. It simply waits for the server
		 * to exit to clean up its resources. This is needed because
		 * the server creates resources as root, but then permanently
		 * drops its privileges. This means it cannot clean up after
		 * itself. The cleanup process has no interfaces other than
		 * waiting for the termination of the server process but has
		 * full privileges to be able to clean up the server resources.
		 */

		/*
		 * In case the cleanup process received a signal, relay it to
		 * the server, but do not process the signal itself.
		 */
		server_pid = pid;
		esdm_rpcs_cleanup_signals(esdm_rpcs_cleanup_term);

		/* Now wait for the server to finish. */
		waitpid(pid, NULL, 0);
		server_pid = -1;

		esdm_rpcs_cleanup_signals(SIG_DFL);

		/* Clean up all resources */
		esdm_rpcs_cleanup();
	}

out:
	return ret;
}

void esdm_rpc_server_fini(void)
{
	thread_stop_spawning();

	atomic_set(&server_exit, 1);
	thread_wake_all(&esdm_rpc_thread_init_wait);
}
