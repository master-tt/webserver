#ifndef LST_TIMER_H
#define LST_TIMER_H

#include <stdio.h>
#include <time.h>
#include <arpa/inet.h>

#define BUFFER_SIZE 64
class util_timer;

struct client_data
{
    sockaddr_in address;
    int socketfd;
    char buf[BUFFER_SIZE];
    util_timer* timer;
};

class util_timer {
public:
    util_timer() {}
    ~util_timer() {}

public:
    time_t expire;                      // 任务超时的时间，绝对时间
    void (*cb_func)(client_data*);      // 任务回调函数，回调函数处理的客户数据，由定时器的执行者传递给回调函数
    client_data* user_data;             // 客户的信息
    util_timer* prev;                   // 指向前一个定时器
    util_timer* next;                   // 指向下一个定时器

};

// 定时器链表，升序(任务超时的时间,expire)、双向链表，带有头结点、尾节点
class sort_timer_lst{
public:
    sort_timer_lst(): head(NULL), tail(NULL) {}
    ~sort_timer_lst() {
        util_timer* temp = head;
        while (temp) {
            head = temp->next;
            delete temp;
            temp = head;
        }
    }

    // 将定时器添加到链表中
    void add_timer(util_timer* timer) {
        if (!timer) {
            return;
        }
        if (!head) {
            head = tail = timer;
            return;
        }

        // 如果目标定时器的超时时间小于当前链表中所有定时器的超时时间，则把该定时器插入链表头部,作为链表新的头节点，
        // 否则就需要调用重载函数 add_timer(),把它插入链表中合适的位置，以保证链表的升序特性
        if (timer->expire < head->expire) {
            timer->next = head;
            head->prev = timer;
            head = timer;
            return ;
        }
        add_timer(timer, head);
    }


    // 当某个定时任务发生变化时，调整对应的定时器在链表中的位置。这个函数只考虑被调整的定时器的
    // 超时时间延长的情况，即该定时器需要往链表的尾部移动
    void adjust_timer(util_timer* timer) {
        if (!head)
            return;
        
        util_timer* temp = timer->next;
        // 如果被调整的目标定时器已经在链表的尾部，或者定时器新的超时时间仍小于其下一个定时器的超时时间则不用调整
        if (!temp || (timer->expire < temp->expire)) {
            return;
        }

        // 需要调整
        // 如果定时器是链表的头结点，则将定时器从链表中取出并重新插入链表
        if (timer == head) {
            head = head->next;
            timer->next = NULL;
            timer->prev = NULL;
            add_timer(timer, timer->next);
        }
        else {
            // 如果定时器是链表的中间节点，则将其取出并重新插入链表
            timer->prev->next = timer->next;
            timer->next->prev = timer->prev;
            add_timer(timer, timer->next);
        }
    }

    // 将定时器从链表中删除
    void del_timer(util_timer* timer) {
        if (!timer)
            return;


        // 如果链表中只有一个定时器，即需要删除的定时器
        if (head == timer && tail == timer) {
            delete timer;
            head = NULL;
            tail = NULL;
            return;
        }

        // 链表中有多个定时器，且目标定时器为头结点
        if (head == timer && tail != timer) {
            delete timer;
            head = head->next;
            head->prev = NULL;
            return;
        }

        // 链表中有多个定时器，且目标定时器为尾结点
        if (head != timer && tail == timer) {
            delete timer;
            tail = tail->prev;
            tail->next = NULL;
            return;
        }

        // 链表中有多个定时器，且目标定时器为中间节点
        timer->prev->next = timer->next;
        timer->next->prev = timer->prev;
        delete timer;
    }

    // SIGALARM 信号每次被触发就在其信号处理函数中执行一次tick()函数，以处理链表上到期任务
    void tick() {
        if (!head) {
            return;
        }
        printf("timer tick\n");
        time_t cur = time(NULL);      // 获取当前系统时间
        util_timer* temp = head;

        // 从头节点开始依次处理每个定时器，直到遇到一个尚未到期的定时器结束
        while (temp) {
            if (cur < temp->expire) {
                break;
            }
            // 此节点已经超时，调用定时器的回调函数，以执行定时任务
            temp->cb_func(temp->user_data);
            // 执行完定时器中的定时任务之后，就将它从链表中删除，并重置链表头节点
            head = temp->next;
            if (head) {
                head->prev = NULL;
            }
            delete temp;
            temp = head;
        }
    }

private:
    // 一个重载的辅助函数，它被公有的 add_timer 函数和 adjust_timer 函数调用
    // 该函数表示将目标定时器 timer 添加到节点 lst_head 之后的部分链表中
    void add_timer(util_timer* timer, util_timer* lst_head) {
        util_timer* prev =lst_head;
        util_timer* temp = prev->next;

        // 遍历 list_head 节点之后的部分链表，直到找到一个超时时间大于目标定时器的超时时间节点
        // 并将目标定时器插入该节点之前
        while (temp) {
            // 找到了一个超时时间大于目标定时器的超时时间节点
            if (timer->expire < temp->expire) {
                prev->next = timer;
                timer->next = temp;
                temp->prev = timer;
                timer->prev = prev;
                break;
            }
            prev = temp;
            temp = temp->next;
        }
        // 如果遍历完 lst_head 节点之后的部分链表，仍未找到超时时间大于目标定时器的超时时间的节点，
        // 则将目标定时器插入链表尾部，并把它设置为链表新的尾节点
        if (!temp) {
            prev->next = timer;
            timer->prev = prev;
            timer->next = NULL;
            tail = timer;
        }
    }

private:
    util_timer* head;   // 头结点
    util_timer* tail;   // 尾结点
};


#endif
