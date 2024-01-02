


#ifndef __NTY_COROUTINE_H__
#define __NTY_COROUTINE_H__


#define _GNU_SOURCE
#include <dlfcn.h>


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>
#include <assert.h>
#include <inttypes.h>
#include <unistd.h>
#include <pthread.h>
#include <fcntl.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <netinet/tcp.h>

#include <sys/epoll.h>
#include <sys/poll.h>

#include <errno.h>

#include "nty_queue.h"
#include "nty_tree.h"

#define NTY_CO_MAX_EVENTS		(1024*1024)
#define NTY_CO_MAX_STACKSIZE	(128*1024) // {http: 16*1024, tcp: 4*1024}

#define BIT(x)	 				(1 << (x)) //计算2的幂次方
#define CLEARBIT(x) 			~(1 << (x))

#define CANCEL_FD_WAIT_UINT64	1

typedef void (*proc_coroutine)(void *);


typedef enum {
	NTY_COROUTINE_STATUS_WAIT_READ,
	NTY_COROUTINE_STATUS_WAIT_WRITE,
	NTY_COROUTINE_STATUS_NEW,
	NTY_COROUTINE_STATUS_READY,
	NTY_COROUTINE_STATUS_EXITED,
	NTY_COROUTINE_STATUS_BUSY,
	NTY_COROUTINE_STATUS_SLEEPING,
	NTY_COROUTINE_STATUS_EXPIRED,
	NTY_COROUTINE_STATUS_FDEOF,
	NTY_COROUTINE_STATUS_DETACH,
	NTY_COROUTINE_STATUS_CANCELLED,
	NTY_COROUTINE_STATUS_PENDING_RUNCOMPUTE,
	NTY_COROUTINE_STATUS_RUNCOMPUTE,
	NTY_COROUTINE_STATUS_WAIT_IO_READ,
	NTY_COROUTINE_STATUS_WAIT_IO_WRITE,
	NTY_COROUTINE_STATUS_WAIT_MULTI
} nty_coroutine_status;

typedef enum {
	NTY_COROUTINE_COMPUTE_BUSY,
	NTY_COROUTINE_COMPUTE_FREE
} nty_coroutine_compute_status;

typedef enum {
	NTY_COROUTINE_EV_READ,
	NTY_COROUTINE_EV_WRITE
} nty_coroutine_event;


LIST_HEAD(_nty_coroutine_link, _nty_coroutine);
TAILQ_HEAD(_nty_coroutine_queue, _nty_coroutine);

RB_HEAD(_nty_coroutine_rbtree_sleep, _nty_coroutine);
RB_HEAD(_nty_coroutine_rbtree_wait, _nty_coroutine);



typedef struct _nty_coroutine_link nty_coroutine_link;
typedef struct _nty_coroutine_queue nty_coroutine_queue;

typedef struct _nty_coroutine_rbtree_sleep nty_coroutine_rbtree_sleep;
typedef struct _nty_coroutine_rbtree_wait nty_coroutine_rbtree_wait;


typedef struct _nty_cpu_ctx {
	void *esp; //存储了当前栈的顶部地址
	void *ebp; //基址指针寄存器，它通常用于指向当前函数的堆栈帧的底部
	void *eip; //指令指针寄存器，它存储了CPU当前正在执行的指令的内存地址（程序计数器），也就是下一条指令的地址
	void *edi;
	void *esi;
	void *ebx;
	void *r1;
	void *r2;
	void *r3;
	void *r4;
	void *r5;
} nty_cpu_ctx;

///
/*调度器*/
typedef struct _nty_schedule {
	uint64_t birth; //创建时间
	nty_cpu_ctx ctx;
	void *stack;
	size_t stack_size;
	int spawned_coroutines; //协程的数量
	uint64_t default_timeout; //默认超时时间
	struct _nty_coroutine *curr_thread; //当前执行的协程
	int page_size;

	int poller_fd; //epoll句柄
	int eventfd;  //事件对象  一个进程间通信的文件描述符用于协程间发生通知
	struct epoll_event eventlist[NTY_CO_MAX_EVENTS]; //存储就绪的协程
	int nevents;

	int num_new_events; //发生事件的数量epoll_wait的返回值
	pthread_mutex_t defer_mutex;
/*
-----------------------------
	存储各个状态的协程数据结构
	用于管理各个协程
	对于睡眠 的红黑树如果插入的时候超时的时间是一样的，可以在插入的时候加一个判断 如果一样可以让它超时时间加 那么一丢丢然后再插入
*/
	nty_coroutine_queue ready;
	nty_coroutine_queue defer;

	nty_coroutine_link busy;
	
	nty_coroutine_rbtree_sleep sleeping;
	nty_coroutine_rbtree_wait waiting;

	//private 

} nty_schedule;

/*类比线程*/
//协程的定义
typedef struct _nty_coroutine {

	//private
	
	nty_cpu_ctx ctx; //cpu寄存器组
	proc_coroutine func; //入口函数
	void *arg; //入口的参数
	void *data; //协程的返回值
	/*协程的属性栈
	  独立栈
	  如果用共享栈不好维护对于使用策略啥的，溢出啥的
	  使用独立栈就算一个协程分配4k，一百万个协程才4G了
	*/
	/*栈空间大小的设置：
		依据用途：如果是一些字符操作，发送一些你好之类的4k就可以了
		如果对于有些文件传输，文件系统的，可以定义到10M~20M都是可以的*/
	size_t stack_size;
	size_t last_stack_size;
	
	nty_coroutine_status status; //协程的状态
	nty_schedule *sched;  //线程数据键的值

	uint64_t birth; //创建时间
	uint64_t id; //协程id
#if CANCEL_FD_WAIT_UINT64
	int fd;
	unsigned short events;  //POLL_EVENT
#else
	int64_t fd_wait;
#endif
	char funcname[64];
	struct _nty_coroutine *co_join;  //类比链表，把成千上万的协程组织起来

	void **co_exit_ptr;
	void *stack; //协程栈底  协程的栈用于函数使用（调用函数函数返回压栈和出栈）
	void *ebp;
	uint32_t ops;//CPU使用调度策略
	uint64_t sleep_usecs; //需要睡眠的微秒数
	/*各个状态的协程节点的存储的结构的选择：
	 sleep的结构体为什么选择红黑树：
	 	里面存储了睡眠的时间，要有个排序的存在(时间的先后)
	*/

	/*
	把所有的节点都存在就绪队列ready里执行一个移除就行，避免了 删除添加的加锁行为
	*/
	RB_ENTRY(_nty_coroutine) sleep_node; //睡眠协程的结构体  集合
	RB_ENTRY(_nty_coroutine) wait_node;  //等待协程的结构体  集合

	LIST_ENTRY(_nty_coroutine) busy_next;

	TAILQ_ENTRY(_nty_coroutine) ready_next;
	TAILQ_ENTRY(_nty_coroutine) defer_next;
	TAILQ_ENTRY(_nty_coroutine) cond_next;

	TAILQ_ENTRY(_nty_coroutine) io_next;
	TAILQ_ENTRY(_nty_coroutine) compute_next;

	struct {
		void *buf;
		size_t nbytes;
		int fd;
		int ret;
		int err;
	} io;

	struct _nty_coroutine_compute_sched *compute_sched;
	int ready_fds;
	struct pollfd *pfds;
	nfds_t nfds;
} nty_coroutine;


typedef struct _nty_coroutine_compute_sched {
	nty_cpu_ctx ctx;
	nty_coroutine_queue coroutines;

	nty_coroutine *curr_coroutine;

	pthread_mutex_t run_mutex;
	pthread_cond_t run_cond;

	pthread_mutex_t co_mutex;
	LIST_ENTRY(_nty_coroutine_compute_sched) compute_next;
	
	nty_coroutine_compute_status compute_status;
} nty_coroutine_compute_sched;

extern pthread_key_t global_sched_key;
/*获取线程特定数据键的值*/
static inline nty_schedule *nty_coroutine_get_sched(void) {
	/*获取指定线程的线程特定数据（TSD）的值（线程特定数据键的值）*/
	return pthread_getspecific(global_sched_key);
}

static inline uint64_t nty_coroutine_diff_usecs(uint64_t t1, uint64_t t2) {
	return t2-t1;
}

//当前时间的微秒数
static inline uint64_t nty_coroutine_usec_now(void) {
	struct timeval t1 = {0, 0};
	//获取当前的时间
	gettimeofday(&t1, NULL);

	return t1.tv_sec * 1000000 + t1.tv_usec;
}



int nty_epoller_create(void);


void nty_schedule_cancel_event(nty_coroutine *co);
void nty_schedule_sched_event(nty_coroutine *co, int fd, nty_coroutine_event e, uint64_t timeout);

void nty_schedule_desched_sleepdown(nty_coroutine *co);
void nty_schedule_sched_sleepdown(nty_coroutine *co, uint64_t msecs);

nty_coroutine* nty_schedule_desched_wait(int fd);
void nty_schedule_sched_wait(nty_coroutine *co, int fd, unsigned short events, uint64_t timeout);

void nty_schedule_run(void);

int nty_epoller_ev_register_trigger(void);
int nty_epoller_wait(struct timespec t);
int nty_coroutine_resume(nty_coroutine *co);
void nty_coroutine_free(nty_coroutine *co);
int nty_coroutine_create(nty_coroutine **new_co, proc_coroutine func, void *arg);
void nty_coroutine_yield(nty_coroutine *co);

void nty_coroutine_sleep(uint64_t msecs);


int nty_socket(int domain, int type, int protocol);
int nty_accept(int fd, struct sockaddr *addr, socklen_t *len);
ssize_t nty_recv(int fd, void *buf, size_t len, int flags);
ssize_t nty_send(int fd, const void *buf, size_t len, int flags);
int nty_close(int fd);
int nty_poll(struct pollfd *fds, nfds_t nfds, int timeout);
int nty_connect(int fd, struct sockaddr *name, socklen_t namelen);

ssize_t nty_sendto(int fd, const void *buf, size_t len, int flags,
               const struct sockaddr *dest_addr, socklen_t addrlen);
ssize_t nty_recvfrom(int fd, void *buf, size_t len, int flags,
                 struct sockaddr *src_addr, socklen_t *addrlen);


#define COROUTINE_HOOK 

#ifdef  COROUTINE_HOOK


typedef int (*socket_t)(int domain, int type, int protocol);
extern socket_t socket_f;

typedef int(*connect_t)(int, const struct sockaddr *, socklen_t);
extern connect_t connect_f;

typedef ssize_t(*read_t)(int, void *, size_t);
extern read_t read_f;


typedef ssize_t(*recv_t)(int sockfd, void *buf, size_t len, int flags);
extern recv_t recv_f;

typedef ssize_t(*recvfrom_t)(int sockfd, void *buf, size_t len, int flags,
        struct sockaddr *src_addr, socklen_t *addrlen);
extern recvfrom_t recvfrom_f;

typedef ssize_t(*write_t)(int, const void *, size_t);
extern write_t write_f;

typedef ssize_t(*send_t)(int sockfd, const void *buf, size_t len, int flags);
extern send_t send_f;

typedef ssize_t(*sendto_t)(int sockfd, const void *buf, size_t len, int flags,
        const struct sockaddr *dest_addr, socklen_t addrlen);
extern sendto_t sendto_f;

typedef int(*accept_t)(int sockfd, struct sockaddr *addr, socklen_t *addrlen);
extern accept_t accept_f;

// new-syscall
typedef int(*close_t)(int);
extern close_t close_f;


int init_hook(void);


/*

typedef int(*fcntl_t)(int __fd, int __cmd, ...);
extern fcntl_t fcntl_f;

typedef int (*getsockopt_t)(int sockfd, int level, int optname,
        void *optval, socklen_t *optlen);
extern getsockopt_t getsockopt_f;

typedef int (*setsockopt_t)(int sockfd, int level, int optname,
        const void *optval, socklen_t optlen);
extern setsockopt_t setsockopt_f;

*/

#endif



#endif


