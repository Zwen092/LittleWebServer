#ifndef _CONNECTION_POOL_
#define _CONNECTION_POOL_

#include <stdio.h>
#include <list>
#include <mysql/mysql.h>
#include <error.h>
#include <string.h>
#include <iostream>
#include <iostream>
#include <string>
#include "../lock/locker.h"

using namespace std;

class connection_pool
{
    public:
        MYSQL *GetConnection();
        bool ReleaseConnection(MYSQL *conn);
        int GetFreeConn();
        void DestroyPool();

        //singleton
        static connection_pool *GetInstance();

        void init(string url, string User, string PassWord, string DataBaseName, int Port, unsigned int MaxConn);

        connection_pool();
        ~connection_pool();
    
    private:
        unsigned int MaxConn;
        unsigned int CurConn; //current used connections
        unsigned int FreeConn;

    private:
        locker lock;
        list<MYSQL *> connList; //connnection pool
        sem reserve;
    
    private:
        string url;
        string Port;
        string User;
        string PassWord;
        string DatabaseName;
};

class connectionRAII {
    public:
        connectionRAII(MYSQL **con, connection_pool *connPool);
        ~connectionRAII();
    private:
        MYSQL *conRAII;
        connection_pool *poolRAII;
};

#endif