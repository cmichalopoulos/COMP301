#ifndef KERNEL_SOCKET_H
#define KERNEL_SOCKET_H

#include "tinyos.h"
#include "util.h"
#include "kernel_pipe.h"
#include "kernel_streams.h"

typedef struct listener_socket listener;
typedef struct unbound_socket unbound;
typedef struct peer_socket peer;
typedef struct socket_control_block socket_cb;

typedef enum {
	SOCKET_LISTENER,
	SOCKET_UNBOUND,
	SOCKET_PEER
} socket_type;

typedef struct listener {
	rlnode queue;
	CondVar req_available;
} listener_socket;

typedef struct unbound {
	rlnode unbound_socket;
} unbound_socket;

typedef struct peer {
	socket_cb* peer;
	pipe_cb* write_pipe;
	pipe_cb* read_pipe;
} peer_socket;

typedef struct socket_control_block {
	uint refcount;
	FCB* fcb;
	socket_type type;
	port_t port;
	union {
		listener_socket listener_s;
		unbound_socket unbound_s;
		peer_socket peer_s;
	};
} socket_cb;

typedef struct conn_req {
	int admitted;
	socket_cb* peer;
	CondVar connect_cv;
	rlnode queue_node;
} connection_request;


int socket_write(void* socketcb_t, const char* buffer, unsigned int size);

int socket_read(void* socketcb_t, char* buffer, unsigned int size);

int socket_close(void* socketcb_t);


#endif 