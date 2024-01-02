
#include "nty_coroutine.h"



#define FD_KEY(f,e) (((int64_t)(f) << (sizeof(int32_t) * 8)) | e)
#define FD_EVENT(f) ((int32_t)(f))
#define FD_ONLY(f) ((f) >> ((sizeof(int32_t) * 8)))


static inline int nty_coroutine_sleep_cmp(nty_coroutine *co1, nty_coroutine *co2) {
	if (co1->sleep_usecs < co2->sleep_usecs) {
		return -1;
	}
	if (co1->sleep_usecs == co2->sleep_usecs) {
		return 0;
	}
	return 1;
}

static inline int nty_coroutine_wait_cmp(nty_coroutine *co1, nty_coroutine *co2) {
#if CANCEL_FD_WAIT_UINT64
	if (co1->fd < co2->fd) return -1;
	else if (co1->fd == co2->fd) return 0;
	else return 1;
#else
	if (co1->fd_wait < co2->fd_wait) {
		return -1;
	}
	if (co1->fd_wait == co2->fd_wait) {
		return 0;
	}
#endif
	return 1;
}

//RB_GENERATE()红黑树的自动生成 的宏
RB_GENERATE(_nty_coroutine_rbtree_sleep, _nty_coroutine, sleep_node, nty_coroutine_sleep_cmp);
RB_GENERATE(_nty_coroutine_rbtree_wait, _nty_coroutine, wait_node, nty_coroutine_wait_cmp);


/*将当前协程设置为睡眠状态，并在一定时间后唤醒该协程*/
/*第一个参数：当前协程的指针；第二个参数：协程睡眠的时间*/
void nty_schedule_sched_sleepdown(nty_coroutine *co, uint64_t msecs) {
	uint64_t usecs = msecs * 1000u; //微妙数
	
	/*保证同一个协程在红黑树中只有一个节点*/
	//查找sleeping任务中的co的节点
	nty_coroutine *co_tmp = RB_FIND(_nty_coroutine_rbtree_sleep, &co->sched->sleeping, co);
	if (co_tmp != NULL) {
		RB_REMOVE(_nty_coroutine_rbtree_sleep, &co->sched->sleeping, co_tmp); //移除
	}

     //需要睡眠的微秒数
	co->sleep_usecs = nty_coroutine_diff_usecs(co->sched->birth, nty_coroutine_usec_now()) + usecs;
	//将当前协程co插入到睡眠任务红黑树中，并将状态设置为
	while (msecs) {
		co_tmp = RB_INSERT(_nty_coroutine_rbtree_sleep, &co->sched->sleeping, co);
		if (co_tmp) {
			printf("1111 sleep_usecs %"PRIu64"\n", co->sleep_usecs);
			co->sleep_usecs ++;
			continue;
		}
		co->status |= BIT(NTY_COROUTINE_STATUS_SLEEPING);
		break;
	}

	//yield
}

void nty_schedule_desched_sleepdown(nty_coroutine *co) {
	if (co->status & BIT(NTY_COROUTINE_STATUS_SLEEPING)) {
		RB_REMOVE(_nty_coroutine_rbtree_sleep, &co->sched->sleeping, co);

		co->status &= CLEARBIT(NTY_COROUTINE_STATUS_SLEEPING);
		co->status |= BIT(NTY_COROUTINE_STATUS_READY);
		co->status &= CLEARBIT(NTY_COROUTINE_STATUS_EXPIRED);
	}
}

/*在调度器的等待树中查找指定文件描述符的协程，并将其状态设置为 0*/
nty_coroutine *nty_schedule_search_wait(int fd) {
	nty_coroutine find_it = {0};
	find_it.fd = fd;

	nty_schedule *sched = nty_coroutine_get_sched();
	//协程的等待状态
	nty_coroutine *co = RB_FIND(_nty_coroutine_rbtree_wait, &sched->waiting, &find_it);
	co->status = 0;

	return co;
}

nty_coroutine* nty_schedule_desched_wait(int fd) {
	
	nty_coroutine find_it = {0};
	find_it.fd = fd;

	nty_schedule *sched = nty_coroutine_get_sched();
	
	nty_coroutine *co = RB_FIND(_nty_coroutine_rbtree_wait, &sched->waiting, &find_it);
	if (co != NULL) {
		RB_REMOVE(_nty_coroutine_rbtree_wait, &co->sched->waiting, co);
	}
	co->status = 0;
	nty_schedule_desched_sleepdown(co);
	
	return co;
}

void nty_schedule_sched_wait(nty_coroutine *co, int fd, unsigned short events, uint64_t timeout) {
	
	if (co->status & BIT(NTY_COROUTINE_STATUS_WAIT_READ) ||
		co->status & BIT(NTY_COROUTINE_STATUS_WAIT_WRITE)) {
		printf("Unexpected event. lt id %"PRIu64" fd %"PRId32" already in %"PRId32" state\n",
            co->id, co->fd, co->status);
		assert(0);
	}

	if (events & POLLIN) {
		co->status |= NTY_COROUTINE_STATUS_WAIT_READ;
	} else if (events & POLLOUT) {
		co->status |= NTY_COROUTINE_STATUS_WAIT_WRITE;
	} else {
		printf("events : %d\n", events);
		assert(0);
	}

	co->fd = fd;
	co->events = events;
	nty_coroutine *co_tmp = RB_INSERT(_nty_coroutine_rbtree_wait, &co->sched->waiting, co);

	assert(co_tmp == NULL);

	//printf("timeout --> %"PRIu64"\n", timeout);
	if (timeout == 1) return ; //Error

	nty_schedule_sched_sleepdown(co, timeout);
	
}

void nty_schedule_cancel_wait(nty_coroutine *co) {
	RB_REMOVE(_nty_coroutine_rbtree_wait, &co->sched->waiting, co);
}

void nty_schedule_free(nty_schedule *sched) {
	if (sched->poller_fd > 0) {
		close(sched->poller_fd);
	}
	if (sched->eventfd > 0) {
		close(sched->eventfd);
	}
	
	free(sched);

	assert(pthread_setspecific(global_sched_key, NULL) == 0);
}
//实现数据键与调度器的绑定，并且初始化调度器
int nty_schedule_create(int stack_size) {

	int sched_stack_size = stack_size ? stack_size : NTY_CO_MAX_STACKSIZE; //默认最大大小

	nty_schedule *sched = (nty_schedule*)calloc(1, sizeof(nty_schedule));
	if (sched == NULL) {
		printf("Failed to initialize scheduler\n");
		return -1;
	}

	assert(pthread_setspecific(global_sched_key, sched) == 0);

	sched->poller_fd = nty_epoller_create();
	if (sched->poller_fd == -1) {
		printf("Failed to initialize epoller\n");
		nty_schedule_free(sched);//关闭epoll，event。free掉sched。将数据键重新置为NULL
		return -2;
	}
	/*创建时间对象，并且上树监听可读，为eventfd赋值*/
	//创建一个用于进程间通信的文件描述符
	nty_epoller_ev_register_trigger();

	sched->stack_size = sched_stack_size;
	//一个系统调用函数，用于获取操作系统中一个页面的大小，即内存分配和管理的最小单位
	sched->page_size = getpagesize();

	sched->spawned_coroutines = 0;
	sched->default_timeout = 3000000u;

	//对各个任务队列红黑树存储结构进行初始化
	RB_INIT(&sched->sleeping); 
	RB_INIT(&sched->waiting);

	sched->birth = nty_coroutine_usec_now(); //调度器创建时间

	TAILQ_INIT(&sched->ready);
	TAILQ_INIT(&sched->defer);
	LIST_INIT(&sched->busy);
	/*用于将一段内存区域中的所有字节清零*/
	bzero(&sched->ctx, sizeof(nty_cpu_ctx));
}


/*获取已经过期的休眠协程并将其从休眠红黑树中删除并返回*/
static nty_coroutine *nty_schedule_expired(nty_schedule *sched) {
	//当前时间与协程创建时间的差值
	uint64_t t_diff_usecs = nty_coroutine_diff_usecs(sched->birth, nty_coroutine_usec_now());
	//从调度器的sleeping红黑树中获取一个最小的nty_coroutine节点（key值最小的节点RB_LEFT）
	nty_coroutine *co = RB_MIN(_nty_coroutine_rbtree_sleep, &sched->sleeping); //休眠时间最短的协程
	if (co == NULL) return NULL;
	/*判断节点co的sleep_usecs值是否小于等于t_diff_usecs。
	如果是，则表示该节点已经超时，需要从sleeping红黑树中移除，并返回该节点；否则，表示该节点还未超时，返回NULL*/
	/*获取到的协程的休眠时间已经过期*/
	if (co->sleep_usecs <= t_diff_usecs) {
		RB_REMOVE(_nty_coroutine_rbtree_sleep, &co->sched->sleeping, co);
		return co;
	}
	return NULL;
}
/*判断调度任务是否已经完成*/
static inline int nty_schedule_isdone(nty_schedule *sched) {
	return (RB_EMPTY(&sched->waiting) && 
		LIST_EMPTY(&sched->busy) &&
		RB_EMPTY(&sched->sleeping) &&
		TAILQ_EMPTY(&sched->ready));
}

/*计算当前调度器中所有睡眠协程到期的最小超时时间*/
//还要多长时间才结束睡眠
static uint64_t nty_schedule_min_timeout(nty_schedule *sched) {
	uint64_t t_diff_usecs = nty_coroutine_diff_usecs(sched->birth, nty_coroutine_usec_now());//当前时间与协程创建时间的差值
	uint64_t min = sched->default_timeout;

	nty_coroutine *co = RB_MIN(_nty_coroutine_rbtree_sleep, &sched->sleeping);
	if (!co) return min;

	min = co->sleep_usecs;
	if (min > t_diff_usecs) {
		return min - t_diff_usecs;
	}

	return 0;
} 

static int nty_schedule_epoll(nty_schedule *sched) {

	sched->num_new_events = 0;

	struct timespec t = {0, 0};
	uint64_t usecs = nty_schedule_min_timeout(sched); //睡眠队列中被唤醒等待的最短时间
	if (usecs && TAILQ_EMPTY(&sched->ready)) {
		t.tv_sec = usecs / 1000000u; //得到秒数
		if (t.tv_sec != 0) {
			t.tv_nsec = (usecs % 1000u) * 1000u;//得到纳秒数
		} else {
			t.tv_nsec = usecs * 1000u;
		}
	} else {
		return 0;
	}

	int nready = 0;
	while (1) {
		//等待事件的发生epoll_wait
		nready = nty_epoller_wait(t);
		if (nready == -1) {
			if (errno == EINTR) continue;
			else assert(0);
		}
		break;
	}

	sched->nevents = 0;
	sched->num_new_events = nready;

	return 0;
}
/*所作内容：
	(1)获取一个睡眠时间最短并且睡眠超时的协程，让其运行
*/
/*调度器*/
void   nty_schedule_run(void) {
	/*获取线程特定数据键的值*/
	nty_schedule *sched = nty_coroutine_get_sched();
	if (sched == NULL) return ;
	//判断调度任务是否已经完成
	//各个存储队列都为空
	while (!nty_schedule_isdone(sched)) {
		
		// 1. expired --> sleep rbtree
		nty_coroutine *expired = NULL;
		/*获取一个睡眠超时的协程，即睡眠时间到了的协程*/
		while ((expired = nty_schedule_expired(sched)) != NULL) {
			//恢复指定的协程，并让其开始执行相应的任务
			nty_coroutine_resume(expired);
		}
		// 2. ready queue
		//返回一个双向链表中的最后一个元素，即末尾节点
		nty_coroutine *last_co_ready = TAILQ_LAST(&sched->ready, _nty_coroutine_queue);
		while (!TAILQ_EMPTY(&sched->ready)) {
			nty_coroutine *co = TAILQ_FIRST(&sched->ready);
			TAILQ_REMOVE(&co->sched->ready, co, ready_next);

			if (co->status & BIT(NTY_COROUTINE_STATUS_FDEOF)) {
				/*释放一个指定的协程占用的资源，包括协程栈空间和协程结构体本身*/
				nty_coroutine_free(co);
				break;
			}
			/*恢复一个指定的协程，并让其开始执行相应的任务*/
			nty_coroutine_resume(co);
			if (co == last_co_ready) break;
		}

		// 3. wait rbtree 就绪的
		//epoll_wait
		nty_schedule_epoll(sched);
		while (sched->num_new_events) {
			int idx = --sched->num_new_events;
			struct epoll_event *ev = sched->eventlist+idx;
			
			int fd = ev->data.fd;
			int is_eof = ev->events & EPOLLHUP;
			if (is_eof) errno = ECONNRESET;
			//在调度器的等待树中查找指定文件描述符的协程，并将其状态设置为 0
			nty_coroutine *co = nty_schedule_search_wait(fd);
			if (co != NULL) {
				if (is_eof) {
					//状态设置为文件描述符已关闭
					co->status |= BIT(NTY_COROUTINE_STATUS_FDEOF);
				}
				//指定一个协程，并让其开始执行相应的任务
				nty_coroutine_resume(co);
			}

			is_eof = 0;
		}
	}

	nty_schedule_free(sched);
	
	return ;
}

