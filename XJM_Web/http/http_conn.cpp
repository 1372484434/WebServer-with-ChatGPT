#include "http_conn.h"

#include <mysql/mysql.h>
#include <fstream>

//定义http响应的一些状态信息
const char *ok_200_title = "OK";
const char *error_400_title = "Bad Request";
const char *error_400_form = "Your request has bad syntax or is inherently impossible to staisfy.\n";
const char *error_403_title = "Forbidden";
const char *error_403_form = "You do not have permission to get file form this server.\n";
const char *error_404_title = "Not Found";
const char *error_404_form = "The requested file was not found on this server.\n";
const char *error_500_title = "Internal Error";
const char *error_500_form = "There was an unusual problem serving the request file.\n";

locker m_lock;
//用户名和密码
map<string, string> users;

/*
载入数据库表
将数据库中的用户名和密码载入到服务器的map中来，map中的key为用户名，value为密码。
*/
void http_conn::initmysql_result(connection_pool *connPool)
{
    //先从连接池中取一个连接
    MYSQL *mysql = NULL;
    connectionRAII mysqlcon(&mysql, connPool);

    //在user表中检索username，passwd数据，浏览器端输入
    if (mysql_query(mysql, "SELECT username,passwd FROM user"))
    {
        LOG_INFO<<"SELECT error:%s\n"<<mysql_error(mysql);
    }

    //从表中检索完整的结果集
    MYSQL_RES *result = mysql_store_result(mysql);

    //返回结果集中的列数
    int num_fields = mysql_num_fields(result);

    //返回所有字段结构的数组
    MYSQL_FIELD *fields = mysql_fetch_fields(result);

    //从结果集中获取下一行，将对应的用户名和密码，存入map中
    while (MYSQL_ROW row = mysql_fetch_row(result))
    {
        string temp1(row[0]);
        string temp2(row[1]);
        users[temp1] = temp2;
    }
    

}

//对文件描述符设置非阻塞
int setnonblocking(int fd)
{
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

//将内核事件表注册读事件，ET模式，选择开启EPOLLONESHOT
//内核事件表注册新事件，开启EPOLLONESHOT，针对客户端连接的描述符，listenfd不用开启
void addfd(int epollfd, int fd, bool one_shot, int TRIGMode)
{
    epoll_event event;
    event.data.fd = fd;

    if (1 == TRIGMode)//ET模式
        event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    else
        event.events = EPOLLIN | EPOLLRDHUP;

    if (one_shot)
        event.events |= EPOLLONESHOT;
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setnonblocking(fd);//设置文件描述符非阻塞
}

//从内核事件表删除描述符
void removefd(int epollfd, int fd)
{
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
    close(fd);
}

//修改文件描述符，重置socket上的EPOLLONESHOT事件，以确保下一次可读时，EPOLLIN事件能被触发
void modfd(int epollfd, int fd, int ev, int TRIGMode)
{
    epoll_event event;
    event.data.fd = fd;

    if (1 == TRIGMode)
        event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
    else
        event.events = ev | EPOLLONESHOT | EPOLLRDHUP;

    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}

int http_conn::m_user_count = 0;
int http_conn::m_epollfd = -1;

//关闭连接，关闭一个连接，客户总量减一
void http_conn::close_conn(bool real_close)
{
    if (real_close && (m_sockfd != -1))
    {
        printf("close %d\n", m_sockfd);
        removefd(m_epollfd, m_sockfd);
        m_sockfd = -1;
        m_user_count--;
    }
}

//初始化连接,外部调用初始化套接字地址
void http_conn::init(int sockfd, const sockaddr_in &addr, char *root, int TRIGMode,
                     int close_log, string user, string passwd, string sqlname)
{
    m_sockfd = sockfd;
    m_address = addr;

    addfd(m_epollfd, sockfd, true, m_TRIGMode);
    m_user_count++;

    //当浏览器出现连接重置时，可能是网站根目录出错或http响应格式出错或者访问的文件中内容完全为空
    doc_root = root;
    m_TRIGMode = TRIGMode;
    m_close_log = close_log;

    strcpy(sql_user, user.c_str());
    strcpy(sql_passwd, passwd.c_str());
    strcpy(sql_name, sqlname.c_str());

    init();
}

//初始化新接受的连接
//check_state默认为分析请求行状态
void http_conn::init()
{
    mysql = NULL;
    bytes_to_send = 0;
    bytes_have_send = 0;
    m_check_state = CHECK_STATE_REQUESTLINE;    // 初始状态为检查请求行
    m_linger = false;       // 默认不保持链接  Connection : keep-alive保持连接

    m_method = GET;         // 默认请求方式为GET
    m_url = 0;
    m_version = 0;
    m_content_length = 0;
    m_host = 0;
    m_start_line = 0;
    m_checked_idx = 0;
    m_read_idx = 0;
    m_write_idx = 0;
    cgi = 0;
    m_state = 0;
    timer_flag = 0;
    improv = 0;

    memset(m_read_buf, '\0', READ_BUFFER_SIZE);
    memset(m_write_buf, '\0', WRITE_BUFFER_SIZE);
    memset(m_real_file, '\0', FILENAME_LEN);
}

//从状态机，用于分析出一行内容。读取一行，分析是请求报文的哪一部分
//返回值为行的读取状态，有LINE_OK,LINE_BAD,LINE_OPEN
http_conn::LINE_STATUS http_conn::parse_line()
{
    char temp;
    //m_read_idx指向缓冲区m_read_buf的数据末尾的下一个字节
    //m_checked_idx指向从状态机当前正在分析的字节
    for (; m_checked_idx < m_read_idx; ++m_checked_idx)
    {
        //temp为将要分析的字节(数据/字母)
        temp = m_read_buf[m_checked_idx];

        //如果当前是\r字符，则有可能会读取到完整行
        if (temp == '\r')
        {
            //下一个字符达到了buffer结尾，则接收不完整，需要继续接收
            //一行放不下，所以弄了两行数据，所以要继续接受
            if ((m_checked_idx + 1) == m_read_idx)
                return LINE_OPEN;
            
            //下一个字符是\n，将\r\n改为\0\0
            else if (m_read_buf[m_checked_idx + 1] == '\n')
            {
                m_read_buf[m_checked_idx++] = '\0';
                m_read_buf[m_checked_idx++] = '\0';//此时指向第二个\0后面
                return LINE_OK;
            }
            //如果都不符合，则返回语法错误
            return LINE_BAD;
        }

        //如果当前字符是\n，也有可能读取到完整行
        //一般是上次读取到\r就到buffer末尾了，没有接收完整，再次接收时会出现这种情况
        //也就是上次末尾是\r，这次开头是\n
        else if (temp == '\n')
        {
            //前一个字符是\r，则接收完整
            if (m_checked_idx > 1 && m_read_buf[m_checked_idx - 1] == '\r')
            {
                m_read_buf[m_checked_idx - 1] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
    }
    return LINE_OPEN;
}

//循环读取客户数据，直到无数据可读或对方关闭连接
//非阻塞ET工作模式下，需要一次性将数据读完
//循环读取客户数据，直到无数据可读或者对方关闭连接
bool http_conn::read_once()
{
    if (m_read_idx >= READ_BUFFER_SIZE)
    {
        return false;
    }
    int bytes_read = 0;

    //LT读取数据，默认水平触发
    if (0 == m_TRIGMode)
    {
        bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
        m_read_idx += bytes_read;//这里其实就是加的间隔

        if (bytes_read <= 0)
        {
            return false;
        }
        //我自己测试输出
        // FILE *f = fopen("test.txt","r+");
        // string shuchu = m_read_buf;
        // fprintf(f,shuchu.c_str());
        // fclose(f);
        //我自己测试输出

        //std::string str(m_read_buf);
        //std::cout << str << std::endl;
        return true;
    }
    //ET读数据
    else
    {
        while (true)
        {
//从m_read_buf + m_read_idx索引出开始保存数据，大小是READ_BUFFER_SIZE - m_read_idx
            bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
            if (bytes_read == -1)
            {
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    //没有数据
                    break;
                return false;
            }
            else if (bytes_read == 0)// 对方关闭连接
            {
                return false;
            }
            //修改m_read_idx的读取字节数
            m_read_idx += bytes_read;
        }
        return true;
    }
}

//解析http请求行，获得请求方法，目标url及http版本号
http_conn::HTTP_CODE http_conn::parse_request_line(char *text)
{
    //在HTTP报文中，请求行用来说明请求类型,要访问的资源以及所使用的HTTP版本
    //其中各个部分之间通过\t或空格分隔
    //请求行中最先含有空格和\t任一字符的位置并返回
    //strpbrk()函数是判断后面的参数哪个先在前面参数出现，也就是空格或者\t哪个先在text里出现
    m_url = strpbrk(text, " \t");
    
    //如果没有空格或\t，则报文格式有误
    if (!m_url)
    {
        return BAD_REQUEST;
    }
    
    //将该位置改为\0，用于将前面数据取出
    //比如说:GET /index.html HTTP/1.1
    *m_url++ = '\0';

    //取出数据，并通过与GET和POST比较，以确定请求方式
    char *method = text;
    if (strcasecmp(method, "GET") == 0)
        m_method = GET;
    else if (strcasecmp(method, "POST") == 0)
    {
        m_method = POST;
        cgi = 1;
    }
    else
        return BAD_REQUEST;
    
    //m_url此时跳过了第一个空格或\t字符，但不知道之后是否还有
    //将m_url向后偏移，通过查找，继续跳过空格和\t字符，指向请求资源的第一个字符
    //strspn函数是第一个参数从头找，有几个出现在第二个参数里
    m_url += strspn(m_url, " \t");
    
    //变成了这个/index.html HTTP/1.1
    m_version = strpbrk(m_url, " \t");
    
    if (!m_version)
        return BAD_REQUEST;
    *m_version++ = '\0';
    
    m_version += strspn(m_version, " \t");
    if (strcasecmp(m_version, "HTTP/1.1") != 0)//仅支持HTTP/1.1
        return BAD_REQUEST;
    
    /**
     * 本来是/index.html
     * 但有的可能是http://192.168.110.129:10000/index.html
    */
    if (strncasecmp(m_url, "http://", 7) == 0)
    {
        m_url += 7;
        //+7之后192.168.110.129:10000/index.html
        m_url = strchr(m_url, '/');//变成/index.html
    }
    //https同样处理
    if (strncasecmp(m_url, "https://", 8) == 0)
    {
        m_url += 8;
        m_url = strchr(m_url, '/');
    }

    if (!m_url || m_url[0] != '/')
        return BAD_REQUEST;
    
    //当url为/时，显示判断界面
    if (strlen(m_url) == 1)//一开始是GET / HTTP/1.1,最后murl指向n中间的/
        strcat(m_url, "index.html");
    
    //请求行处理完毕，将主状态机转移处理请求头
    m_check_state = CHECK_STATE_HEADER;
    return NO_REQUEST;
}

//解析http请求的一个头部信息
http_conn::HTTP_CODE http_conn::parse_headers(char *text)
{
    /*
    在报文中，请求头和空行的处理使用的同一个函数，这里通过判断当前的text首位是不是\0字符，
    若是，则表示当前处理的是空行，若不是，则表示当前处理的是请求头。
    */
    if (text[0] == '\0')
    {
        //判断是GET还是POST请求
        //如果HTTP请求有消息体，则还需要读取m_content_length字节的消息体，
        //状态机转移到CHECK_STATE_CONTENT状态
        if (m_content_length != 0)
        {
            //POST需要跳转到消息体处理状态
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
        //否则说明我们已经得到了一个完整的HTTP请求
        return GET_REQUEST;
    }
    //解析请求头部连接字段
    else if (strncasecmp(text, "Connection:", 11) == 0)
    {
        // 处理Connection 头部字段  Connection: keep-alive
        text += 11;
        //跳过空格和\t字符
        text += strspn(text, " \t");
        if (strcasecmp(text, "keep-alive") == 0)
        {
            //如果是长连接，则将linger标志设置为true
            m_linger = true;
        }
    }
    //解析请求头部内容长度字段
    else if (strncasecmp(text, "Content-length:", 15) == 0)
    {
        text += 15;
        text += strspn(text, " \t");
        m_content_length = atol(text);
    }
    //解析请求头部HOST字段
    else if (strncasecmp(text, "Host:", 5) == 0)
    {
        text += 5;
        text += strspn(text, " \t");
        m_host = text;
    }
    else
    {
        LOG_INFO<<"oop!unknow header: %s"<<text;
    }
    return NO_REQUEST;
}

//判断http请求是否被完整读入
http_conn::HTTP_CODE http_conn::parse_content(char *text)
{
    /*
    parse_content(text);，解析请求数据，对于GET来说这部分是空的，
    因为这部分内容已经以明文的方式包含在了请求行中的URL部分了；
    只有POST的这部分是有数据的，项目中的这部分数据为用户名和密码，
    我们会根据这部分内容做登录和校验，并涉及到与数据库的连接。
    */
    if (m_read_idx >= (m_content_length + m_checked_idx))
    {
        text[m_content_length] = '\0';
        //POST请求中最后为输入的用户名和密码
        m_string = text;
        return GET_REQUEST;
    }
    return NO_REQUEST;
}

http_conn::HTTP_CODE http_conn::process_read()
{
    //初始化从状态机状态、HTTP请求解析结果
    LINE_STATUS line_status = LINE_OK;
    HTTP_CODE ret = NO_REQUEST;
    char *text = 0;

    /*
    在GET请求报文中，每一行都是\r\n作为结束，
    所以对报文进行拆解时，仅用从状态机的状态line_status=parse_line())==LINE_OK语句即可。

    但，在POST请求报文中，消息体的末尾没有任何字符，所以不能使用从状态机的状态，
    这里转而使用主状态机的状态作为循环入口条件。解析完消息体后，报文的完整解析就完成了，
    但此时主状态机的状态还是CHECK_STATE_CONTENT，
    也就是说，符合循环入口条件，还会再次进入循环，这并不是我们所希望的。
    所以是(m_check_state == CHECK_STATE_CONTENT && line_status == LINE_OK)
    */
    while ((m_check_state == CHECK_STATE_CONTENT && line_status == LINE_OK) || 
          ((line_status = parse_line()) == LINE_OK))
    {
        //m_start_line是行在buffer中的起始位置，将该位置后面的数据赋给text
        //此时从状态机已提前将一行的末尾字符\r\n变为\0\0，所以text可以直接取出完整的行进行解析
        text = get_line();
        
        //m_start_line是每一个数据行在m_read_buf中的起始位置
        //m_checked_idx表示从状态机在m_read_buf中读取的位置
        m_start_line = m_checked_idx;
        
        //LOG_INFO("%s", text);
        //LOG_INFO("%s", "yi_hang_du_wan");
        switch (m_check_state)
        {
            case CHECK_STATE_REQUESTLINE:
            {
                ret = parse_request_line(text);
                if (ret == BAD_REQUEST)
                    return BAD_REQUEST;
                break;
            }
            
            case CHECK_STATE_HEADER:
            {
                ret = parse_headers(text);
                if (ret == BAD_REQUEST)
                    return BAD_REQUEST;
                //完整解析GET请求后，跳转到报文响应函数
                else if (ret == GET_REQUEST)
                {
                    return do_request();
                }
                break;
            }
            
            case CHECK_STATE_CONTENT:
            {
                //解析消息体
                ret = parse_content(text);
                //完整解析POST请求后，跳转到报文响应函数
                if (ret == GET_REQUEST)
                    return do_request();
                //解析完消息体即完成报文解析，避免再次进入循环，更新line_status    
                line_status = LINE_OPEN;
                break;
            }
        
        default:
            return INTERNAL_ERROR;
        }
    }
    return NO_REQUEST;
}

http_conn::HTTP_CODE http_conn::do_request()
{
    //将初始化的m_real_file赋值为网站根目录
    strcpy(m_real_file, doc_root);
    int len = strlen(doc_root);
    //printf("m_url:%s\n", m_url);
    
    //找到m_url中/的位置
    const char *p = strrchr(m_url, '/');

    //处理cgi
    //实现登录和注册校验(这一部分在数据库那里的文章)
    if (cgi == 1 && (*(p + 1) == '2' || *(p + 1) == '3'))
    {

        //根据标志判断是登录检测还是注册检测
        char flag = m_url[1];//没用啊老铁

        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/");
        strcat(m_url_real, m_url + 2);
        strncpy(m_real_file + len, m_url_real, FILENAME_LEN - len - 1);
        free(m_url_real);

        //将用户名和密码提取出来
        //user=123&passwd=123
        char name[100], password[100];

        //以&为分隔符，前面的为用户名
        int i;
        for (i = 5; m_string[i] != '&'; ++i)
            name[i - 5] = m_string[i];
        name[i - 5] = '\0';

        //以&为分隔符，后面的是密码
        int j = 0;
        for (i = i + 10; m_string[i] != '\0'; ++i, ++j)
            password[j] = m_string[i];
        password[j] = '\0';


        //同步线程登录注册
        if (*(p + 1) == '3')
        {
            //如果是注册，先检测数据库中是否有重名的
            //没有重名的，进行增加数据
            char *sql_insert = (char *)malloc(sizeof(char) * 200);
            strcpy(sql_insert, "INSERT INTO user(username, passwd) VALUES(");
            strcat(sql_insert, "'");
            strcat(sql_insert, name);
            strcat(sql_insert, "', '");
            strcat(sql_insert, password);
            strcat(sql_insert, "')");

            //判断map中能否找到重复的用户名
            if (users.find(name) == users.end())
            {
                //向数据库中插入数据时，需要通过锁来同步数据
                m_lock.lock();
                int res = mysql_query(mysql, sql_insert);
                users.insert(pair<string, string>(name, password));
                m_lock.unlock();

                //校验成功，跳转登录页面
                if (!res)
                    strcpy(m_url, "/login.html");

                //校验失败，跳转注册失败页面
                else
                    strcpy(m_url, "/registererror.html");
            }
            else
                strcpy(m_url, "/registererror.html");
        }
        //如果是登录，直接判断
        //若浏览器端输入的用户名和密码在表中可以查找到，返回1，否则返回0
        else if (*(p + 1) == '2')
        {
            if (users.find(name) != users.end() && users[name] == password)
                strcpy(m_url, "/welcome.html");
            else
                strcpy(m_url, "/loginerror.html");
        }
    }
    //从这开始
    //如果请求资源为/0，表示跳转注册界面
    if (*(p + 1) == '0')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/register.html");

        //将网站目录和/register.html进行拼接，更新到m_real_file中
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }

    //如果请求资源为/1，表示跳转登录界面
    else if (*(p + 1) == '1')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/log.html");

        //将网站目录和/log.html进行拼接，更新到m_real_file中
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }

    //跳转别的界面，这里可以自己设计了
    else if (*(p + 1) == '5')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/picture.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if (*(p + 1) == '6')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/video.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if (*(p + 1) == '7')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/fans.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    //到这结束代码其实没用了
    else
        strncpy(m_real_file + len, m_url, FILENAME_LEN - len - 1);

    //通过stat获取请求资源文件信息，成功则将信息更新到m_file_stat结构体
    //失败返回NO_RESOURCE状态，表示资源不存在
    if (stat(m_real_file, &m_file_stat) < 0)
        return NO_RESOURCE;

    //判断文件的权限，是否可读，不可读则返回FORBIDDEN_REQUEST状态
    if (!(m_file_stat.st_mode & S_IROTH))
        return FORBIDDEN_REQUEST;

    //判断文件类型，如果是目录，则返回BAD_REQUEST，表示请求报文有误
    if (S_ISDIR(m_file_stat.st_mode))
        return BAD_REQUEST;

    //以只读方式获取文件描述符，通过mmap将该文件映射到内存中
    int fd = open(m_real_file, O_RDONLY);
    m_file_address = (char *)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);

    //表示请求文件存在，且可以访问
    return FILE_REQUEST;
}

void http_conn::unmap()
{
    if (m_file_address)
    {
        munmap(m_file_address, m_file_stat.st_size);
        m_file_address = 0;
    }
}

bool http_conn::write()
{

    /*
    该函数具体逻辑如下：

    在生成响应报文时初始化byte_to_send，包括头部信息和文件数据大小。
    通过writev函数循环发送响应报文数据，根据返回值更新byte_have_send和iovec结构体的指针和长度，
    并判断响应报文整体是否发送成功。

        若writev单次发送成功，更新byte_to_send和byte_have_send的大小，
        若响应报文整体发送成功,则取消mmap映射,并判断是否是长连接.

                长连接重置http类实例，注册读事件，不关闭连接，
                
                短连接直接关闭连接

        若writev单次发送不成功，判断是否是写缓冲区满了。
            若不是因为缓冲区满了而失败，取消mmap映射，关闭连接
            
            若eagain则满了，更新iovec结构体的指针和长度，并注册写事件，
            等待下一次写事件触发（当写缓冲区从不可写变为可写，触发epollout），
            因此在此期间无法立即接收到同一用户的下一请求，但可以保证连接的完整性。
    */

    int temp = 0;

    //若要发送的数据长度为0
    //表示响应报文为空，一般不会出现这种情况
    if (bytes_to_send == 0)
    {
        modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);
        init();
        return true;
    }

    while (1)
    {
        //将响应报文的状态行、消息头、空行和响应正文发送给浏览器端
        temp = writev(m_sockfd, m_iv, m_iv_count);

        //非正常发送
        if (temp < 0)
        {
            //判断缓冲区是否满了
            if (errno == EAGAIN)
            {
                //重新注册写事件
                modfd(m_epollfd, m_sockfd, EPOLLOUT, m_TRIGMode);
                return true;
            }
            //如果发送失败，但不是缓冲区问题，取消映射
            unmap();
            return false;
        }

        //正常发送，temp为发送的字节数
        bytes_have_send += temp;//更新已发送字节
        bytes_to_send -= temp;//更新已发送字节数

        //第一个iovec头部信息的数据已发送完，发送第二个iovec数据
        if (bytes_have_send >= m_iv[0].iov_len)
        {
            //不再继续发送头部信息
            m_iv[0].iov_len = 0;
            m_iv[1].iov_base = m_file_address + (bytes_have_send - m_write_idx);
            m_iv[1].iov_len = bytes_to_send;
        }
        else
        {
            //继续发送第一个iovec头部信息的数据
            m_iv[0].iov_base = m_write_buf + bytes_have_send;
            m_iv[0].iov_len = m_iv[0].iov_len - bytes_have_send;
        }

        //判断条件，数据已全部发送完
        if (bytes_to_send <= 0)
        {
            unmap();

            //在epoll树上重置EPOLLONESHOT事件
            modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);

            //浏览器的请求为长连接
            if (m_linger)
            {
                //重新初始化HTTP对象
                init();
                return true;
            }
            else
            {
                return false;
            }
        }
    }
}

//均是内部调用add_response函数更新m_write_idx指针和缓冲区m_write_buf中的内容
bool http_conn::add_response(const char *format, ...)
{
    //如果写入内容超出m_write_buf大小则报错
    if (m_write_idx >= WRITE_BUFFER_SIZE)
        return false;
    
    //定义可变参数列表
    va_list arg_list;
    
    //将变量arg_list初始化为传入参数
    va_start(arg_list, format);

    //将数据format从可变参数列表写入缓冲区写，返回写入数据的长度
    int len = vsnprintf(m_write_buf + m_write_idx, WRITE_BUFFER_SIZE - 1 - m_write_idx, format, arg_list);
    
    //如果写入的数据长度超过缓冲区剩余空间，则报错
    if (len >= (WRITE_BUFFER_SIZE - 1 - m_write_idx))
    {
        va_end(arg_list);
        return false;
    }

    //更新m_write_idx位置
    m_write_idx += len;

    //清空可变参列表
    va_end(arg_list);

    LOG_INFO<<"request:%s"<<m_write_buf;

    return true;
}

//add_status_line函数，添加状态行：http/1.1 状态码 状态消息
bool http_conn::add_status_line(int status, const char *title)
{
    return add_response("%s %d %s\r\n", "HTTP/1.1", status, title);
}

/*add_headers函数添加消息报头，内部调用add_content_length和add_linger函数
content-length记录响应报文长度，用于浏览器端判断服务器是否发送完数据
connection记录连接状态，用于告诉浏览器端保持长连接
添加消息报头，具体的添加文本长度、连接状态和空行*/
bool http_conn::add_headers(int content_len)
{
    return add_content_length(content_len) && add_linger() &&
           add_blank_line();
}

//添加Content-Length，表示响应报文的长度
bool http_conn::add_content_length(int content_len)
{
    return add_response("Content-Length:%d\r\n", content_len);
}

//添加文本类型，这里是html
bool http_conn::add_content_type()
{
    return add_response("Content-Type:%s\r\n", "text/html");
}

//添加连接状态，通知浏览器端是保持连接还是关闭
bool http_conn::add_linger()
{
    return add_response("Connection:%s\r\n", (m_linger == true) ? "keep-alive" : "close");
}

//add_blank_line添加空行
bool http_conn::add_blank_line()
{
    return add_response("%s", "\r\n");
}

//添加文本content
bool http_conn::add_content(const char *content)
{
    return add_response("%s", content);
}

/*
响应报文分为两种，一种是请求文件的存在，通过io向量机制iovec，声明两个iovec，
第一个指向m_write_buf，第二个指向mmap的地址m_file_address；
一种是请求出错，这时候只申请一个iovec，指向m_write_buf。

*iovec是一个结构体，里面有两个元素，指针成员iov_base指向一个缓冲区，
这个缓冲区是存放的是writev将要发送的数据。

成员iov_len表示实际写入的长度

struct iovec {
    void *iov_base;      
    size_t iov_len;        
};*/
//根据do_request的返回状态，服务器子线程调用process_write向m_write_buf中写入响应报文。
bool http_conn::process_write(HTTP_CODE ret)
{
    switch (ret)
    {
        //内部错误，500
        case INTERNAL_ERROR:
        {
            //状态行
            add_status_line(500, error_500_title);
            //消息报头
            add_headers(strlen(error_500_form));
            if (!add_content(error_500_form))
                return false;
            break;
        }

        //报文语法有误，404
        case BAD_REQUEST:
        {
            add_status_line(404, error_404_title);
            add_headers(strlen(error_404_form));
            if (!add_content(error_404_form))
                return false;
            break;
        }
        //1
        //资源没有访问权限，403
        case FORBIDDEN_REQUEST:
        {
            add_status_line(403, error_403_title);
            add_headers(strlen(error_403_form));
            if (!add_content(error_403_form))
                return false;
            break;
        }

        //文件存在，200
        case FILE_REQUEST:
        {
            add_status_line(200, ok_200_title);

            //如果请求的资源存在
            if (m_file_stat.st_size != 0)
            {
                add_headers(m_file_stat.st_size);
                //第一个iovec指针指向响应报文缓冲区，长度指向m_write_idx
                m_iv[0].iov_base = m_write_buf;
                m_iv[0].iov_len = m_write_idx;

                //第二个iovec指针指向mmap返回的文件指针，长度指向文件大小
                m_iv[1].iov_base = m_file_address;
                m_iv[1].iov_len = m_file_stat.st_size;
                m_iv_count = 2;

                //发送的全部数据为响应报文头部信息和文件大小
                bytes_to_send = m_write_idx + m_file_stat.st_size;
                return true;
            }
            else
            {
                //如果请求的资源大小为0，则返回空白html文件
                const char *ok_string = "<html><body></body></html>";
                add_headers(strlen(ok_string));
                if (!add_content(ok_string))
                    return false;
            }
        }
    default:
        return false;
    }

    //除FILE_REQUEST状态外，其余状态只申请一个iovec，指向响应报文缓冲区
    m_iv[0].iov_base = m_write_buf;
    m_iv[0].iov_len = m_write_idx;
    m_iv_count = 1;
    bytes_to_send = m_write_idx;
    return true;
}

//由线程池中的工作线程调用，这是处理HTTP请求的入口函数
void http_conn::process()
{
    //解析HTTP请求
    HTTP_CODE read_ret = process_read();
    //NO_REQUEST，表示请求不完整，需要继续接收请求数据
    if (read_ret == NO_REQUEST)
    {
        //注册并监听读事件
        modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);
        return;
    }
    //调用process_write完成报文响应
    bool write_ret = process_write(read_ret);
    if (!write_ret)
    {
        close_conn();
    }
    //注册并监听写事件
    /*
    服务器子线程调用process_write完成响应报文，随后注册epollout事件。
    服务器主线程检测写事件，并调用http_conn::write函数将响应报文发送给浏览器端。
    */
    modfd(m_epollfd, m_sockfd, EPOLLOUT, m_TRIGMode);
}
