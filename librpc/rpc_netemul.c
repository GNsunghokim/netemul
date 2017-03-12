#include <string.h>
#include <strings.h>
#include <malloc.h>
#include <fcntl.h>

#include "node.h"
#include "rpc_netemul.h"

struct {
	uint32_t 	id;
	bool(*callback)(RPC_NetEmulator* rpc, void* context);
	void*		context;
} RPC_NetEmulator_Callback;

#define MAKE_WRITE(TYPE)					\
static int write_##TYPE(RPC_NetEmulator* rpc, TYPE##_t v) {			\
	int len = sizeof(TYPE##_t);				\
	if(rpc->wbuf_index + len > RPC_NETEMUL_BUFFER_SIZE)		\
		return 0;					\
								\
	memcpy(rpc->wbuf + rpc->wbuf_index, &v, len);		\
	rpc->wbuf_index += len;					\
								\
	return len;						\
}

#define MAKE_READ(TYPE)						\
static int read_##TYPE(RPC_NetEmulator* rpc, TYPE##_t* v) {			\
	int len = sizeof(TYPE##_t);				\
	if(rpc->rbuf_read + len > rpc->rbuf_index) {		\
		int len2 = rpc->read(rpc, 			\
			rpc->rbuf + rpc->rbuf_index, 		\
			RPC_NETEMUL_BUFFER_SIZE - rpc->rbuf_index);	\
		if(len2 < 0) {					\
			return len2;				\
		}						\
								\
		rpc->rbuf_index += len2;			\
	}							\
								\
	if(rpc->rbuf_read + len > rpc->rbuf_index)		\
		return 0;					\
								\
	memcpy(v, rpc->rbuf + rpc->rbuf_read, len);		\
	rpc->rbuf_read += len;					\
								\
	return len;						\
}

MAKE_READ(uint8)
MAKE_WRITE(uint8)
MAKE_READ(uint16)
MAKE_WRITE(uint16)
MAKE_READ(uint32)
MAKE_WRITE(uint32)
MAKE_READ(uint64)
MAKE_WRITE(uint64)

MAKE_READ(int32)
MAKE_WRITE(int32)

#define read_bool(RPC_NetEmulator, DATA)	read_uint8((RPC_NetEmulator), (uint8_t*)(DATA))
#define write_bool(RPC_NetEmulator, DATA)	write_uint8((RPC_NetEmulator), (uint8_t)(DATA))

static int write_string(RPC_NetEmulator* rpc, const char* v) {
	uint16_t len0;
	if(!v)
		len0 = 0;
	else
		len0 = strlen(v);

	uint16_t len = sizeof(uint16_t) + len0;
	if(rpc->wbuf_index + len > RPC_NETEMUL_BUFFER_SIZE)
		return 0;
	
	memcpy(rpc->wbuf + rpc->wbuf_index, &len0, sizeof(uint16_t));
	memcpy(rpc->wbuf + rpc->wbuf_index + sizeof(uint16_t), v, len0);
	rpc->wbuf_index += len;
	
	return len;
}

static int read_string(RPC_NetEmulator* rpc, char** v, uint16_t* len) {
	*len = 0;
	
	uint16_t len0;
	memcpy(&len0, rpc->rbuf + rpc->rbuf_read, sizeof(uint16_t));
	uint16_t len1 = sizeof(uint16_t) + len0;
	
	if(rpc->rbuf_read + len1 > rpc->rbuf_index) {
		int len2 = rpc->read(rpc, rpc->rbuf + rpc->rbuf_index, 
			RPC_NETEMUL_BUFFER_SIZE - rpc->rbuf_index);
		if(len2 < 0) {
			return len2;
		}
		
		rpc->rbuf_index += len2;
	}
	
	if(rpc->rbuf_read + len1 > rpc->rbuf_index)
		return 0;
	
	*len = len0;
	if(v != NULL)
		*v = (char*)(rpc->rbuf + rpc->rbuf_read + sizeof(uint16_t));
	rpc->rbuf_read += len1;
	
	return len1;
}

static int write_bytes(RPC_NetEmulator* rpc, void* v, int32_t size) {
	int32_t len = sizeof(int32_t) + (size < 0 ? 0 : size);
	if(rpc->wbuf_index + len > RPC_NETEMUL_BUFFER_SIZE)
		return 0;
	
	memcpy(rpc->wbuf + rpc->wbuf_index, &size, sizeof(int32_t));
	if(size > 0)
		memcpy(rpc->wbuf + rpc->wbuf_index + sizeof(int32_t), v, size);
	rpc->wbuf_index += len;
	
	return len;
}

static int read_bytes(RPC_NetEmulator* rpc, void** v, int32_t* len) {
	*len = 0;
	
	int32_t len0;
	memcpy(&len0, rpc->rbuf + rpc->rbuf_read, sizeof(int32_t));
	int32_t len1 = sizeof(int32_t) + (len0 < 0 ? 0 : len0);
	
	if(rpc->rbuf_read + len1 > rpc->rbuf_index) {
		int len2 = rpc->read(rpc, rpc->rbuf + rpc->rbuf_index, 
			RPC_NETEMUL_BUFFER_SIZE - rpc->rbuf_index);
		if(len2 < 0) {
			return len2;
		}
		
		rpc->rbuf_index += len2;
	}
	
	if(rpc->rbuf_read + len1 > rpc->rbuf_index)
		return 0;
	
	*len = len0;
	if(v != NULL) {
		if(len0 > 0) {
			*v = (void*)(rpc->rbuf + rpc->rbuf_read + sizeof(int32_t));
		} else {
			*v = NULL;
		}
	}
	rpc->rbuf_read += len1;
	
	return len1;
}

#define INIT()								\
	__attribute__((__unused__)) int _len = 0;			\
	__attribute__((__unused__)) int _size = 0;			\
	__attribute__((__unused__)) int _rbuf_read = rpc->rbuf_read;	\
	__attribute__((__unused__)) int _wbuf_index = rpc->wbuf_index;

#define ROLLBACK()			\
	rpc->rbuf_read = _rbuf_read;	\
	rpc->wbuf_index = _wbuf_index;
	
#define READ(VALUE)			\
if((_len = (VALUE)) <= 0) {		\
	ROLLBACK();			\
	return _len; 			\
} else {				\
	_size += _len;			\
}

#define READ2(VALUE, FAILED)		\
if((_len = (VALUE)) <= 0) {		\
	ROLLBACK();			\
	FAILED();			\
	return _len; 			\
} else {				\
	_size += _len;			\
}

#define WRITE(VALUE)			\
if((_len = (VALUE)) <= 0) {		\
	ROLLBACK();			\
	return _len; 			\
} else {				\
	_size += _len;			\
}

#define RETURN()	return _size;

#define INIT2()					\
	int _wbuf_index = rpc->wbuf_index;

#define WRITE2(VALUE)			\
if((VALUE) <= 0) {			\
	rpc->wbuf_index = _wbuf_index;	\
	return;				\
}

#define RETURN2()			\
if(rpc->wbuf_index > 0 && rpc->write && wbuf_flush(rpc) < 0 && rpc->close) {	\
	rpc->close(rpc);							\
	return;									\
}

static int rbuf_flush(RPC_NetEmulator* rpc) {
	int len = rpc->rbuf_read;
	memmove(rpc->rbuf, rpc->rbuf + len, rpc->rbuf_index - len);
	
	rpc->rbuf_index -= len;
	rpc->rbuf_read = 0;
	
	return len;
}

static int wbuf_flush(RPC_NetEmulator* rpc) {
	int len = rpc->write(rpc, rpc->wbuf, rpc->wbuf_index);
	if(len < 0) {
		return len;
	}
	memmove(rpc->wbuf, rpc->wbuf + len, rpc->wbuf_index - len);
	
	rpc->wbuf_index -= len;
	
	return len;
}

// API
// hello client API
int rpc_hello(RPC_NetEmulator* rpc, bool(*callback)(void* context), void* context) {
	INIT();
	
	WRITE(write_uint16(rpc, RPC_NETEMUL_TYPE_HELLO_REQ));
	WRITE(write_string(rpc, RPC_NETEMUL_MAGIC));
	WRITE(write_uint32(rpc, RPC_NETEMUL_VERSION));
	
	rpc->hello_callback = callback;
	rpc->hello_context = context;
	
	RETURN();
}

static int hello_res_handler(RPC_NetEmulator* rpc) {
	if(rpc->hello_callback && !rpc->hello_callback(rpc->hello_context)) {
		rpc->hello_callback = NULL;
		rpc->hello_context = NULL;
	}
	
	return 1;
}

// hello server API
static int hello_req_handler(RPC_NetEmulator* rpc) {
	INIT();
	
	char* magic;
	uint16_t len;
	READ(read_string(rpc, &magic, &len));
	if(len != RPC_NETEMUL_MAGIC_SIZE) {
		return -1;
	}
	
	if(memcmp(magic, RPC_NETEMUL_MAGIC, RPC_NETEMUL_MAGIC_SIZE) != 0) {
		return -1;
	}
	
	uint32_t ver;
	READ(read_uint32(rpc, &ver));
	if(ver == RPC_NETEMUL_VERSION) {
		rpc->ver = ver;
	} else {
		return -1;
	}
	
	WRITE(write_uint16(rpc, RPC_NETEMUL_TYPE_HELLO_RES));
	
	RETURN();
}

// on client API
int rpc_on(RPC_NetEmulator* rpc, char* node, bool(*callback)(bool result, void* context), void* context) {
	INIT();
	
	WRITE(write_uint16(rpc, RPC_NETEMUL_TYPE_ON_REQ));
	WRITE(write_string(rpc, node));
	
	rpc->on_callback = callback;
	rpc->on_context = context;
	
	RETURN();
}

static int on_res_handler(RPC_NetEmulator* rpc) {
	INIT();
	
	bool result;
	READ(read_bool(rpc, &result));
	
	if(rpc->on_callback && !rpc->on_callback(result, rpc->on_context)) {
		rpc->on_callback = NULL;
		rpc->on_context = NULL;
	}
	
	RETURN();
}

// on server API
void rpc_on_handler(RPC_NetEmulator* rpc, void(*handler)(RPC_NetEmulator* rpc, char* node, void* context, void(*callback)(RPC_NetEmulator* rpc, bool result)), void* context) {
	rpc->on_handler = handler;
	rpc->on_handler_context = context;
}

	
static void on_handler_callback(RPC_NetEmulator* rpc, bool result) {
	INIT2();
	
	WRITE2(write_uint16(rpc, RPC_NETEMUL_TYPE_ON_RES));
	WRITE2(write_bool(rpc, result));
	
	RETURN2();
}

static int on_req_handler(RPC_NetEmulator* rpc) {
	INIT();
	
	char* node;
	uint16_t len;
	READ(read_string(rpc, &node, &len));
	
	if(rpc->on_handler) {
		rpc->on_handler(rpc, node, rpc->on_handler_context, on_handler_callback);
	} else {
		on_handler_callback(rpc, false);
	}
	
	RETURN();
}

// list client API
int rpc_list(RPC_NetEmulator* rpc, uint8_t type, bool(*callback)(char* result, void* context), void* context) {
	INIT();
	
	WRITE(write_uint16(rpc, RPC_NETEMUL_TYPE_LIST_REQ));
	WRITE(write_uint8(rpc, type));
	
	rpc->list_callback = callback;
	rpc->list_context = context;
	
	RETURN();
}

static int list_res_handler(RPC_NetEmulator* rpc) {
	INIT();
	
	char* result;
	uint16_t len;
	//TODO string read
	READ(read_string(rpc, &result, &len));
	
	if(rpc->list_callback && !rpc->list_callback(result, rpc->list_context)) {
		rpc->list_callback = NULL;
		rpc->list_context = NULL;
	}
	
	RETURN();
}

// list server API
void rpc_list_handler(RPC_NetEmulator* rpc, void(*handler)(RPC_NetEmulator* rpc, uint8_t type, void* context, void(*callback)(RPC_NetEmulator* rpc, char* result)), void* context) {
	rpc->list_handler = handler;
	rpc->list_handler_context = context;
}

	
static void list_handler_callback(RPC_NetEmulator* rpc, char* result) {
	INIT2();
	
	WRITE2(write_uint16(rpc, RPC_NETEMUL_TYPE_LIST_RES));
	WRITE2(write_string(rpc, result));
	
	RETURN2();
}

static int list_req_handler(RPC_NetEmulator* rpc) {
	INIT();
	
	uint8_t type;
	READ(read_uint8(rpc, &type));
	
	if(rpc->list_handler) {
		rpc->list_handler(rpc, type, rpc->list_handler_context, list_handler_callback);
	} else {
		list_handler_callback(rpc, false);
	}
	
	RETURN();
}

// create client API
int rpc_create(RPC_NetEmulator* rpc, CreateSpec* spec, bool(*callback)(bool result, void* context), void* context) {
	INIT();
	
	WRITE(write_uint16(rpc, RPC_NETEMUL_TYPE_CREATE_REQ));
	WRITE(write_uint8(rpc, spec->type));
	switch(spec->type) {
		case NODE_TYPE_BRIDGE:
			WRITE(write_string(rpc, spec->info.bridge));
			break;
		case NODE_TYPE_HOST:
			WRITE(write_uint32(rpc, spec->info.port_count));
			break;
		case NODE_TYPE_LINK:
			WRITE(write_string(rpc, spec->info.link.node0));
			WRITE(write_string(rpc, spec->info.link.node1));
			break;
		case NODE_TYPE_ETHER_SWITCH:
			WRITE(write_uint32(rpc, spec->info.port_count));
			break;
		case NODE_TYPE_HUB_SWITCH:
			WRITE(write_uint32(rpc, spec->info.port_count));
			break;
	}
	
	rpc->create_callback = callback;
	rpc->create_context = context;
	
	RETURN();
}

static int create_res_handler(RPC_NetEmulator* rpc) {
	INIT();
	
	bool result;
	READ(read_bool(rpc, &result));
	
	if(rpc->create_callback && !rpc->create_callback(result, rpc->create_context)) {
		rpc->create_callback = NULL;
		rpc->create_context = NULL;
	}
	
	RETURN();
}

// create server API
void rpc_create_handler(RPC_NetEmulator* rpc, void(*handler)(RPC_NetEmulator* rpc, CreateSpec* spec, void* context, void(*callback)(RPC_NetEmulator* rpc, bool result)), void* context) {
	rpc->create_handler = handler;
	rpc->create_handler_context = context;
}

	
static void create_handler_callback(RPC_NetEmulator* rpc, bool result) {
	INIT2();
	
	WRITE2(write_uint16(rpc, RPC_NETEMUL_TYPE_CREATE_RES));
	WRITE2(write_bool(rpc, result));
	
	RETURN2();
}

static int create_req_handler(RPC_NetEmulator* rpc) {
	INIT();
	
	CreateSpec spec;
	READ(read_uint8(rpc, &spec.type));
	char* bridge;
	char* node0;
	char* node1;
	uint16_t len;
	switch(spec.type) {
		case NODE_TYPE_BRIDGE:
			READ(read_string(rpc, &bridge, &len));
			strncpy(spec.info.bridge, bridge, IFNAMSIZ);
			break;

		case NODE_TYPE_HOST:
			READ(read_uint32(rpc, &spec.info.port_count));
			break;

		case NODE_TYPE_LINK:
			READ(read_string(rpc, &node0, &len));
			strncpy(spec.info.link.node0, node0, IFNAMSIZ);
			READ(read_string(rpc, &node1, &len));
			strncpy(spec.info.link.node1, node1, IFNAMSIZ);
			break;

		case NODE_TYPE_ETHER_SWITCH:
			READ(read_uint32(rpc, &spec.info.port_count));
			break;

		case NODE_TYPE_HUB_SWITCH:
			READ(read_uint32(rpc, &spec.info.port_count));
			break;
	}
	
	if(rpc->create_handler) {
		rpc->create_handler(rpc, &spec, rpc->create_handler_context, create_handler_callback);
	} else {
		create_handler_callback(rpc, false);
	}
	
	RETURN();
}

// Handlers
typedef int(*Handler)(RPC_NetEmulator*);

static Handler handlers[] = {
	NULL,
	hello_req_handler,
	hello_res_handler,
	create_req_handler,
	create_res_handler,
	on_req_handler,
	on_res_handler,
	list_req_handler,
	list_res_handler,
};

bool rpc_loop(RPC_NetEmulator* rpc) {
	if(rpc->wbuf_index > 0 && rpc->write) {
		if(wbuf_flush(rpc) < 0 && rpc->close) {
			rpc->close(rpc);
			return false;
		}
	}
	
	bool is_first = true;
	while(true) {
		INIT();
		
		uint16_t type = (uint16_t)-1;
		_len = read_uint16(rpc, &type);
		
		if(_len > 0) {
			if(type >= RPC_NETEMUL_TYPE_END || !handlers[type]) {
				if(rpc->close)
					rpc->close(rpc);
				
				return _size > 0;
			}
		} else if(_len < 0) {
			if(rpc->close)
				rpc->close(rpc);
			
			return _size > 0;
		} else {
			if(!is_first)
				return _size > 0;
		}
		
		if(type != (uint16_t)-1) {
			_len = handlers[type](rpc);
			if(_len > 0) {
				_size += _len;
				
				if(rpc->wbuf_index > 0 && rpc->write && wbuf_flush(rpc) < 0 && rpc->close) {
					rpc->close(rpc);
					return false;
				}
				
				if(rbuf_flush(rpc) < 0 && rpc->close) {
					rpc->close(rpc);
					return false;
				}
			} else if(_len == 0) {
				ROLLBACK();
				return _size > 0;
			} else if(rpc->close) {
				rpc->close(rpc);
				return _size > 0;
			}
		} else {
			return _size > 0;
		}
		
		is_first = false;
	}
}

#ifdef LINUX
#define DEBUG 0
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>
#include <errno.h>

// Posix socket RPC_NetEmulator data
typedef struct {
	int	fd;
	struct	sockaddr_in caddr;
} RPC_NetEmulatorData;

static int sock_read(RPC_NetEmulator* rpc, void* buf, int size) {
	RPC_NetEmulatorData* data = (RPC_NetEmulatorData*)rpc->data;
	int len = recv(data->fd, buf, size, MSG_DONTWAIT);
	#if DEBUG
	if(len > 0) {
		printf("Read: ");
		for(int i = 0; i < len; i++) {
			printf("%02x ", ((uint8_t*)buf)[i]);
		}
		printf("\n");
	}
	#endif /* DEBUG */
	
	if(len == -1) {
		if(errno == EAGAIN)
			return 0;
				
		return -1;
	} else if(len == 0) {
		return -1;
	} else {
		return len;
	}
}

static int sock_write(RPC_NetEmulator* rpc, void* buf, int size) {
	RPC_NetEmulatorData* data = (RPC_NetEmulatorData*)rpc->data;
	int len = send(data->fd, buf, size, MSG_DONTWAIT);
	#if DEBUG
	if(len > 0) {
		printf("Write: ");
		for(int i = 0; i < len; i++) {
			printf("%02x ", ((uint8_t*)buf)[i]);
		}
		printf("\n");
	}
	#endif /* DEBUG */
	
	if(len == -1) {
		if(errno == EAGAIN)
			return 0;
				
		return -1;
	} else if(len == 0) {
		return -1;
	} else {
		return len;
	}
}

static void sock_close(RPC_NetEmulator* rpc) {
	RPC_NetEmulatorData* data = (RPC_NetEmulatorData*)rpc->data;
	close(data->fd);
	data->fd = -1;
#if DEBUG
	printf("Connection closed : %s\n", inet_ntoa(data->caddr.sin_addr));
#endif
}

RPC_NetEmulator* rpc_netemul_open(const char* host, int port, int timeout) {
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if(fd < 0)
		return NULL;
	
	void handler(int signo) {
		// Do nothing just interrupt
	}
	
	struct sigaction sigact, old_sigact;
	sigact.sa_handler = handler;
	sigemptyset(&sigact.sa_mask);
	sigact.sa_flags = SA_INTERRUPT;
	
	if(sigaction(SIGALRM, &sigact, &old_sigact) < 0) {
		close(fd);
		return NULL;
	}
	
	struct sockaddr_in addr;
	memset(&addr, 0x0, sizeof(struct sockaddr_in));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = inet_addr(host);
	addr.sin_port = htons(port);
	
	alarm(timeout);
	
	if(connect(fd, (struct sockaddr*)&addr, sizeof(struct sockaddr_in)) < 0) {
		alarm(0);
		sigaction(SIGALRM, &old_sigact, NULL);
		close(fd);
		return NULL;
	}
	
	alarm(0);
	
	if(sigaction(SIGALRM, &old_sigact, NULL) < 0) {
		close(fd);
		return NULL;
	}
	
	RPC_NetEmulator* rpc = malloc(sizeof(RPC_NetEmulator) + sizeof(RPC_NetEmulatorData));
	rpc->read = sock_read;
	rpc->write = sock_write;
	rpc->close = sock_close;
	
	RPC_NetEmulatorData* data = (RPC_NetEmulatorData*)rpc->data;
	data->fd = fd;
	
	return rpc;
}

void rpc_netemul_close(RPC_NetEmulator* rpc) {
	if(rpc->close)
		rpc->close(rpc);
}

RPC_NetEmulator* rpc_netemul_listen(int port) {
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if(fd < 0) {
		return NULL;
	}
	
	int reuse = 1;
	if(setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0)
		perror("Failed to set socket option - SO_REUSEADDR\n");
	
	struct sockaddr_in addr;
	memset(&addr, 0x0, sizeof(struct sockaddr_in));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	addr.sin_port = htons(port);
	
	if(bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
		return NULL;
	}
	
	RPC_NetEmulator* rpc = malloc(sizeof(RPC_NetEmulator) + sizeof(RPC_NetEmulatorData));
	memset(rpc, 0x0, sizeof(RPC_NetEmulator));
	RPC_NetEmulatorData* data = (RPC_NetEmulatorData*)rpc->data;
	data->fd = fd;
	
	return rpc;
}

RPC_NetEmulator* rpc_netemul_accept(RPC_NetEmulator* srpc) {
	RPC_NetEmulatorData* data = (RPC_NetEmulatorData*)srpc->data;
	if(listen(data->fd, 5) < 0) {
		return NULL;
	}
	
	// TODO: would rather change to nonblock socket
	int rc = fcntl(data->fd, F_SETFL, fcntl(data->fd, F_GETFL, 0) | O_NONBLOCK);
	if(rc < 0)
		perror("Failed to modifiy socket to nonblock\n");
		    
	socklen_t len = sizeof(struct sockaddr_in);
	struct sockaddr_in addr;
	int fd = accept(data->fd, (struct sockaddr*)&addr, &len);
	if(fd < 0)
		return NULL;
	
	RPC_NetEmulator* rpc = malloc(sizeof(RPC_NetEmulator) + sizeof(RPC_NetEmulatorData));
	memcpy(rpc, srpc, sizeof(RPC_NetEmulator));
	rpc->ver = 0;
	rpc->rbuf_read = 0;
	rpc->rbuf_index = 0;
	rpc->wbuf_index = 0;
	rpc->read = sock_read;
	rpc->write = sock_write;
	rpc->close = sock_close;
	
	data = (RPC_NetEmulatorData*)rpc->data;
	memcpy(&data->caddr, &addr, sizeof(struct sockaddr_in));
	data->fd = fd;
	
	return rpc;
}

bool rpc_netemul_is_closed(RPC_NetEmulator* rpc) {
	RPC_NetEmulatorData* data = (RPC_NetEmulatorData*)rpc->data;
	return data->fd < 0;
	
}
#endif /* LINUX */
