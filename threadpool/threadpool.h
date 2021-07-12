#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <list>
#include <cstdio>
#include <exception>
#include <pthread.h>
#include "../lock/locker.h"
#include "../CGImysql/sql_connection_pool.h"

template <typename T>
class threadpool
{
public:
    /* thread_number is the number of threads in the pool, max_requests is the max number in the request queue which is going to be processed and allowed */
    threadpool(connection_pool *connPool, int thread_number = 8, int max_request = 10000);
    ~threadpool();
    bool append(T *request);

private:
    /* worker function, which fetch jobs from the queue and process it */
    static void *worker(void *arg);
    void run();

private:
    int m_thread_number;         //线程池中的线程数
    int m_max_requests;          //请求队列中允许的最大请求数
    pthread_t *m_threads;        //描述线程池的数组，其大小为m_thread_number
    std::list<T *> m_workqueue;  //请求队列
    locker m_queuelocker;        //保护请求队列的互斥锁
    sem m_queuestat;             //是否有任务需要处理
    bool m_stop;                 //是否结束线程
    connection_pool *m_connPool; //数据库
};

template <typename T>
threadpool<T>::threadpool(connection_pool *connPool, int thread_number, int max_requests) : m_thread_number(thread_number), m_max_requests(max_requests), m_stop(false), m_threads(NULL), m_connPool(connPool)
{
    if (thread_number <= 0 || max_requests <= 0)
    {
        throw std::exception();
    }

    //initialize thread id
    m_threads = new pthread_t[m_thread_number];
    if (!m_threads)
    {
        throw std::exception();
    }
    for (int i = 0; i < thread_number; i++)
    {
        //create threads circularly and make work thread do they have
        //why `this` is an arg?
        if (pthread_create(m_threads + i, NULL, worker, this) != 0)
        {
            delete[] m_threads;
            throw std::exception();
        }

        //detach the thread and no need to retreive them
        if (pthread_detach(m_threads[i]))
        {
            delete[] m_threads;
            throw std::exception();
        }
    }
}

template <typename T>
threadpool<T>::~threadpool()
{
    delete[] m_threads;
    m_stop = true;
}

template <typename T>
bool threadpool<T>::append(T *request)
{
    m_queuelocker.lock();

    //Set the maxsize of the request queue in advance according to your hardware
    if (m_workqueue.size() > m_max_requests)
    {
        m_queuelocker.unlock();
        return false;
    }

    //add job
    m_workqueue.push_back(request);
    m_queuelocker.unlock();

    //signal that there's a job to be processed
    m_queuestat.post();
    return true;
}

//thread process function
template <typename T>
void *threadpool<T>::worker(void *arg)
{
    threadpool *pool = (threadpool *) arg;
    pool->run();
    return pool;
}

template<typename T>
void threadpool<T>::run()
{
    while (!m_stop)
    {
        //wait for signal
        m_queuestat.wait();

        //lock it after woken up
        m_queuelocker.lock();
        if (m_workqueue.empty)
        {
            m_queuelocker.unlock();
            continue;
        }

        /**
         * fetch the first job from the request list
         * and delete it
         */
        T* request = m_workqueue.front();
        m_workqueue.pop_front();
        m_queuelocker.unlock();

        if (!request) {
            continue;
        }
        //fetch a connection from the pool
        request->mysql = m_connPool->GetConnection();
        
        //process it;
        request->process();

        //put the connection back to the pool
        m_connPool->ReleaseConnection(request->mysql);
    }
}

#endif