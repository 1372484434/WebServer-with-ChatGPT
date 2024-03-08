#include <mysql/mysql.h>
#include <stdio.h>
#include <string>
#include <string.h>
#include <stdlib.h>
#include <list>
#include <pthread.h>
#include <iostream>
#include "sql_connection_pool.h"

using namespace std;

connection_pool::connection_pool()
{
	m_CurConn = 0;
	m_FreeConn = 0;
}

//优雅的实现单例模式，使用函数内的局部静态对象，这种方法不用加锁和解锁操作。
connection_pool *connection_pool::GetInstance()
{
	static connection_pool connPool;
	return &connPool;
}

//构造初始化
void connection_pool::init(string url, string User, string PassWord, string DBName, int Port, int MaxConn, int close_log)
{
	//初始化数据库信息
	m_url = url;
	m_Port = Port;
	m_User = User;
	m_PassWord = PassWord;
	m_DatabaseName = DBName;
	m_close_log = close_log;

	//创建MaxConn(数据库连接数量)条数据库连接
	for (int i = 0; i < MaxConn; i++)
	{
		MYSQL *con = NULL;
		con = mysql_init(con);
		/*这个函数用来分配或者初始化一个MYSQL对象，用于连接mysql服务端。如果你传入的参数是NULL指针，它将自动为你分配一个MYSQL对象，如果这个MYSQL对象是它自动分配的，那么在调用mysql_close的时候，会释放这个对象。*/

		if (con == NULL)
		{
			//LOG_ERROR("MySQL initError");
			LOG_INFO << "MySQL initError";
			exit(1);
		}
		//如果连接成功，返回MYSQL*连接句柄。如果连接失败，返回NULL。对于成功的连接，返回值与第1个参数的值相同。
		con = mysql_real_connect(con, url.c_str(), User.c_str(), PassWord.c_str(), DBName.c_str(), Port, NULL, 0);

		if (con == NULL)
		{
			//LOG_ERROR("MySQL connectError");
			LOG_INFO << "MySQL connectError";
			exit(1);
		}
		//更新连接池和空闲连接数量
		connList.push_back(con);
		++m_FreeConn;
	}

	//将信号量初始化为最大连接次数
	reserve = sem(m_FreeConn);

	m_MaxConn = m_FreeConn;
}


//当有请求时，从数据库连接池中返回一个可用连接，更新使用和空闲连接数
MYSQL *connection_pool::GetConnection()
{
	MYSQL *con = NULL;

	if (0 == connList.size())
		return NULL;

	//取出连接，信号量原子减1，为0则等待
	reserve.wait();
	
	lock.lock();

	con = connList.front();
	connList.pop_front();

	//这里的两个变量，并没有用到，非常鸡肋...
	--m_FreeConn;
	++m_CurConn;

	lock.unlock();
	return con;
}

//释放当前使用的连接
bool connection_pool::ReleaseConnection(MYSQL *con)
{
	if (NULL == con)
		return false;

	lock.lock();

	connList.push_back(con);
	++m_FreeConn;
	--m_CurConn;

	lock.unlock();

	//释放连接原子加1
	reserve.post();
	return true;
}

//销毁数据库连接池
void connection_pool::DestroyPool()
{

	lock.lock();
	if (connList.size() > 0)
	{
		//通过迭代器遍历，关闭数据库连接
		list<MYSQL *>::iterator it;
		for (it = connList.begin(); it != connList.end(); ++it)
		{
			MYSQL *con = *it;
			mysql_close(con);
		}
		m_CurConn = 0;
		m_FreeConn = 0;
		//清空list
		connList.clear();
	}

	lock.unlock();
}

//当前空闲的连接数
int connection_pool::GetFreeConn()
{
	return this->m_FreeConn;
}

connection_pool::~connection_pool()
{
	DestroyPool();
}

/*
将数据库连接的获取与释放通过RAII机制封装，避免手动释放。
这里需要注意的是，在获取连接时，通过有参构造对传入的参数进行修改。
其中数据库连接本身是指针类型，所以参数需要通过双指针才能对其进行修改。
*/
connectionRAII::connectionRAII(MYSQL* *SQL, connection_pool *connPool){
	*SQL = connPool->GetConnection();
	
	conRAII = *SQL;
	poolRAII = connPool;
}

//将数据库连接的获取与释放通过RAII机制封装，避免手动释放。
connectionRAII::~connectionRAII(){
	poolRAII->ReleaseConnection(conRAII);
}