
#include "tinyos.h"
#include "kernel_streams.h"
#include "util.h"
#include "kernel_socket.h"

socket_cb* PORT_MAP[MAX_PORT+1];

file_ops socket_file_ops = {
	.Open = NULL,
	.Read = socket_read,
	.Write = socket_write,
	.Close = socket_close
};

int socket_write(void* socketcb_t, const char* buffer, unsigned int n){
	if(socketcb_t == NULL || buffer == NULL || n<0)
		return -1;

	socket_cb* scb = (socket_cb*) socketcb_t;
	if(scb->peer_s.write_pipe != NULL)
		return pipe_write(&(scb->peer_s.write_pipe), buffer, n);
	return -1;
}

int socket_read(void* socketcb_t, char* buffer, unsigned int n){
	if(socketcb_t == NULL || buffer == NULL || n<0)
		return -1;

	socket_cb* scb = (socket_cb*) socketcb_t;
	if(scb->peer_s.read_pipe != NULL)
		return pipe_read(&(scb->peer_s.read_pipe), buffer, n);
	return -1;
}

int socket_close(void* socketcb_t){
	socket_cb* scb = (socket_cb*) socketcb_t;
	// PEER CLOSE
	if(scb->type == SOCKET_PEER){
		pipe_reader_close(scb->peer_s.read_pipe);
		pipe_writer_close(scb->peer_s.write_pipe);
	}
	// LISTENER CLOSE
	if (scb->type == SOCKET_LISTENER){
		kernel_broadcast(&scb->listener_s.req_available); 	// Wake up socket waiting in listener
		PORT_MAP[scb->port] = NULL; 						// Release the socket from the port map
	}

	scb = NULL;
	return 0;
}


Fid_t sys_Socket(port_t port)
{	
	if(port < NOPORT || port> MAX_PORT)
		return NOFILE;

	Fid_t fid;
	FCB* fcb[1]; 
	if(FCB_reserve(1, &fid, fcb) == 0)
		return NOFILE;

	socket_cb* scb = (socket_cb*)xmalloc(sizeof(socket_cb));
	scb->refcount = 1;
	scb->fcb = fcb[0]; 
	fcb[0]->streamobj = scb;
	fcb[0]->streamfunc = &socket_file_ops;
	scb->type = SOCKET_UNBOUND;
	scb->port = port;

	return fid;
}

int sys_Listen(Fid_t sock)
{
	

	FCB* fcb = get_fcb(sock);
	if(fcb == NULL)						// Check if fcb is illegal 
		return -1;

	socket_cb* scb = fcb->streamobj;

	if(scb == NULL)						// Check if socket is NULL
		return -1;
	if(scb->type == SOCKET_LISTENER)	// Check if type of socket is already LISTENER
		return -1;
	if(scb->port == NOPORT)				// Check if socket is not bound to a port
		return -1;
	if(PORT_MAP[scb->port] != NULL)		// Check if port to bound at, is unavailable
		return -1;
	
	// Install scb to the PORT_MAP
	PORT_MAP[scb->port] = scb;
	
	// Make scb type a LISTENER
	scb->type = SOCKET_LISTENER;

	// Initialize listener_socket fields
	rlnode_init(&scb->listener_s.queue, NULL);
	scb->listener_s.req_available = COND_INIT;

	return 0;
}


Fid_t sys_Accept(Fid_t lsock)
{	
	if(lsock > MAX_FILEID || lsock < NOFILE)
		return NOFILE;

	FCB* fcb = get_fcb(lsock);
	if(fcb == NULL)								// If fcb is invalid return -1
		return NOFILE;

	socket_cb* lscb = fcb->streamobj;	
	if(lscb == NULL)							// Socket is invalid
		return NOFILE;
	if(lscb->type != SOCKET_LISTENER)			// Socket is not type listener 
		return NOFILE;

	lscb->refcount = lscb->refcount + 1;		// Increase refcount

	while(is_rlist_empty(&(lscb->listener_s.queue))){				// If list of requests in the listener queue is empty 
		kernel_wait(&lscb->listener_s.req_available, SCHED_IO);		// kernel wait until we receive a request
		if(PORT_MAP[lscb->port] == NULL)							// While waiting, if the listening socket port closes, 
			return NOFILE;											// return error.
	}

	rlnode* found = rlist_pop_front(&lscb->listener_s.queue);		// pop front in the queue the incoming node
	connection_request* req = found->obj;

	req->peer->type = SOCKET_PEER;				// Make type of socket -> PEER 
	
	socket_cb* socket_cb2 = req->peer;			// Create socket_cb2 - This is for Client 

	// Time to create socket_cb3 - Server stuff
	Fid_t fid3; 
	FCB* fcb3[1];
	//port_t port;

	int reserved3 = FCB_reserve(1, &fid3, fcb3);
	if(reserved3 == 0){
		return NOFILE;
	}
	
	socket_cb* socket_cb3 = (socket_cb*)xmalloc(sizeof(socket_cb));
	socket_cb3->refcount = 1;
	socket_cb3->fcb = fcb3[0]; 
	fcb3[0]->streamobj = socket_cb3;
	fcb3[0]->streamfunc = &socket_file_ops;
	socket_cb3->type = SOCKET_UNBOUND;
	//socket_cb3->port = port;

	// Make socket_cb type -> PEER 
	socket_cb3->port = SOCKET_PEER;

	// As of right now we have one socket for Server (socket_cb3) and one socket for Client (socket_cb2)
	// Lets create the two pipes and connect them with the sockets

	pipe_cb* pipe_cb1 = (pipe_cb*)xmalloc(sizeof(pipe_cb));
	pipe_cb* pipe_cb2 = (pipe_cb*)xmalloc(sizeof(pipe_cb));

	// PIPE_CB1
	pipe_cb1->reader->streamobj = socket_cb2; 
	pipe_cb1->writer->streamobj = socket_cb3;

	pipe_cb1->reader->streamfunc = &socket_file_ops;
	pipe_cb1->writer->streamfunc = &socket_file_ops;

	pipe_cb1->r_position = 0;
	pipe_cb1->w_position = 0;

	pipe_cb1->has_data = COND_INIT;
	pipe_cb1->has_space = COND_INIT;

	// PIPE_CB2
	pipe_cb2->reader->streamobj = socket_cb3;
	pipe_cb2->writer->streamobj = socket_cb2;

	pipe_cb2->reader->streamfunc = &socket_file_ops;
	pipe_cb2->writer->streamfunc = &socket_file_ops;

	pipe_cb2->r_position = 0;
	pipe_cb2->w_position = 0;

	pipe_cb2->has_data = COND_INIT;
	pipe_cb2->has_space = COND_INIT;

	// CONNECTIONS
	socket_cb2->peer_s.read_pipe = pipe_cb1;
	socket_cb2->peer_s.write_pipe = pipe_cb2;

	socket_cb3->peer_s.read_pipe = pipe_cb2;
	socket_cb3->peer_s.write_pipe = pipe_cb1;

	// Change admitted to 1
	req->admitted = 1;

	// Signal connect side
	kernel_signal(&(req->connect_cv));

	// Decrease refcount
	lscb->refcount = lscb->refcount - 1;

	return fid3;
}


int sys_Connect(Fid_t sock, port_t port, timeout_t timeout)
{
	if(PORT_MAP[port] == NULL || PORT_MAP[port]->type != SOCKET_LISTENER)	// Socket is unconnected or non-listening
		return -1;

	FCB* fcb = get_fcb(sock);
	if(fcb == NULL)					
		return -1;

	socket_cb* scb = fcb->streamobj;
	if(scb == NULL)
		return -1;
	if(scb->type != SOCKET_UNBOUND)		// Must be unbounded 
		return -1;

	scb->refcount = scb->refcount + 1;

	// Building the request
	connection_request* req = (connection_request*)xmalloc(sizeof(connection_request));
	req->admitted = 0;
	req->peer = scb;
	req->connect_cv = COND_INIT;
	rlnode_init(&req->queue_node, req);

	req->peer->type = SOCKET_PEER;

	// Adding request to listener's request queue and kernel_signal listener
	socket_cb* lscb = PORT_MAP[port];
	rlist_push_back(&lscb->listener_s.queue, &req->queue_node);
	kernel_broadcast(&lscb->listener_s.req_available);

	//while(req->admitted == 0){
		int timeout_error = kernel_timedwait(&req->connect_cv, SCHED_PIPE, 1000);
		if(!timeout_error)
			return -1;
	//}

	scb->refcount = scb->refcount - 1;
	return 0;

}


int sys_ShutDown(Fid_t sock, shutdown_mode how)
{	
	FCB* fcb = get_fcb(sock);
	if(fcb == NULL)
		return -1;

	socket_cb* scb = fcb->streamobj;
	if(scb == NULL || scb->type != SOCKET_PEER)
		return -1;

	switch(how){
		case SHUTDOWN_READ:
			pipe_reader_close(&(scb->peer_s.read_pipe));
			break;
		case SHUTDOWN_WRITE:
			pipe_writer_close(&(scb->peer_s.write_pipe));
			break;
		case SHUTDOWN_BOTH:
			pipe_reader_close(&(scb->peer_s.read_pipe));
			pipe_writer_close(&(scb->peer_s.write_pipe));
			break;
		default:
			return -1;
	}
	return 0;
}

