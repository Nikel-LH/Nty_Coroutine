

#include "nty_coroutine.h"

pthread_key_t global_sched_key; //数据键与调度器绑定，做个单例
static pthread_once_t sched_key_once = PTHREAD_ONCE_INIT;



/*切换下一个位置的值 当前的值（寄存器的值）*/
/*这个函数可以用于保存和恢复函数调用的栈帧，以便在进行子函数调用时恢复现场，返回时再次保存现场*/
int _switch(nty_cpu_ctx *new_ctx, nty_cpu_ctx *cur_ctx);

#ifdef __i386__
__asm__ (
"    .text                                  \n"
"    .p2align 2,,3                          \n"
".globl _switch                             \n"
"_switch:                                   \n"
"__switch:                                  \n"
"movl 8(%esp), %edx      # fs->%edx         \n"
"movl %esp, 0(%edx)      # save esp         \n"
"movl %ebp, 4(%edx)      # save ebp         \n"
"movl (%esp), %eax       # save eip         \n"
"movl %eax, 8(%edx)                         \n"
"movl %ebx, 12(%edx)     # save ebx,esi,edi \n"
"movl %esi, 16(%edx)                        \n"
"movl %edi, 20(%edx)                        \n"
"movl 4(%esp), %edx      # ts->%edx         \n"
"movl 20(%edx), %edi     # restore ebx,esi,edi      \n"
"movl 16(%edx), %esi                                \n"
"movl 12(%edx), %ebx                                \n"
"movl 0(%edx), %esp      # restore esp              \n"
"movl 4(%edx), %ebp      # restore ebp              \n"
"movl 8(%edx), %eax      # restore eip              \n"
"movl %eax, (%esp)                                  \n"
"ret                                                \n"
);
/*这段代码是一段使用 GCC 内联汇编实现的函数，用于在 x86-64 架构的机器上保存和恢复函数调用的现场。
这个函数被命名为 _switch 和 __switch，可以通过这两个名称来调用。*/
#elif defined(__x86_64__)
//rdi代表_switch的第一个参数new_ctx,rsi代表第二个参数cur_ctx(堆栈不需要改变)
//没有改变堆栈（针对临时变量没有什么意义，重新定义）
__asm__ (
"    .text                                  \n"
"       .p2align 4,,15                                   \n"
".globl _switch                                          \n"
".globl __switch                                         \n"
"_switch:                                                \n"
"__switch:                                               \n"
"       movq %rsp, 0(%rsi)      # save stack_pointer     \n"
"       movq %rbp, 8(%rsi)      # save frame_pointer     \n"
"       movq (%rsp), %rax       # save insn_pointer      \n"
"       movq %rax, 16(%rsi)                              \n"
"       movq %rbx, 24(%rsi)     # save rbx,r12-r15       \n"
"       movq %r12, 32(%rsi)                              \n"
"       movq %r13, 40(%rsi)                              \n"
"       movq %r14, 48(%rsi)                              \n"
"       movq %r15, 56(%rsi)                              \n"
"       movq 56(%rdi), %r15                              \n"
"       movq 48(%rdi), %r14                              \n"
"       movq 40(%rdi), %r13     # restore rbx,r12-r15    \n"
"       movq 32(%rdi), %r12                              \n"
"       movq 24(%rdi), %rbx                              \n"
"       movq 8(%rdi), %rbp      # restore frame_pointer  \n"
"       movq 0(%rdi), %rsp      # restore stack_pointer  \n"
"       movq 16(%rdi), %rax     # restore insn_pointer   \n"
"       movq %rax, (%rsp)                                \n"
"       ret                                              \n"
);
#endif

/*实现了协程的执行和上下文的切换，使得协程可以在需要的时候进行挂起和恢复 */
/*执行函数*/
static void _exec(void *lt) {
//用于在编译时判断是否为特定平台（（x86-64架构）和编译器（LVM））并开启汇编代码块。
#if defined(__lvm__) && defined(__x86_64__)
	//汇编代码的作用是将当前函数堆栈帧中第二个参数（即协程指针）的值保存到一个寄存器中，以便在后续代码中使用
	__asm__("movq 16(%%rbp), %[lt]" : [lt] "=r" (lt));
#endif

	nty_coroutine *co = (nty_coroutine*)lt;
	co->func(co->arg);
	//表示协程已退出、文件结束和协程已分离。
	co->status |= (BIT(NTY_COROUTINE_STATUS_EXITED) | BIT(NTY_COROUTINE_STATUS_FDEOF) | BIT(NTY_COROUTINE_STATUS_DETACH));
#if 1
	/*切换下一个位置的值 当前的值（寄存器的值）*/
	//该操作将协程挂起，并切换到下一个可运行的协程
	nty_coroutine_yield(co);
#else
	co->ops = 0;
	_switch(&co->sched->ctx, &co->ctx);
#endif
}

extern int nty_schedule_create(int stack_size);


/*释放一个指定的协程占用的资源，包括协程栈空间和协程结构体本身*/
void nty_coroutine_free(nty_coroutine *co) {
	if (co == NULL) return ;
	co->sched->spawned_coroutines --;
#if 1
	if (co->stack) {
		free(co->stack);
		co->stack = NULL;
	}
#endif
	if (co) {
		free(co);
	}

}
/*设置栈空间
执行协程函数
*/
static void nty_coroutine_init(nty_coroutine *co) {
	//栈底加上栈大小
	void **stack = (void **)(co->stack + co->stack_size);

	stack[-3] = NULL;
	stack[-2] = (void *)co;

	co->ctx.esp = (void*)stack - (4 * sizeof(void*)); //栈指针寄存器，它存储了当前栈的顶部地址
	co->ctx.ebp = (void*)stack - (3 * sizeof(void*)); //基址指针寄存器，它通常用于指向当前函数的堆栈帧的底部
	co->ctx.eip = (void*)_exec; //存储了CPU当前正在执行的指令的内存地址（程序计数器），也就是下一条指令的地址
	co->status = BIT(NTY_COROUTINE_STATUS_READY); //协程已经准备就绪
	
}

/*将当前协程暂停并切换到其他协程执行*/
void nty_coroutine_yield(nty_coroutine *co) {
	//通过将 ops 设置为 0，协程的状态被标记为挂起状态，以便在下一次调度时不会再次执行这个协程，而是执行其他可运行的协程
	co->ops = 0;
	//将协程 co 的上下文切换为调度器的上下文，从而切换到下一个可运行的协程上
	/*_switch 函数的作用是将当前的 CPU 寄存器状态保存到协程的上下文中，然后将调度器的上下文切换为当前 CPU 的状态。
	这样，当前协程的执行就被暂停了，调度器将会选择一个可运行的协程来执行，直到这个协程再次被唤醒*/
	_switch(&co->sched->ctx, &co->ctx);
}

/*管理协程（nty_coroutine）的栈内存*/
/*在协程的栈空间被缩小时，使用 MADV_DONTNEED 提示内核释放不再需要的栈空间，
以便系统可以将这些空间分配给其他程序使用，从而提高系统的整体性能。
*/
static inline void nty_coroutine_madvise(nty_coroutine *co) {
	//计算出当前协程栈的使用大小（current_stack）
	size_t current_stack = (co->stack + co->stack_size) - co->ctx.esp;
	assert(current_stack <= co->stack_size);
	//与上一次协程栈的使用大小（co->last_stack_size）进行比较
	if (current_stack < co->last_stack_size &&
		co->last_stack_size > co->sched->page_size) {
		size_t tmp = current_stack + (-current_stack & (co->sched->page_size - 1));
		/*用于向内核提供有关内存区域使用方式的提示，从而优化内存的使用*/
		/*使用madvise函数将之前未使用的部分（即当前协程栈的尾部）释放回操作系统，以便操作系统能够更好地管理内存。
		madvise函数的MADV_DONTNEED参数表示告诉操作系统，这部分内存可以被释放。*/
		assert(madvise(co->stack, co->stack_size-tmp, MADV_DONTNEED) == 0);
	}
	co->last_stack_size = current_stack;
}

/*恢复一个指定的协程，并让其开始执行相应的任务*/
int nty_coroutine_resume(nty_coroutine *co) {
	/*判断协程状态是否为新建状态，即 NTY_COROUTINE_STATUS_NEW，
	如果是，则调用 nty_coroutine_init() 函数初始化协程的上下文，为其设置栈空间和执行入口等必要参数。*/
	if (co->status & BIT(NTY_COROUTINE_STATUS_NEW)) {
		//设置栈空间以及执行协程函数
		nty_coroutine_init(co);
	}
	//_switch(&co->ctx, &co->sched->ctx);
	nty_schedule *sched = nty_coroutine_get_sched();
	sched->curr_thread = co;
	_switch(&co->ctx, &co->sched->ctx);
	sched->curr_thread = NULL;
	//协程栈内存的管理
	nty_coroutine_madvise(co);
#if 1
	if (co->status & BIT(NTY_COROUTINE_STATUS_EXITED)) {
		if (co->status & BIT(NTY_COROUTINE_STATUS_DETACH)) {
			printf("nty_coroutine_resume --> \n");
			nty_coroutine_free(co);
		}
		return -1;
	} 
#endif
	return 0;
}

//一种调度策略，避免一个协程频繁的占用CPU后面的协程占用不到
/*将协程的ops计数器加1。然后，如果ops计数器小于5，该函数将直接返回，不做任何操作。
否则，该函数将使用TAILQ_INSERT_TAIL()函数将协程插入到调度器的就绪队列中，以等待下一次调度。
然后，该函数使用nty_coroutine_yield()函数强制让出CPU，并将控制权交给调度器，以便调度器可以选择执行其他协程。*/
void nty_coroutine_renice(nty_coroutine *co) {
	co->ops ++;
#if 1
	if (co->ops < 5) return ;
#endif
	printf("nty_coroutine_renice\n");
	TAILQ_INSERT_TAIL(&nty_coroutine_get_sched()->ready, co, ready_next);
	printf("nty_coroutine_renice 111\n");
	nty_coroutine_yield(co);
}

/*将当前协程暂停一段时间并切换到其他协程执行*/
void nty_coroutine_sleep(uint64_t msecs) {
	nty_coroutine *co = nty_coroutine_get_sched()->curr_thread;

	/*将当前协程插入到协程调度器的就绪队列 ready 的尾部，并调用"yield" 函数将控制权切换到其他就绪协程执行*/
	if (msecs == 0) {
		TAILQ_INSERT_TAIL(&co->sched->ready, co, ready_next);
		nty_coroutine_yield(co);
	} else {
		nty_schedule_sched_sleepdown(co, msecs);
	}
}

//将当前协程进行分离
void nty_coroutine_detach(void) {
	nty_coroutine *co = nty_coroutine_get_sched()->curr_thread;
	co->status |= BIT(NTY_COROUTINE_STATUS_DETACH);
}
//数据键的释放，自动调用
static void nty_coroutine_sched_key_destructor(void *data) {
	free(data);
}

static void nty_coroutine_sched_key_creator(void) {
	/*用于创建线程特定数据键
	线程特定数据键是一个线程局部存储的变量，它可以在多个线程之间共享，但每个线程都有自己的副本，因此可以独立地修改和访问
	第一个参数key是一个指向pthread_key_t类型的指针，用于*接收*新创建的线程特定数据键的标识符；
	第二个参数destructor是一个函数指针，用于在线程退出时自动调用，释放线程特定数据键所关联的内存空间
	*/
	assert(pthread_key_create(&global_sched_key, nty_coroutine_sched_key_destructor) == 0);
	/*用于将一个线程特定数据键（Thread-Specific Data Key）与一个指针关联起来，以便于在多个线程之间共享数据
	第一个参数key是线程特定数据键的标识符，由pthread_key_create函数创建；
	第二个参数value是一个指针，用于指向需要与该线程特定数据键关联的数据
	*/
	assert(pthread_setspecific(global_sched_key, NULL) == 0);
	
	return ;
}


// coroutine --> 
// create 
//
/*所作内容:
	(1)创建线程数据键(单例模式)
	(2)将数据键与调度器绑定并且初始化这个调度器
	(3)初始化协程结构体指针(status指向为新建态)
	(4)将该协程设置为ready状态，并且添加到就绪队列
*/
int nty_coroutine_create(nty_coroutine **new_co, proc_coroutine func, void *arg) {

	/*pthread_once函数是一个线程控制函数，用于保证在多线程环境下一个函数只被执行一次
	第一个参数once_control是一个指向pthread_once_t类型的变量的指针，用于标识该函数是否已经被执行；
	第二个参数init_routine是一个函数指针，指向需要被执行一次的函数*/
	assert(pthread_once(&sched_key_once, nty_coroutine_sched_key_creator) == 0);//用与线程数据键的创建(表示数据键只被创建一次)
	/*获取线程特定数据键的值*/
	nty_schedule *sched = nty_coroutine_get_sched();

	/*当前线程没有与该TSD键关联的数据，那么该函数将返回NULL
	*/
	if (sched == NULL) {
		nty_schedule_create(0);//实现数据键与调度器的绑定，并且初始化调度器
		
		sched = nty_coroutine_get_sched();
		if (sched == NULL) {
			printf("Failed to create scheduler\n");
			return -1;
		}
	}

	nty_coroutine *co = calloc(1, sizeof(nty_coroutine));
	if (co == NULL) {
		printf("Failed to allocate memory for new coroutine\n");
		return -2;
	}
	/*分配指定对齐方式的内存块，并返回该内存块的首地址
	  第一个参数一个指向指针的指针，用于存储分配的内存块的首地址
	  第二个参数分配内存块的对齐方式，必须是2的整数次幂
	  第三个参数要分配的内存块的大小
	  在堆上分配一块内存，并将该内存块的首地址存储在memptr指向的指针中
	  getpagesize函数的返回值是系统的内存页面大小
	*/
	int ret = posix_memalign(&co->stack, getpagesize(), sched->stack_size);
	if (ret) {
		printf("Failed to allocate stack for new coroutine\n");
		free(co);
		return -3;
	}

	co->sched = sched;
	co->stack_size = sched->stack_size;
	co->status = BIT(NTY_COROUTINE_STATUS_NEW); //
	co->id = sched->spawned_coroutines ++; //协程序号数量
	co->func = func; //入口函数
#if CANCEL_FD_WAIT_UINT64
	co->fd = -1;
	co->events = 0;
#else
	co->fd_wait = -1;
#endif
	co->arg = arg;
	co->birth = nty_coroutine_usec_now(); //获取当前时间的微妙数
	*new_co = co;

	TAILQ_INSERT_TAIL(&co->sched->ready, co, ready_next);

	return 0;
}




