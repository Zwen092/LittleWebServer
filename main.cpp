#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <cassert>
#include <unistd.h>

#include "./http/http_conn.h"
#include "./timer/lst_timer.h"

#define MAX_EVENT_NUMBER 10000
#define MAX_FD 65536
#define TIMESLOT 5 //minimum timeout unit

#define SYNLOG //sync write log
#define listenfdLT

//those three functions defined in http_conn.cpp
extern int addfd(int epollfd, int fd, bool one_shot);
extern int remove(int epollfd, int fd);
extern int setnonblocking(int fd);

//timer related params
static int pipefd[2];
//创建定时器容器链表
static sort_timer_lst timer_lst;

static int epollfd = 0;

//sig_handler
void sig_handler(int sig)
{
    //to keep reentrancty, keep the previous error
    int save_error = errno;
    int msg = sig;
    send(pipefd[1], (char *)&msg, 1, 0);
    errno = save_error;
}

void addsig(int sig, void(handler)(int), bool restart = true)
{
    struct sigaction sa;
    memset(&sa, '\0', sizeof(sa));
    sa.sa_handler = handler;
    if (restart)
    {
        sa.sa_flags |= SA_RESTART;
    }
    sigfillset(&sa.sa_mask);
    assert(sigaction(sig, &sa, NULL) != -1);
}

//定时处理任务，重新定时以不断触发SIGALRM信号
void timer_handler()
{
    timer_lst.tick();
    alarm(TIMESLOT);
}

//timer callback function, delete inactive resistered event connected to the socket and shut it down
void cb_func(client_data *user_data)
{
    //删除非活动连接在socket上的注册事件//删除非活动连接在socket上的注册事件
    epoll_ctl(epollfd, EPOLL_CTL_DEL, user_data->sockfd, 0);
    assert(user_data);

    close(user_data->sockfd);

    //reduce the connection num
    http_conn::m_user_count--;
}

void show_error(int connfd, const cahr *info)
{
    printf("%s", info);
    send(connfd, info, strlen(info), 0);
    close(connfd);
}

int main(int argc, char const *argv[])
{
#ifdef ASYNLOG
    Log::get_instance()->init("ServerLog", 2000, 800000, 8);
#endif

#ifdef SYNLOG
    Log::get_instance()->init("ServerLog", 2000, 800000, 0);
#endif

    if (argc <= 1)
    {
        printf("usage: %s ip_address port_number\n", basename(argv[0]));
        return 1;
    }
    int port = atoi(argv[1]);

    addsig(SIGPIPE, SIG_IGN);

    //create database connectionPool
    connection_pool *connPool = connection_pool::GetInstance();
    connPool->init("localhost", "root", "54433221Qwe", "common_data", 3306, 8);

    //创建线程池
    threadpool<http_conn> *pool = NULL;
    try
    {
        pool = new threadpool<http_conn>(connPool);
    }
    catch (...)
    {
        return 1;
    }

    //create max_fd http objects
    http_conn *users = new http_conn[MAX_FD];

    //pfinet mean?
    int listendfd = socket(PF_INET, SOCK_STREAM, 0);

    int ret = 0;
    struct sockaddr_in address;
    bzero(&address, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port);

    int flag = 1;
    /* so_reuseaddr allow port to be reused */
    setsockopt(listendfd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag));
    /* bind socket and its address */
    ret = bind(listendfd, (struct sockaddr *)&address, sizeof(address));
    assert(ret >= 0);
    ret = listen(listendfd, 5);
    assert(ret >= 0);

    //create an additional fd to illustrate the event table of the kernel
    epollfd = epoll_create(5);
    // event array to store poll table
    epoll_event events[MAX_EVENT_NUMBER];

    assert(ret >= 0);
    /* Main thread registrate listen socket, when new client arrives, listenfd become ready */
    addfd(epollfd, listendfd, false);
    //what is this?
    http_conn::m_epollfd = epollfd;

    //create pipe, don't know what is means
    ret = socketpair(PF_UNIX, SOCK_STREAM, 0, pipefd);
    assert(ret != -1);

    //设置管道写端为非阻塞，为什么写端要非阻塞？
    //send是将信息发送给套接字缓冲区，如果缓冲区满了，则会阻塞，
    //这时候会进一步增加信号处理函数的执行时间，为此，将其修改为非阻塞。
    setnonblocking(pipefd[1]);

    addfd(epollfd, pipefd[0], false);

    addsig(SIGALRM, sig_handler, false);
    addsig(SIGTERM, sig_handler, false);

    //循环条件
    bool stop_server = false;

    client_data *users_timer = new client_data[MAX_FD];

    //超时标志
    bool timeout = false;
    alarm(TIMESLOT);

    while (!stop_server)
    {
        //监测发生事件的文件描述符
        int number = epoll_wait(epollfd, events, MAX_EVENT_NUMBER, -1);
        if (number < 0 && errno != EINTR)
        {
            break;
        }

        /* iterate through this */
        for (int i = 0; i < number; i++)
        {
            int sockfd = events[i].data.fd;
            if (sockfd == listendfd)
            {
                //初始化客户端连接地址
                struct sockaddr_in client_address;
                socklen_t client_addrlength = sizeof(client_address);

                //该连接分配的文件描述符
                int connfd = accept(listenfd, (struct sockaddr *)&client_address, &client_addrlength);

                //初始化该连接对应的连接资源
                users_timer[connfd].address = client_address;
                users_timer[connfd].sockfd = connfd;

                //创建定时器临时变量
                util_timer *timer = new util_timer;

                //设置定时器对应的连接资源
                timer->user_data = &users_timer[connfd];
                //set call back function
                timer->cb_func = cb_func;

                time_t cur = time(NULL);

                //set absolute expire time
                timer->expire = cur + 3 * TIMESLOT;
                //创建该连接对应的定时器，初始化为前述临时变量
                users_timer[connfd].timer = timer;
                //add this timer to list
                timer_lst.add_timer(timer);
            }
            //handle exception events

            else if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR))
            {
                //服务器端关闭连接，移除对应的定时器
                cb_func(&users_timer[sockfd]);

                util_timer *timer = users_timer[sockfd].timer;
                if (timer)
                {
                    timer_lst.del_timer(timer);
                }
            }

            //处理定时器信号
            else if ((sockfd == pipefd[0]) && (events[i].events & EPOLLIN))
            {
                //接收到SIGALRM信号，timeout设置为True
            }

            //处理客户连接上接收到的数据
            else if (events[i].events & EPOLLIN)
            {
                //创建定时器临时变量，将该连接对应的定时器取出来
                util_timer *timer = users_timer[sockfd].timer;
                if (users[sockfd].read_once())
                {
                    //若监测到读事件，将该事件放入请求队列
                    pool->append(users + sockfd);

                    //若有数据传输，则将定时器往后延迟3个单位
                    //对其在链表上的位置进行调整
                    if (timer)
                    {
                        timer_t cur = time(NULL);
                        timer->expire = cur + 3 * TIMESLOT;
                        timer_lst.adjust_timer(timer);
                    }
                }
                else
                {
                    //if not
                    //服务器端关闭连接，移除对应的定时器
                    cb_func(&users_timer[sockfd]);
                    if (timer)
                    {
                        timer_lst.del_timer(timer);
                    }
                }
            }
            //write event happens
            else if (events[i].events & EPOLLOUT)
            {
                util_timer *timer = users_timer[sockfd].timer;
                if (users[sockfd].write())
                {
                    //若有数据传输，则将定时器往后延迟3个单位
                    //对其在链表上的位置进行调整
                    if (timer)
                    {
                        time_t cur = time(NULL);
                        timer->expire = cur + 3 * TIMESLOT;
                        timer_lst.adjust_timer(timer);
                    }
                }
                else
                {
                    //服务器端关闭连接，移除对应的定时器
                    cb_func(&users_timer[sockfd]);
                    if (timer)
                    {
                        time_t cur = time(NULL);
                        timer->expire = cur + 3 * TIMESLOT;
                        timer_lst.adjust_timer(timer);
                    }
                }
            }
            else if ((sockfd = pipefd[0]) && (events[i].events & EPOLLIN))
            {
                int sig;
                char signals[1024];

                //从管道读端读出信号值，成功返回字节数，失败返回-1
                //正常情况下，这里的ret返回值总是1，只有14和15两个ASCII码对应的字符
                ret = recv(pipefd[0], signals, sizeof(signals), 0);
                if (ret == -1)
                {
                    //handle the error
                    continue;
                }
                else if (ret == 0)
                {
                    continue;
                }
                else
                {
                    //处理信号值对应的逻辑
                    for (int i = 0; i < ret; ++i)
                    {
                        switch (signals[i])
                        {
                        case SIGALRM:
                        {
                            timeout = true;
                            break;
                        }
                        case SIGTERM:
                        {
                            stop_server = true;
                        }
                        }
                    }
                }
            }
        }
        //处理定时器为非必须事件，收到信号并不是立马处理
        //完成读写事件后，再进行处理
        if (timeout)
        {
            timer_handler();
            timeout = false;
        }
    }
}
