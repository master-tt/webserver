#include "pthreadpool/lcoker.h"
#include <stdio.h>
#include <stdlib.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <string.h>
#include <error.h>
#include "pthreadpool/threadpool.h"
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include "http/http_conn.h"
#include "timer/lst_timer.h"
#include <assert.h>

#define MAXFD 65535    // 支持的最大客户端数
#define MAX_EVENT_NUMBER 10000   // 监听最大数
#define TIMESLOT 5

static int pipefd[2];
static sort_timer_lst timer_lst;
static int epollfd = 0;

// 添加信号捕捉
void addsig(int sig, void(handle)(int)) {
    struct sigaction sa;
    memset(&sa, '\0', sizeof(sa));
    sa.sa_handler = handle;
    sigfillset(&sa.sa_mask);
    sigaction(sig, &sa, NULL);
}

void sig_handler(int sig) {
    int save_errno = errno;
    int msg = sig;
    send(pipefd[1], (char*)&msg, 1, 0);

}

void add_sig(int sig) {
    struct sigaction sa;
    memset( &sa, '\0', sizeof( sa ) );
    sa.sa_handler = sig_handler;
    sa.sa_flags |= SA_RESTART;
    sigfillset( &sa.sa_mask );
    assert( sigaction( sig, &sa, NULL ) != -1 );
}


// 定时器回调函数，它删除非活动连接socket上的注册事件，并关闭之。
void cb_func(http_conn* user_data)
{
    user_data->close_conn();
}

void time_handler() {
    // 定时处理任务，实际上就是调用tick()函数
    timer_lst.tick();
    // 因为一次alarm调用只会引起一次SIGALARM信号，所以我们要重新定时，以不断触发SIGALARM信号。
    alarm(TIMESLOT);
}

// 添加文件描述符到epoll中
extern void addfd(int epollfd, int fd, bool one_shot);

// 删除文件描述符到epoll中
extern void removefd(int epollfd, int fd);

extern void setnonblocking(int fd);

// 修改文件描述符(epoll)
extern void modfd(int epollfd, int fd, int ev);
int main(int argc, char* argv[])
{
    if (argc <= 1) {
        printf("usage: port\n");
        return 1;
    }

    // 获取端口号
    int port = atoi(argv[1]);

    // 对SIGPIE信号进行处理
    addsig(SIGPIPE, SIG_IGN);

    // 创建线程池,并初始化
    threadpool<http_conn>* pool = NULL;
    try {
        pool = new threadpool<http_conn>;
    }
    catch(...) {
        return 1;
    }

    // 创建数组保存所有客户端信息
    http_conn* users = new http_conn[MAXFD];

    // socket
    int listenfd = socket(AF_INET, SOCK_STREAM, 0);
    /*if (listenfd == -1) {
        perror("socket");
        exit(-1);
    }*/

    // 端口复用
    int reuse = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    // 绑定端口
    struct sockaddr_in saddr;
    bzero(&saddr, sizeof(saddr));
    saddr.sin_family = AF_INET;
    saddr.sin_port = htons(port);
    saddr.sin_addr.s_addr = INADDR_ANY;
    int ret = bind(listenfd, (struct sockaddr*)&saddr, sizeof(saddr));
    /*if (ret == -1) {
        perror("bind");
        exit(-1);
    }
*/
    // 监听
    ret = listen(listenfd, 8);
   /* if (ret == -1) {
        perror("listen");
        exit(-1);
    }*/

    // 创建epoll对象，添加事件数组
    epoll_event events[MAX_EVENT_NUMBER];
    int epollfd = epoll_create(5);

    // 将监听文件描述符添加到epoll中
    addfd(epollfd, listenfd, false);
    http_conn::m_epollfd = epollfd;

    ret = socketpair(PF_UNIX, SOCK_STREAM, 0, pipefd);
    assert(ret != -1);
    setnonblocking(pipefd[1]);
    // addfd(epollfd, pipefd[0], false);
    addfd(epollfd, pipefd[0], false);
    // 设置信号处理函数
    add_sig(SIGALRM);
    add_sig(SIGTERM);

    bool stop_server = false;

    bool timeout = false;
    alarm(TIMESLOT);                                    // 定时,5秒后产生SIGALARM信号

    while (!stop_server) {
        int num = epoll_wait(epollfd, events, MAX_EVENT_NUMBER, -1);
        if ((num < 0) && (errno != EINTR)) {
            printf("epoll failed\n");
            break;
        }

        // 遍历事件数组
        for (int i = 0; i < num; ++i) {
            int socketfd = events[i].data.fd;
            if (socketfd == listenfd) {
                // 有客户端连接
                
                struct sockaddr_in clientaddr;
                socklen_t len = sizeof(clientaddr);
                int connectfd = accept(listenfd, (struct sockaddr*)&clientaddr, &len);
                if (connectfd < 0) {
                    printf("errno is %d\n", errno);
                    continue;
                }

                if (http_conn::m_user_count >= MAXFD) {
                    // 目前连接的数已满
                    close(connectfd);
                    continue;
                }
                users[connectfd].init(connectfd, clientaddr);
                util_timer* timer = new util_timer;
                timer->user_data = &users[connectfd];
                timer->cb_func = cb_func;
                time_t cur = time(NULL);
                timer->expire = cur + 3 * TIMESLOT;
                users[connectfd].timer = timer;
                // users[connectfd].user_data->timer = timer;
                timer_lst.add_timer(timer); 
                
            }
            else if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {         
                // 错误，关闭连接
                users[socketfd].close_conn();
                if (users[socketfd].timer) {
                        timer_lst.del_timer(users[socketfd].timer);
                    }
            }
            else if (socketfd == pipefd[0] && events[i].events & EPOLLIN) {
                // 处理信号
                printf("asdasdasdasdasdasdassssssssssssssssssssssssssd\n\n\n\n\n");
                int sig;
                char signals[1024];
                ret = recv(pipefd[0], signals, sizeof(signals), 0);
                if (ret == -1) {
                    continue;
                }
                else if (ret == 0) {
                    continue;
                }
                else {
                    
                    for (int i = 0; i < ret; ++i) {
                        switch( signals[i] )  {
                            case SIGALRM:
                            {
                                // 用timeout变量标记有定时任务需要处理，但不立即处理定时任务
                                // 这是因为定时任务的优先级不是很高，我们优先处理其他更重要的任务。
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
            else if (events[i].events & EPOLLIN) {              // 有数据到来
                    printf("213213123\n");
                if (users[socketfd].read()) {                   // 读完成，提交给pool
                    pool->append(users + socketfd);
                        time_t cur = time(NULL);
                        users[socketfd].timer->expire = cur + 3 * TIMESLOT;
                        timer_lst.adjust_timer(users[socketfd].timer);
                }
                else {
                    users[socketfd].close_conn();
                    if (users[socketfd].timer) {
                        timer_lst.del_timer(users[socketfd].timer);
                    }
                }
                // memset(users[socketfd].m_read_buf, '\0', BUFFER_SIZE);
                /*ret = users[socketfd].read();
                // util_timer* timer = users[socketfd].timer;
                if (ret < 0) {
                    // 如果发生读错误，则关闭连接，并移除其对应的定时器
                    if (errno == EAGAIN) {
                        users[socketfd].close_conn();
                        if (users[socketfd].timer) {
                            timer_lst.del_timer(users[socketfd].timer);
                        }
                    }
                }
                else if (ret == 0) {
                    // 如果对方已经关闭连接，则我们也关闭连接，并移除对应的定时器
                    users[socketfd].close_conn();
                    if (users[socketfd].timer) {
                        timer_lst.del_timer(users[socketfd].timer);
                    }
                }
                else {
                    time_t cur = time(NULL);
                    users[socketfd].timer->expire = cur + 3 * TIMESLOT;
                    timer_lst.adjust_timer(users[socketfd].timer);
                }*/
            }
            else if (events[i].events & EPOLLOUT) {             // 线程池中工作线程注册写，将数据写到socket，发送到客户端
                util_timer* timer = users[socketfd].timer;
                printf("wwwwwwwwwwwwwwwwwww\n");
                if (!users[socketfd].write()) {                 // 写失败     
                    users[socketfd].close_conn();
                    if (timer) {
                        timer_lst.del_timer(timer);
                    }
                }

                    time_t cur = time(NULL);
                    timer->expire = cur + 3 * TIMESLOT;
                    timer_lst.adjust_timer(timer);
           
            }
        }
        if( timeout ) {
            printf("sssddasda122222222222222\n");
            time_handler();
            timeout = false;
        }     
    }

    close( listenfd );
    close( pipefd[1] );
    close( pipefd[0] );
    close(epollfd);
    delete[] users;
    delete pool;

    return 0;
}




