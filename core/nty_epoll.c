
#include <sys/eventfd.h>

#include "nty_coroutine.h"


int nty_epoller_create(void) {
	return epoll_create(1024);
} 
//等待事件的发生
int nty_epoller_wait(struct timespec t) {
	nty_schedule *sched = nty_coroutine_get_sched();
	return epoll_wait(sched->poller_fd, sched->eventlist, NTY_CO_MAX_EVENTS, t.tv_sec*1000.0 + t.tv_nsec/1000000.0);
}
/*创建时间对象，并且上树监听可读*/
int nty_epoller_ev_register_trigger(void) {
	nty_schedule *sched = nty_coroutine_get_sched();

  /*创建一个进程间通信的文件描述符用于协程间发生通知，并且对它上树，监听可读事件*/
	if (!sched->eventfd) {
    /*用于创建一个用于进程间通信的事件文件描述符
    用于创建一个用于在进程之间传递事件通知的"eventfd对象
    第一个参数initval参数指定了eventfd对象的初始值，
    第二个参数flags参数用于设置eventfd对象的标志位。
    函数返回一个用于操作eventfd对象的文件描述符
    */
		sched->eventfd = eventfd(0, EFD_NONBLOCK);
		assert(sched->eventfd != -1);
	}

	struct epoll_event ev;
	ev.events = EPOLLIN;
	ev.data.fd = sched->eventfd;
	int ret = epoll_ctl(sched->poller_fd, EPOLL_CTL_ADD, sched->eventfd, &ev);

	assert(ret != -1);
}


