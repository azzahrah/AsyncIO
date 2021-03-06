#include "asyncServer.hpp"

#define PORT_UDP	10200
#define PORT_TCP	10201
#define PORT_SCTP	10202

#define HOST		"127.0.0.1"

callBackFunction callback[5];

// time utility function starts ...
static void msec_to_ts(struct __kernel_timespec *ts, unsigned int msec)
{
	ts->tv_sec = msec / 1000;
	ts->tv_nsec = (msec % 1000) * 1000000;
}

static unsigned long long mtime_since(const struct timeval *s,
				      const struct timeval *e)
{
	long long sec, usec;

	sec = e->tv_sec - s->tv_sec;
	usec = (e->tv_usec - s->tv_usec);
	if (sec > 0 && usec < 0) {
		sec--;
		usec += 1000000;
	}

	sec *= 1000;
	usec /= 1000;
	return sec + usec;
}

static unsigned long long mtime_since_now(struct timeval *tv)
{
	struct timeval end;

	gettimeofday(&end, NULL);
	return mtime_since(tv, &end);
}
// timer utility function ends ....

AsyncServer::AsyncServer() {
	
	ring_ = new io_uring();
	if(ring_ == nullptr) {
		std::cout <<"Problem in allocating io_uring ring" << std::endl;
	}
	assert(io_uring_queue_init(32, this->ring_, 0) >= 0);
	std::cout << "AsyncServer instance Created" << std::endl;
}

AsyncServer::~AsyncServer() {
	// std::cout <<"AsyncServer desctructed" << std::endl;
	// io_uring_queue_exit(ring_);
};

void AsyncServer::setServerConfig(uint8_t _types) {
	isSocketServer = true;
	this->server_type = _types;
}

void AsyncServer::Send(void *buf,size_t len,client_info* client_) {
	std::string cKey = client_->to_string();
	int fd = -1;
	if(clientFdMap.find(cKey) != clientFdMap.end()) {
		fd = clientFdMap[cKey];	
	}else {
		std::cerr <<"Client file descriptor not found" << std::endl;
		return;
	}
	auto prep_SEND = [this,_buf = buf,_len = len](StateMgmtIntf* instance) {
		auto dbuf = instance->getBuffer();
		std::memcpy(dbuf,_buf,_len);
		auto sqe = io_uring_get_sqe(this->ring_);
		io_uring_prep_rw(IORING_OP_SEND, sqe,instance->getReadFd(), 
						instance->getBuffer(), _len, 0);
		sqe->user_data = (__u64)(instance); // important
		auto ret = io_uring_submit(this->ring_);
		if (ret <= 0) {
			fprintf(stderr, "%s: sqe submit failed: %d\n", __FUNCTION__, ret);
		}
	};
	auto handle_CQE_AfterSend = [](StateMgmtIntf* instance,io_uring_cqe *cqe) {
		std::cout <<"Data is sent to client. Bytes sent = "<<cqe->res << std::endl;
	};
	std::cout <<"Send prep done!" << std::endl;
	new StateMgmt<decltype(prep_SEND),decltype(handle_CQE_AfterSend)>
	(
        this->ring_,
        fd, // this instance of StateMgmt is created for sending
		-1, 
        this,
        prep_SEND,
        handle_CQE_AfterSend,
		TCP_SEND,
		true // isSend = true
	);
}
timerInfo* AsyncServer::createNewTimer(std::string tname,int duration) {
	auto t = new timerInfo(tname,duration);
	return t;
}
void AsyncServer::startTimer(timerInfo* t) {

	auto prep_timeout = [this](StateMgmtIntf *instance) {
		struct __kernel_timespec ts;
		struct timeval tv;
		// std::cout <<(void*)(this->ring_) << std::endl;
		auto sqe = io_uring_get_sqe(this->ring_);
		if (!sqe) {
			fprintf(stderr, "%s: get sqe failed\n", __FUNCTION__);
			return;
		}
		msec_to_ts(&ts, (instance->getTimerInfo()->duration) * 1000);
		io_uring_prep_timeout(sqe, &ts, 0, 0);
		sqe->user_data = (__u64)(instance); // important
		auto ret = io_uring_submit(this->ring_);
		if (ret <= 0) {
			fprintf(stderr, "%s: sqe submit failed: %d\n", __FUNCTION__, ret);
			return;
		}
	};
	auto handle_CQE_timeout = [this,_prep_timeout = prep_timeout]
		(StateMgmtIntf *instance,io_uring_cqe *cqe) mutable {
		auto _t = instance->getTimerInfo();
		if(_t->remainCount > 0) {
			if(_t->repeatIndHandler != nullptr)
				_t->repeatIndHandler();
			_t->remainCount--;
		}else {
			if(_t->expiryIndHandler != nullptr)
				_t->expiryIndHandler();
			_t->isExpired = true;
		}
	};
	
	client_info cli; // TODO FIX
	new StateMgmt<decltype(prep_timeout),decltype(handle_CQE_timeout)>
	(
        this->ring_,
        -1,
		-1, 
        this,
        prep_timeout,
        handle_CQE_timeout,
		TIMEOUT, // this instance of type 
		false,
		cli,
		t
	);
	std::cout << t->tname <<" started" << std::endl;
}

void AsyncServer::stopTimer(timerInfo* t) {
	
}
// we need to call this Run() method from ecterna module
void AsyncServer::Run() {
	struct sockaddr_in saddr_tcp,saddr_udp,saddr_sctp;
	int ret;

	if(isSocketServer) {
		memset(&saddr_tcp, 0, sizeof(saddr_tcp));
		saddr_tcp.sin_family = AF_INET;
		saddr_tcp.sin_addr.s_addr = htonl(INADDR_ANY);
		saddr_tcp.sin_port = htons(PORT_TCP);

		memset(&saddr_udp, 0, sizeof(saddr_udp));
		saddr_udp.sin_family = AF_INET;
		saddr_udp.sin_addr.s_addr = htonl(INADDR_ANY);
		saddr_udp.sin_port = htons(PORT_UDP);

		memset(&saddr_sctp, 0, sizeof(saddr_sctp));
		saddr_sctp.sin_family = AF_INET;
		saddr_sctp.sin_addr.s_addr = htonl(INADDR_ANY);
		saddr_sctp.sin_port = htons(PORT_SCTP);

		if(this->server_type & SOCK_TCP) {
			sockfd_tcp = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		}
		if(this->server_type & SOCK_UDP) {
			sockfd_udp = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
		}
		if(this->server_type & SOCK_SCTP) {
			sockfd_sctp = socket(AF_INET, SOCK_STREAM, IPPROTO_SCTP);
		}
		
		if (sockfd_tcp < 0 && sockfd_udp < 0 && sockfd_sctp < 0) {
			perror("socket");
			// return;
		}
		
		int32_t val = 1;

		if(sockfd_tcp > 0) {
			assert(setsockopt(sockfd_tcp, SOL_SOCKET, SO_REUSEPORT, &val, 
				sizeof(val)) != -1);
			assert(setsockopt(sockfd_tcp, SOL_SOCKET, SO_REUSEADDR, &val, 
				sizeof(val)) != -1);
			ret = bind(sockfd_tcp, (struct sockaddr *)&saddr_tcp, sizeof(saddr_tcp));
			if (ret < 0) {
				perror("bind");
				return;
			}
			if ((listen(sockfd_tcp, 5)) != 0) { 
				printf("Listen failed...\n"); 
				exit(0); 
			} 
			else
				printf("TCP server listening...\n");
		}
		if(sockfd_udp > 0) {
			assert(setsockopt(sockfd_udp, SOL_SOCKET, SO_REUSEPORT, &val, sizeof(val)) != -1);
			assert(setsockopt(sockfd_udp, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val)) != -1);
			ret = bind(sockfd_udp, (struct sockaddr *)&saddr_udp, sizeof(saddr_udp));
			if (ret < 0) {
				perror("bind");
				return;
			}
			printf("UDP server listening...\n");
		}
		if(sockfd_sctp > 0) {
			assert(setsockopt(sockfd_sctp, SOL_SOCKET, SO_REUSEPORT, &val, 
				sizeof(val)) != -1);
			assert(setsockopt(sockfd_sctp, SOL_SOCKET, SO_REUSEADDR, &val, 
				sizeof(val)) != -1);
			ret = bind(sockfd_sctp, (struct sockaddr *)&saddr_sctp, 
				sizeof(saddr_sctp));
			if (ret < 0) {
				perror("bind");
				return;
			}
			if ((listen(sockfd_sctp, 5)) != 0) { 
				printf("Listen failed...\n"); 
				exit(0); 
			} 
			else
				printf("SCTP server listening...\n");
		}
		std::cout <<"Server socket created " << std::endl;
	}
	// create a new ring
    // this->ring_ = new io_uring();
	// assert(io_uring_queue_init(32, this->ring_, 0) >= 0);
    
    HandleRequest();
}
void AsyncServer::registerHandler(callBackFunction handler) {
	callback[0] = handler;
}
void AsyncServer::InitiateRequest() {
	/******* prepare and define the handler functions for read operation ******/	

	////////////////////////////////////////////////////////////////////////////
	auto prep_RECV = [this](StateMgmtIntf* instance) {
		// std::cout << __FUNCTION__ <<" called prep_RECV"<< std::endl;
		// std::lock_guard<std::mutex> lock(mtx);
		// std::cout <<"> this : "<<this << std::endl;
		// std::cout <<"> this ring : "<<this->ring_ << std::endl;
		auto sqe = io_uring_get_sqe(this->ring_);
		io_uring_prep_rw(IORING_OP_RECV, sqe,instance->getReadFd(), 
						instance->getBuffer(), READ_BUF_SIZE, 0);
		sqe->user_data = (__u64)(instance); // important
		auto ret = io_uring_submit(this->ring_);
		if (ret <= 0) {
			fprintf(stderr, "%s: sqe submit failed: %d\n", __FUNCTION__, ret);
		}
	};
	auto handle_CQE_PrintMsg = [this](StateMgmtIntf* instance,io_uring_cqe *cqe){
		//std::cout << __FUNCTION__ <<" called handle_CQE_PrintMsg"<< std::endl;
		// std::cout <<"Number of bytes read : " << cqe->res << std::endl;
		// std::string x((char*)(instance->getBuffer()),cqe->res/*TODO replace 
		// with ??*/);
		// std::cout <<"Client data : " << x << std::endl;
		if(callback[0] != nullptr) {
			callback[0](this,instance->getBuffer(),cqe->res,
				instance->getClientInfo());
		}
		else 
			std::cerr << "callback not registered" << std::endl;
		return true;
	};

	if(sockfd_udp != -1) {
		new StateMgmt<decltype(prep_RECV),decltype(handle_CQE_PrintMsg)>
		(
			this->ring_,
			this->sockfd_udp, // this instance of StateMgmt is created for UDP 
			// server read
			-1, 
			this,
			prep_RECV,
			handle_CQE_PrintMsg,
			TCP_READ
		);
	}
	/**************************************************************************/

	/***** prepare and define the handler functions for accept Operation ******/
	auto prep_ACCEPT = [this](StateMgmtIntf* instance) {
		// std::cout << __FUNCTION__ <<" called prep_ACCEPT"<< std::endl;
		//std::lock_guard<std::mutex> lock(mtx);
		// std::cout <<"# this : "<<this << std::endl;
		// std::cout <<"# this ring : "<<this->ring_ << std::endl;
		auto sqe = io_uring_get_sqe(this->ring_);
		int len = sizeof(sockaddr_in);
		io_uring_prep_rw(IORING_OP_ACCEPT, sqe, this->sockfd_tcp, 
			instance->getClientSockAddr(), 0, (__u64)(unsigned long)(&len));
		sqe->user_data = (__u64)(instance); // important
		auto ret = io_uring_submit(this->ring_);
		if (ret <= 0) {
			fprintf(stderr, "%s: sqe submit failed: %d\n", __FUNCTION__, ret);
		}
	};
	auto handle_CQE_AcceptHandle = [this,prep_RECV,handle_CQE_PrintMsg]
		(StateMgmtIntf* instance,io_uring_cqe *cqe) mutable {
		char hbuf[NI_MAXHOST], sbuf[NI_MAXSERV];
        if (getnameinfo((const sockaddr*)instance->getClientSockAddr(), 
						sizeof(sockaddr_in), hbuf, sizeof(hbuf), sbuf,
                		sizeof(sbuf), NI_NUMERICHOST | NI_NUMERICSERV) == 0) {
			
			instance->setClientAddr(std::string(hbuf,strlen(hbuf)));
			instance->setClientPort(std::string(sbuf,strlen(sbuf)));
		}
		std::string key = instance->getClientInfo()->to_string();
		clientFdMap[key] = cqe->res;
		fdClientMap[cqe->res] = key;
		new StateMgmt<decltype(prep_RECV),decltype(handle_CQE_PrintMsg)>
		(
			this->ring_,
			cqe->res, // this instance of StateMgmt is created for TCP client fd
			-1, 
			this,
			prep_RECV, // reuse the prep_RECV function used in the UDP case
			handle_CQE_PrintMsg, // reuse the handle function for UDP CQE case
			TCP_READ,
			false,	// this is a TCP client fd read operation
			*(instance->getClientInfo())
		);
	};

	if(sockfd_tcp != -1) {
		new StateMgmt<decltype(prep_ACCEPT),decltype(handle_CQE_AcceptHandle)>
		(
			this->ring_,
			-1,
			this->sockfd_tcp,
			this,
			prep_ACCEPT,
			handle_CQE_AcceptHandle,
			ACCEPT
		);
	}
	/**************************************************************************/
}

void AsyncServer::HandleRequest() {
	InitiateRequest();
    // LOOP
	while(true) {
		// Do I need to use lock here ?
		struct io_uring_cqe *cqe;
		io_uring_wait_cqe(ring_, &cqe);
		//std::cout <<"cqe->res " << cqe->res <<"cqe->flags "<<cqe->flags <<" errno "
		//	<< errno << std::endl;
		if (cqe->res == -EINVAL) {
			fprintf(stdout, "recv not supported, skipping\n");
		}
		if((StateMgmtIntf*)(cqe->user_data) != nullptr) {
			((StateMgmtIntf*)(cqe->user_data))->Proceed(cqe);
		}else {
			std::cerr <<"Error in the cqe!" << std::endl;
		}
		// std::cout <<"One cqe done! flags : "<<cqe->flags << std::endl;
		io_uring_cqe_seen(ring_, cqe);
	}
}