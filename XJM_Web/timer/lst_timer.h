#ifndef LST_TIMER
#define LST_TIMER

#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <sys/stat.h>
#include <string.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <stdarg.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/uio.h>

#include <time.h>
#include"../log/NanoLog.hpp"

class util_timer;

struct client_data
{
    //项目中将连接资源、定时事件和超时时间封装为定时器类
    //连接资源包括客户端套接字地址、文件描述符和定时器
    sockaddr_in address;
    int sockfd;
    util_timer *timer;
};

//定时器类
class util_timer
{
public:
    util_timer() : prev(NULL), next(NULL) {}

public:
    //定时器超时时间 = 浏览器和服务器连接时刻 + 固定时间(TIMESLOT)，
    //可以看出，定时器使用绝对时间作为超时值，这里alarm设置为5秒，连接超时为15秒。
    time_t expire;
    
    //定时事件为回调函数，将其封装起来由用户自定义，这里是删除非活动socket上的注册事件，并关闭
    void (* cb_func)(client_data *);
    client_data *user_data;//连接资源
    util_timer *prev;//前向定时器
    util_timer *next;//后继定时器
};

/*
项目中的定时器容器为带头尾结点的升序双向链表，具体的为每个连接创建一个定时器，
将其添加到链表中，并按照超时时间升序排列。执行定时任务时，将到期的定时器从链表中删除。
*/
class sort_timer_lst
{
public:
    sort_timer_lst();
    ~sort_timer_lst();

    void add_timer(util_timer *timer);
    void adjust_timer(util_timer *timer);
    void del_timer(util_timer *timer);
    void tick();

private:
    void add_timer(util_timer *timer, util_timer *lst_head);

    util_timer *head;
    util_timer *tail;
};

class Utils
{
public:
    Utils() {}
    ~Utils() {}

    void init(int timeslot);

    //对文件描述符设置非阻塞
    int setnonblocking(int fd);

    //将内核事件表注册读事件，ET模式，选择开启EPOLLONESHOT
    void addfd(int epollfd, int fd, bool one_shot, int TRIGMode);

    //信号处理函数
    static void sig_handler(int sig);

    //设置信号函数
    void addsig(int sig, void(handler)(int), bool restart = true);

    //定时处理任务，重新定时以不断触发SIGALRM信号
    void timer_handler();

    void show_error(int connfd, const char *info);

public:
    static int *u_pipefd;
    sort_timer_lst m_timer_lst;
    static int u_epollfd;
    int m_TIMESLOT;
};

void cb_func(client_data *user_data);

#endif
