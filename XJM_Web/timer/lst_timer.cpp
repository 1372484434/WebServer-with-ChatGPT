#include "lst_timer.h"
#include "../http/http_conn.h"

//创建头尾节点，其中头尾节点没有意义，仅仅统一方便调整
sort_timer_lst::sort_timer_lst()
{
    head = NULL;
    tail = NULL;
}

sort_timer_lst::~sort_timer_lst()
{
    util_timer *tmp = head;
    while (tmp)
    {
        head = tmp->next;
        delete tmp;
        tmp = head;
    }
}

//add_timer函数，将目标定时器添加到链表中，添加时按照升序添加
//若当前链表中只有头尾节点，直接插入，否则，将定时器按升序插入
void sort_timer_lst::add_timer(util_timer *timer)
{
    if (!timer)
    {
        return;
    }
    if (!head)
    {
        head = tail = timer;
        return;
    }
    //如果新的定时器超时时间小于当前头部结点
    //直接将当前定时器结点作为头部结点
    if (timer->expire < head->expire)
    {
        timer->next = head;
        head->prev = timer;
        head = timer;
        return;
    }
    //否则调用私有成员，调整内部结点
    add_timer(timer, head);
}

//adjust_timer函数，当定时任务发生变化,调整对应定时器在链表中的位置
/*
客户端在设定时间内有数据收发,则当前时刻对该定时器重新设定时间，这里只是往后延长超时时间
被调整的目标定时器在尾部，或定时器新的超时值仍然小于下一个定时器的超时，不用调整
否则先将定时器从链表取出，重新插入链表
*/
void sort_timer_lst::adjust_timer(util_timer *timer)
{
    if (!timer)
    {
        return;
    }
    util_timer *tmp = timer->next;

    //被调整的定时器在链表尾部
    //定时器超时值仍然小于下一个定时器超时值，不调整
    if (!tmp || (timer->expire < tmp->expire))
    {
        return;
    }

    //被调整定时器是链表头结点，将定时器取出，重新插入
    if (timer == head)
    {
        head = head->next;
        head->prev = NULL;
        timer->next = NULL;
        add_timer(timer, head);
    }
    //被调整定时器在内部，将定时器取出，重新插入
    else
    {
        timer->prev->next = timer->next;
        timer->next->prev = timer->prev;
        add_timer(timer, timer->next);
    }
}

//del_timer函数将超时的定时器从链表中删除(常规双向链表删除结点)
void sort_timer_lst::del_timer(util_timer *timer)
{
    if (!timer)
    {
        return;
    }
    //链表中只有一个定时器，需要删除该定时器
    if ((timer == head) && (timer == tail))
    {
        delete timer;
        head = NULL;
        tail = NULL;
        return;
    }
    //被删除的定时器为头结点
    if (timer == head)
    {
        head = head->next;
        head->prev = NULL;
        delete timer;
        return;
    }
    //被删除的定时器为尾结点
    if (timer == tail)
    {
        tail = tail->prev;
        tail->next = NULL;
        delete timer;
        return;
    }
    //被删除的定时器在链表内部，常规链表结点删除
    timer->prev->next = timer->next;
    timer->next->prev = timer->prev;
    delete timer;
}

//定时任务处理函数
/*
具体的逻辑如下，
遍历定时器升序链表容器，从头结点开始依次处理每个定时器，直到遇到尚未到期的定时器
若当前时间小于定时器超时时间，跳出循环，即未找到到期的定时器
若当前时间大于定时器超时时间，即找到了到期的定时器，执行回调函数，然后将它从链表中删除，然后继续遍历
*/
void sort_timer_lst::tick()
{
    if (!head)
    {
        return;
    }
    
    //获取当前时间
    time_t cur = time(NULL);
    util_timer *tmp = head;

    //遍历定时器链表
    while (tmp)
    {
        //链表容器为升序排列
        //当前时间小于定时器的超时时间，后面的定时器也没有到期
        if (cur < tmp->expire)
        {
            break;
        }

        //当前定时器到期，则调用回调函数，执行定时事件
        tmp->cb_func(tmp->user_data);

        //将处理后的定时器从链表容器中删除，并重置头结点
        head = tmp->next;
        if (head)
        {
            head->prev = NULL;
        }
        delete tmp;
        tmp = head;
    }
}

//私有成员，被公有成员add_timer和adjust_time调用
//主要用于调整链表内部结点
void sort_timer_lst::add_timer(util_timer *timer, util_timer *lst_head)
{
    util_timer *prev = lst_head;
    util_timer *tmp = prev->next;

    //遍历当前结点之后的链表，按照超时时间找到目标定时器对应的位置，常规双向链表插入操作
    while (tmp)
    {
        if (timer->expire < tmp->expire)
        {
            prev->next = timer;
            timer->next = tmp;
            tmp->prev = timer;
            timer->prev = prev;
            break;
        }
        prev = tmp;
        tmp = tmp->next;
    }
    //遍历完发现，目标定时器需要放到尾结点处
    if (!tmp)
    {
        prev->next = timer;
        timer->prev = prev;
        timer->next = NULL;
        tail = timer;
    }
}

void Utils::init(int timeslot)
{
    m_TIMESLOT = timeslot;
}

//对文件描述符设置非阻塞
int Utils::setnonblocking(int fd)
{
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

//将内核事件表注册读事件，ET模式，选择开启EPOLLONESHOT
void Utils::addfd(int epollfd, int fd, bool one_shot, int TRIGMode)
{
    epoll_event event;
    event.data.fd = fd;

    if (1 == TRIGMode)
        event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    else
        event.events = EPOLLIN | EPOLLRDHUP;

    if (one_shot)
        event.events |= EPOLLONESHOT;

    
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setnonblocking(fd);
}

//信号处理函数
//信号处理函数中仅仅通过管道发送信号值，不处理信号对应的逻辑，缩短异步执行时间，减少对主程序的影响。
void Utils::sig_handler(int sig)
{
    //为保证函数的可重入性，保留原来的errno
    //可重入性表示中断后再次进入该函数，环境变量与之前相同，不会丢失数据
    int save_errno = errno;
    int msg = sig;

    //将信号值从管道写端写入，传输字符类型，而非整型
    send(u_pipefd[1], (char *)&msg, 1, 0);

    //将原来的errno赋值为当前的errno
    errno = save_errno;
}

//设置信号函数
//项目中设置信号函数，仅关注SIGTERM和SIGALRM两个信号。
void Utils::addsig(int sig, void(handler)(int), bool restart)
{
    //创建sigaction结构体变量
    /*
    struct sigaction 
    {
        void (*sa_handler)(int);
        void (*sa_sigaction)(int, siginfo_t *, void *);
        sigset_t sa_mask;
        int sa_flags;
        void (*sa_restorer)(void);
    };

    sa_handler是一个函数指针，指向信号处理函数
    
    sa_sigaction同样是信号处理函数，有三个参数，可以获得关于信号更详细的信息
    
    sa_mask用来指定在信号处理函数执行期间需要被屏蔽的信号
    
    sa_flags用于指定信号处理的行为
        
        SA_RESTART，使被信号打断的系统调用自动重新发起
        SA_NOCLDSTOP，使父进程在它的子进程暂停或继续运行时不会收到 SIGCHLD 信号
        SA_NOCLDWAIT，使父进程在它的子进程退出时不会收到 SIGCHLD 信号，这时子进程如果退出也不会成为僵尸进程
        SA_NODEFER，使对信号的屏蔽无效，即在信号处理函数执行期间仍能发出这个信号
        SA_RESETHAND，信号处理之后重新设置为默认的处理方式
        SA_SIGINFO，使用 sa_sigaction 成员而不是 sa_handler 作为信号处理函数
    
    sa_restorer一般不使用
    */
    struct sigaction sa;
    
    memset(&sa, '\0', sizeof(sa));
    
    //信号处理函数中仅仅发送信号值，不做对应逻辑处理
    sa.sa_handler = handler;
    if (restart)
        sa.sa_flags |= SA_RESTART;

    //将所有信号添加到信号集中
    //用来将参数set信号集初始化，然后把所有的信号加入到此信号集里。
    sigfillset(&sa.sa_mask);

    //执行sigaction函数
    /*
    signum表示操作的信号。
    act表示对信号设置新的处理方式。
    oldact表示信号原来的处理方式。
    int sigaction(int signum, const struct sigaction *act, struct sigaction *oldact);
    */
    assert(sigaction(sig, &sa, NULL) != -1);
}

//定时处理任务，重新定时以不断触发SIGALRM信号
void Utils::timer_handler()
{
    m_timer_lst.tick();
    alarm(m_TIMESLOT);
}

void Utils::show_error(int connfd, const char *info)
{
    send(connfd, info, strlen(info), 0);
    close(connfd);
}

int *Utils::u_pipefd = 0;
int Utils::u_epollfd = 0;

class Utils;
//定时事件，具体的，从内核事件表删除事件，关闭文件描述符，释放连接资源。
void cb_func(client_data *user_data)
{
    //删除非活动连接在socket上的注册事件
    epoll_ctl(Utils::u_epollfd, EPOLL_CTL_DEL, user_data->sockfd, 0);
    assert(user_data);
    
    //关闭文件描述符
    close(user_data->sockfd);

    //减少连接数
    http_conn::m_user_count--;
}
