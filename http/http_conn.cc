#include "http_conn.h"
#include "../timer/lst_timer.h"
// 网站的根目录
const char* doc_root = "/home/master/Desktop/WebServer/resources";
// 定义HTTP响应的一些状态信息
const char* ok_200_title = "OK";
const char* error_400_title = "Bad Request";
const char* error_400_form = "Your request has bad syntax or is inherently impossible to satisfy.\n";
const char* error_403_title = "Forbidden";
const char* error_403_form = "You do not have permission to get file from this server.\n";
const char* error_404_title = "Not Found";
const char* error_404_form = "The requested file was not found on this server.\n";
const char* error_500_title = "Internal Error";
const char* error_500_form = "There was an unusual problem serving the requested file.\n";

int http_conn::m_epollfd = -1;      // 所有的socket上的事件都被注册到同意epollfd上
int http_conn::m_user_count = 0;   // 统计已连接用户的数量
static sort_timer_lst timer_lst;

void setnonblocking(int fd) {
    int old_flag = fcntl(fd, F_GETFL);
    int new_flag = old_flag | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_flag);
}

// 修改文件描述符到epoll中
void addfd(int epollfd, int fd, bool one_shot) {
    epoll_event event;
    event.data.fd = fd;
    event.events = EPOLLIN | EPOLLRDHUP;  // EPOLLRDHUP，当连接断开时，触发挂起，断开异常时

    if (one_shot) {                       // EPOLLONESHOT确保将缓冲区数据一次全部读取完成
        event.events |= EPOLLONESHOT;
    }
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    // 设置文件描述符为非阻塞
    setnonblocking(fd);
}

// 删除文件描述符到epoll中
void removefd(int epollfd, int fd) {
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
    close(fd);
}

// 修改文件描述符(epoll)，重置文件描述符EPOLLONESHOT，当下一次读可用时，确保EPOLLIN事件被触发
void modfd(int epollfd, int fd, int ev) {
    epoll_event event;
    event.data.fd = fd;
    event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}

void http_conn::init(int socketfd, sockaddr_in& addr) {
    m_socketfd = socketfd;
    m_saddr = addr;
    // 端口复用

    int reuse = 1;
    setsockopt(m_socketfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    // 添加epoll对象中
    addfd(m_epollfd, m_socketfd, true);
    ++m_user_count;       
    //util_timer* timer = new util_timer;
    init();
}

void http_conn::init() {
    m_check_state = CHECK_STATE_REQUESTLINE;
    m_linger = false;

    m_method = GET;         // 默认请求方式为GET
    m_url = 0;              
    m_version = 0;
    m_content_length = 0;
    m_host = 0;
    m_start_line = 0;
    m_checked_index = 0;
    m_read_idx = 0;
    m_write_idx = 0;

    bzero(m_read_buf, READ_BUFFER_SIZE);
    bzero(m_write_buf, READ_BUFFER_SIZE);
    bzero(m_read_file, FILENAME_LEN);
}

// 关闭连接
void http_conn::close_conn() {
    if (m_socketfd != -1) {
        removefd(m_epollfd, m_socketfd);
        m_socketfd = -1;
        --m_user_count;
    }
}

// 循环读取缓冲区，一次性读完
bool http_conn::read() {
    if (m_read_idx >= READ_BUFFER_SIZE) {
        return false;
    }

    int bytes_read = 0;
    while (true) {
        bytes_read = recv(m_socketfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
        if (bytes_read == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // 没有数据了
                break;
            }
            return false;
        } else if (bytes_read == 0) {
            // 对方已经关闭
            return false;
        }
        m_read_idx += bytes_read;
    }
    printf("读取到了数据:\n %s", m_read_buf);
    return true;
}

// 解析http请求
http_conn::HTTP_CODE http_conn::process_read() {
    LINE_STATUS line_status = LINE_OK;
    HTTP_CODE ret = NO_REQUEST;
    char* text = 0;

    // 当前正在解析请求体，并且请求行、头解析完成，不再需要一行行获取 ||
    // 解析到了一行完整的数据
    while (((m_check_state == CHECK_STATE_CONTENT) && (line_status == LINE_OK)) ||
            (line_status = parse_line()) == LINE_OK) {
        text = get_line();
        m_start_line = m_checked_index;
        printf("get 1 http line: %s\n", text);

        switch (m_check_state) {printf("11111111111111111111111111111111111111111111\n");
            case CHECK_STATE_REQUESTLINE:
            {
                ret = parse_request_line(text);
                if (ret == BAD_REQUEST) {
                    return BAD_REQUEST;
                }
                    
                break;
            }
            case CHECK_STATE_HEADER:
            {
                ret = parse_headers(text);
                if (ret == BAD_REQUEST) {
                    return BAD_REQUEST;
                }
                else if (ret == GET_REQUEST) {
                    return do_request();        // 解析具体信息
                }
                break;
            }
            case CHECK_STATE_CONTENT:
            {
                ret = parse_content(text);
                if (ret == GET_REQUEST) {
                    return do_request();
                }
                line_status = LINE_OPEN;
                break;
            }
            default:
            {
                return INTERNAL_ERROR;
            }
        } 
    }
    return NO_REQUEST;
}

// 解析一行
http_conn::LINE_STATUS http_conn::parse_line() {
    char temp;
    for (;m_checked_index < m_read_idx; ++m_checked_index) {
        temp = m_read_buf[m_checked_index];
        if (temp == '\r') {
            // GET / HTTP/1.1\r\n --> GET / HTTP/1.1\0\0
            if ((m_checked_index + 1) == m_read_idx) {
                return LINE_OPEN;
            }
            else if (m_read_buf[m_checked_index + 1] == '\n') {
                m_read_buf[m_checked_index++] = '\0';
                m_read_buf[m_checked_index++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
        else if (temp == '\n') {
            if ((m_checked_index > 1) && (m_read_buf[m_checked_index - 1] == '\r')) {
                m_read_buf[m_checked_index - 1] = '\0';
                m_read_buf[m_checked_index++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
    }
    return LINE_OPEN;
}

// 解析请求行,获得请求方法、目标URL和HTTP版本号
http_conn::HTTP_CODE http_conn::parse_request_line(char* text) {
    // GET /index.html HTTP/1.1
    m_url = strpbrk(text, " \t");
    if (!m_url) {
        return BAD_REQUEST;
    }
    *m_url++ = '\0';                            // m_url = /index.html HTTP/1.1

    char* method = text;                        // GET\0/index.html HTTP/1.1
    if (strcasecmp(method, "GET") == 0) {
        m_method = GET;
    }
    else {
        return BAD_REQUEST;
    }
    m_version = strpbrk(m_url, " \t");          
    if (!m_version) {
        return BAD_REQUEST;
    }
    *m_version++ = '\0';                        // m_url = /index.html\0HTTP/1.1
                                                // m_version = HTTP/1.1
    // m_version += strspn(m_version, " \t");
    if (strcasecmp(m_version, "HTTP/1.1") != 0) {
        return BAD_REQUEST;
    }
    // 另一种格式 HTTP://192.168.1.1:9999/index.html
    if (strncasecmp(m_url, "http://", 7) == 0) {
        m_url += 7;                     // 192.168.1.1:9999/index.html
        m_url = strchr(m_url, '/');     // /index.html
    }

    if (!m_url || m_url[0] != '/') {
        return BAD_REQUEST;
    }

    m_check_state = CHECK_STATE_HEADER;         // 主状态机状态变为检查请求头
    return NO_REQUEST;                          // 仍需要解析
} 

// 解析请求头
http_conn::HTTP_CODE http_conn::parse_headers(char* text) {
    // m_url = strpbrk(text, " \t");
    if (text[0] == '\0') {
        if (m_content_length != 0) {
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
        return GET_REQUEST;
    }
    // Connection: keep-alive
    else if (strncasecmp(text, "Connection:", 11) == 0) {
        text += 11;
        text += strspn(text, " \t");                // keep-alive
        if (strcasecmp(text, "keep-alive") == 0) {
            m_linger = true;
        }
    }
    else if (strncasecmp(text, "Connection-Length:", 15) == 0) {
        text += 15;
        text += strspn(text, " \t");
        m_content_length = atol(text);
    }
    else if (strncasecmp(text, "Host:", 5) == 0) {
        text += 5;
        text += strspn(text, " \t");
        m_host = text;
    }
    else {
        printf("oop! unknow header %s\n", text);
    }
    return NO_REQUEST;
}

// 解析请求体
http_conn::HTTP_CODE http_conn::parse_content(char* text) {
    if (m_read_idx >= (m_content_length + m_checked_index)) {
        text[m_content_length] = '\0';
        return GET_REQUEST;
    }
    return NO_REQUEST;
}

// 当得到一个完整的HTTP请求时，分析目标文件的属性。如果目标文件存在，对所有的用户可读，且不是目录，则使用mmap将其映射到内存地址m_file_address处，
// 并告诉调用者获取文件成功
http_conn::HTTP_CODE http_conn::do_request() {
    strcpy(m_read_file, doc_root);
    int len = strlen(doc_root);
    strncpy(m_read_file + len, m_url, FILENAME_LEN - len - 1);          // doc_root/m_url

    if (stat(m_read_file, &m_file_stat) < 0) {
        return NO_RESOURCE;
    }
    if (!(m_file_stat.st_mode & S_IROTH)) {
        return FORBIDDEN_REQUEST;
    }
    if (S_ISDIR(m_file_stat.st_mode)) {
        return BAD_REQUEST;
    }

    int fd = open(m_read_file, O_RDONLY);
    m_file_address = (char*)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);

    return FILE_REQUEST;    
}

// 一次性写缓冲区，写完
bool http_conn::write() {
    int temp;
    int byte_have_send = 0;
    int bytes_to_send = m_write_idx;
    if (bytes_to_send == 0) {
        modfd(m_epollfd, m_socketfd, EPOLLIN);
        init();
        return true;
    }
    
    while (true) {
        temp = writev(m_socketfd, m_iv, m_iv_count);
        if (temp <= -1) {
            if (errno == EAGAIN) {
                modfd(m_epollfd, m_socketfd, EPOLLOUT);
                return true;
            }
            unmmap();
            return false;
        }

        bytes_to_send -= temp;
        byte_have_send += temp;
        if (bytes_to_send <= byte_have_send) {
            unmmap();
            if (m_linger) {
                init();
                modfd(m_epollfd, m_socketfd, EPOLLIN);
                return true;
            }
            else {
                modfd(m_epollfd, m_socketfd, EPOLLIN);
                return false;
            }
        }
    }
}

bool http_conn::add_response(const char* format, ...) {
    if (m_write_idx >= WRITE_BUFFER_SIZE) {
        return false;
    }
    va_list arg_list;
    va_start(arg_list, format);
    int len = vsnprintf(m_write_buf + m_write_idx, WRITE_BUFFER_SIZE - 1 - m_write_idx, format, arg_list);
    if (len >= WRITE_BUFFER_SIZE - 1 - m_write_idx) {
        return false;
    }
    m_write_idx += len;
    va_end(arg_list);
    return true;
}

bool http_conn::add_content(const char* content) {
    return add_response("%s", content);
}

bool http_conn::add_content_type() {
    return add_response("Content-Type:%s\r\n", "text/html");
}

bool http_conn::add_status_line(int status, const char* title) {
    return add_response("%s %d %s\r\n", "HTTP/1.1", status, title);
}

bool http_conn::add_headers(int content_length) {
    add_content_length(content_length);
    add_content_type();
    add_linger();
    add_blank_line();
}


bool http_conn::add_content_length(int content_length) {
    return add_response("Content-Length: %d\r\n", content_length);
}

bool http_conn::add_linger() {  
    return add_response("Connection: %s\r\n", (m_linger == true) ? "keep-alive" : "close");
}

bool http_conn::add_blank_line() {
    return add_response("%s", "\r\n");
}

// 根据服务器处理HTTP请求的结果，返回客户想要的内容
bool http_conn::process_write(HTTP_CODE ret) {
    switch(ret) {
        case INTERNAL_ERROR:
        {
            add_status_line(500, error_500_title);
            add_headers(strlen(error_500_form));
            if (!add_content(error_500_form)) {
                return false;
            }
            break;
        }
        case BAD_REQUEST:
        {
            add_status_line(400, error_400_title);
            add_headers(strlen(error_403_form));
            if (!add_content(error_400_form)) {
                return false;
            }
            break;
        }
        case NO_RESOURCE:
        {
            add_status_line(404, error_404_title);
            add_headers(strlen(error_404_form));
            if (!add_content(error_404_form)) {
                return false;
            }
            break;
        }
        case FORBIDDEN_REQUEST:
        {
            add_status_line( 403, error_403_title );
            add_headers(strlen( error_403_form));
            if (!add_content( error_403_form)) {
                return false;
            }
            break;
        }
        case FILE_REQUEST:
        {
            // add_status_line(200, ok_200_title);
            // if (m_file_stat.st_size != 0) {
            //     add_headers(strlen(ok_200_title));
            //     m_iv[0].iov_base = m_write_buf;
            //     m_iv[0].iov_len = m_write_idx;
            //     m_iv[1].iov_base = m_file_address;
            //     m_iv[1].iov_len = m_file_stat.st_size;
            //     return true;
            // }
            add_status_line(200, ok_200_title);
            add_headers(m_file_stat.st_size);
            m_iv[ 0 ].iov_base = m_write_buf;
            m_iv[ 0 ].iov_len = m_write_idx;
            m_iv[ 1 ].iov_base = m_file_address;
            m_iv[ 1 ].iov_len = m_file_stat.st_size;
            m_iv_count = 2;
            return true;
        }
        default:
        {
            return false;
        }
    }
    m_iv[0].iov_base = m_write_buf;
    m_iv[0].iov_len = m_write_idx;
    m_iv_count = 1;
    return true;
}

// 由线程池中的工作线程调用
void http_conn::process() {
    printf("pares request, create response\n");
    // 解析http请求
    HTTP_CODE read_ret  = process_read();    
    if (read_ret == NO_REQUEST) {
        // 请求不完整，再次读取
        modfd(m_epollfd, m_socketfd, EPOLLIN);
        return ;
    }

    bool write_ret = process_write(read_ret);
    if (!write_ret) {
        close_conn();
    }
    modfd(m_epollfd, m_socketfd, EPOLLOUT);
    printf("213213123\n");
}

void http_conn::unmmap() {
    if (m_file_address) {
        munmap(m_file_address, m_file_stat.st_size);
        m_file_address = 0;
    }
}