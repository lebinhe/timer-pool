#include <string.h>
#include "TimerPool.h"

#define ERROR_CHECK(arg1, arg2, arg3) if (arg1 != arg2) cerr << arg3 << endl;
#define DEFAULT_EXECUTOR_COUNT 5

namespace timer_pool
{

    Timer::~Timer() {}

    /**
       TODOs:
       - when timer or pool is destroyed, should wait for some amount of time, then
       kill it, if it doesn't return from timerEvent()

       - should somehow implement thread cleaning. Problem currently is -
       when destroying timer, it waits for executor to exit handler. Waiting
       happens outside timer pool mutex. It is possible that after unlocking
       timer pool mutex in removeTimer() following events take place:
       1) executor, which is being wait for, finishes it's work and returns to
       idle state
       2) cleaner thread starts to clean and deletes executor
       3) meanwhile, thread in removeTimer() tries to call waitUntilServiced()
       for executor, which is allready deleted.
       Solution:
       - introduce new mutex, which guards against this case

       - allow to delete TimerPool from executor thread 

       - invalidate timers after timer pool has gone

       - currently Start() equals Restart() - still, there could be
       subtle differences between them, when enqueueing timer events; should
       it be implemented as well?...

       - check if getInstance() sleep is correct

       - output some warning in case executor count is increased
    */

    /**
       Ownership rules:
       - TimerPool owns ExecutorContainer
       - ExecutorContainer owns Executor instances
       - TimerPool doesn't own PooledTimer instances

       Existing mutex lock sequences:
       1. PooledTimer::Start(), PooledTimer::Restart()
       - PooledTimer m
       - TimerPool m

       2. PooledTimer::Stop(), PooledTimer::~PooledTimer()
       - PooledTimer m
       - PooledTimer m
       - ExecutorContainer m

       2. PooledTimer::Stop(), PooledTimer::~PooledTimer()
       - PooledTimer m
       - Executor m

       3. TimerPool::doExecute()
       - TimerPool m
       - ExecutorContainer m

       4. TimerPool::doExecute()
       - TimerPool m
       - Executor m

       5. Executor::tryExecute(), Executor::reduceCount()
       - Executor m
       - ExecutorContainer m

       There are three hard places in the code, due to "action without mutex to
       prevent deadlock":
       - TimerPool::ExecutionerContainer::shutdown()
       - TimerPool::stopTimer()
       - TimerPool::doExecute()
       Read comments about assumptions.
    */


    TimerFactory::~TimerFactory() {}

    PooledTimer::PooledTimer(TimerPool* tp)
    {
        m_timerPool = tp;
        init();
    }

    void PooledTimer::init()
    {
        m_listener = NULL;
        m_time = 1; // safety precaution: default time is one second

        pthread_mutexattr_t attr;
        ERROR_CHECK(pthread_mutexattr_init(&attr), 0, "Couldn't init mutex attr");
        ERROR_CHECK(pthread_mutex_init(&m_fieldGuard, &attr), 0,
                    "Couldn't init mutex");
        ERROR_CHECK(pthread_mutexattr_destroy(&attr), 0,
                    "Couldn't destroy mutex attr");
    }

    PooledTimer::~PooledTimer()
    {
        m_timerPool->removeTimer(this);
        pthread_mutex_destroy(&m_fieldGuard);
    }

    void PooledTimer::setListener(TimerListener* new_listener)
    {
        pthread_mutex_lock(&m_fieldGuard);
        m_listener = new_listener;
        pthread_mutex_unlock(&m_fieldGuard);
    }

    unsigned PooledTimer::getTime()
    {
        pthread_mutex_lock(&m_fieldGuard);
        unsigned t = m_time;    
        pthread_mutex_unlock(&m_fieldGuard);
    
        return t;
    }

    void PooledTimer::start(unsigned time)
    {
        pthread_mutex_lock(&m_fieldGuard);
        m_time = time;
        timespec t = getTargetTime();
        m_timerPool->startTimer(this, t);
        pthread_mutex_unlock(&m_fieldGuard);
    }

    void PooledTimer::stop()
    {
        pthread_mutex_lock(&m_fieldGuard);
        m_timerPool->stopTimer(this);
        pthread_mutex_unlock(&m_fieldGuard);
    }

    void PooledTimer::restart()
    {
        pthread_mutex_lock(&m_fieldGuard);
        timespec t = getTargetTime();
        m_timerPool->startTimer(this, t);
        pthread_mutex_unlock(&m_fieldGuard);
    }

    void PooledTimer::restart(unsigned time)
    {
        start(time);
    }

    timespec PooledTimer::getTargetTime()
    {
        timespec res;
        clock_gettime(CLOCK_REALTIME, &res);
        res.tv_sec += m_time;
        return res;
    }

    void PooledTimer::execute()
    {
        TimerListener* listener = NULL;
        pthread_mutex_lock(&m_fieldGuard);
        listener = m_listener;
        pthread_mutex_unlock(&m_fieldGuard);

        if (listener)
            listener->timerEvent(this);
    }

    TimerPool* TimerPool::instance = new TimerPool(DEFAULT_EXECUTOR_COUNT);

    TimerPool::TimerPool(int initialExecutorCount) : m_executorContainer(initialExecutorCount)
    {
        m_shutdownStarted = false;
    
        {
            pthread_mutexattr_t attr;
            ERROR_CHECK(pthread_mutexattr_init(&attr), 0, "Couldn't init mutex attr");
            ERROR_CHECK(pthread_mutex_init(&m_mainLoopMutex, &attr), 0,
                        "Couldn't init mutex");
            ERROR_CHECK(pthread_mutexattr_destroy(&attr), 0,
                        "Couldn't destroy mutex attr");
            ERROR_CHECK(pthread_cond_init(&m_mainLoopCond, NULL), 0,
                        "Couldn't init conditional variable");
        }

        // starting main loop
        {
            pthread_attr_t attr;
            ERROR_CHECK(pthread_attr_init(&attr), 0, "Couldn't init pthread attr");
            ERROR_CHECK(pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE),
                        0, "Couldn't set joinable pthread attr");
            ERROR_CHECK(pthread_create(&m_mainThread, &attr, &mainLoopStartPoint, (void*)this), 0, "Couln't create thread");
            ERROR_CHECK(pthread_attr_destroy(&attr), 0, "Couldn't destroy thread attributes");
        }
    }

    TimerPool::~TimerPool()
    {
        stopMainLoop();    
    
        pthread_mutex_lock(&m_mainLoopMutex);
        m_executorContainer.shutdown();
        pthread_mutex_unlock(&m_mainLoopMutex);

        pthread_mutex_destroy(&m_mainLoopMutex);
    }

    void TimerPool::stopMainLoop()
    {
        pthread_mutex_lock(&m_mainLoopMutex);
        m_shutdownStarted = true;
        pthread_cond_signal(&m_mainLoopCond);            
        pthread_mutex_unlock(&m_mainLoopMutex);

        pthread_join(m_mainThread, NULL);
    }

    TimerPool* TimerPool::getInstance()
    {
        while (instance == NULL)
            sleep(1); // sleep just in case accessed too fast

        return instance;
    }

    Timer* TimerPool::createTimer()
    {
        PooledTimer* pt = new PooledTimer(this);

        pthread_mutex_lock(&m_mainLoopMutex);
        m_idleTimers.insert(pt);
        pthread_mutex_unlock(&m_mainLoopMutex);
    
        return pt;
    }

    PooledTimer* TimerPool::getFirstOfRunningTimers(timespec* t)
    {
        memset(t, 0, sizeof(timespec));
        if (m_runningTimers.size() == 0)
            return NULL;

        pair<timespec, PooledTimer*> z = *m_runningTimers.begin();
        *t = z.first;

        return z.second;
    }

    bool TimerPool::isRunning(PooledTimer* pt)
    {
        for (RunningTimers::iterator i = m_runningTimers.begin();
             i != m_runningTimers.end(); i++)
            if ((*i).second == pt)
                return true;

        return false;
    }

    bool TimerPool::isRunning(PooledTimer* pt, timespec* t)
    {
        memset(t, 0, sizeof(timespec));
        for (RunningTimers::iterator i = m_runningTimers.begin();
             i != m_runningTimers.end(); i++)
            if ((*i).second == pt)
            {
                *t = (*i).first;
                return true;
            }

        return false;
    }

    void* TimerPool::mainLoopStartPoint(void* arg)
    {
        TimerPool* tp = (TimerPool*)arg;
        tp->mainLoop();
        return NULL;
    }

    void TimerPool::mainLoop()
    {
        pthread_mutex_lock(&m_mainLoopMutex);
        while (!m_shutdownStarted)
        {
            PooledTimer* pt = NULL;
            timespec nextStop;

            while (!m_shutdownStarted && (pt = getFirstOfRunningTimers(&nextStop)) != NULL)
            {            
                if (isTimeReached(nextStop))
                    doExecute(pt, nextStop);
                else break;
            }

            if (!m_shutdownStarted)
            {
                if (pt)
                    pthread_cond_timedwait(&m_mainLoopCond, &m_mainLoopMutex,
                                           &nextStop);
                else
                    pthread_cond_wait(&m_mainLoopCond, &m_mainLoopMutex);
            }
        }
        pthread_mutex_unlock(&m_mainLoopMutex);    
    }

    void TimerPool::startTimer(PooledTimer* pt, timespec targetTime)
    {    
        pthread_mutex_lock(&m_mainLoopMutex);

        timespec t;
        if (isRunning(pt, &t))
        {
            if (!(t.tv_sec == targetTime.tv_sec &&
                  t.tv_nsec == targetTime.tv_nsec))
            {
                m_runningTimers.erase(t);
                m_runningTimers.insert(pair<timespec, PooledTimer*>(targetTime, pt));
                pthread_cond_signal(&m_mainLoopCond);                
            }
        } else
        {
            m_idleTimers.erase(pt);
            m_runningTimers.insert(pair<timespec, PooledTimer*>(targetTime, pt));
            pthread_cond_signal(&m_mainLoopCond);                
        }
            
        pthread_mutex_unlock(&m_mainLoopMutex);
    }

    void TimerPool::stopTimer(PooledTimer* pt)
    {
        Executor* e = NULL;

        pthread_mutex_lock(&m_mainLoopMutex);

        timespec t;
        if (isRunning(pt, &t))
        {
            m_runningTimers.erase(t);
            m_idleTimers.insert(pt);
        
            e = m_executorContainer.getExecutor(pt, true);

            pthread_cond_signal(&m_mainLoopCond);
        }    

        pthread_mutex_unlock(&m_mainLoopMutex);
    
        // we do this outside of main loop mutex, ofc
        if (e)
            e->waitUntilServiced(pt);
    }

    void TimerPool::removeTimer(PooledTimer* pt)
    {
        Executor* e = NULL;
        pthread_mutex_lock(&m_mainLoopMutex);
        timespec t;
        if (isRunning(pt, &t))
            m_runningTimers.erase(t);
        else
            m_idleTimers.erase(pt); 
        // no need to signal according to impl of mainLoop;
        // thread will anyway wake up at worst case before
        // it is needed

        e = m_executorContainer.getExecutor(pt, true);
        
        pthread_mutex_unlock(&m_mainLoopMutex);

        // we do this outside of main loop mutex, ofc
        if (e)
            e->waitUntilServiced(pt);
    }

    bool TimerPool::isTimeReached(timespec& t)
    {
        timespec c;
        memset(&c, 0, sizeof(timespec));    
        clock_gettime(CLOCK_REALTIME, &c);

        return (c.tv_sec > t.tv_sec) ||
            (c.tv_sec == t.tv_sec && c.tv_nsec > t.tv_nsec);
    }

    void TimerPool::doExecute(PooledTimer* pt, timespec t)
    {
        m_runningTimers.erase(t);
        m_idleTimers.insert(pt);
    
        Executor* exe = m_executorContainer.getExecutor(pt);

        // Even if at this moment exe is finished it's work, it should be able
        // to start work with pt; in other case, pt is allready being processed
        // by this exe, so it just increments counter to process it one more
        // time
        exe->tryExecute(pt);
    }

    TimerPool::Executor::Executor(TimerPool::ExecutorContainer* cont) :
        m_executorContainer(cont)
    {
        m_timer = NULL;
        m_doCount = 0;
        m_shutdownStarted = false;

        {
            pthread_mutexattr_t attr;
            ERROR_CHECK(pthread_mutexattr_init(&attr), 0, "Couldn't init mutex attr");
            ERROR_CHECK(pthread_mutex_init(&m_executorMutex, &attr), 0,
                        "Couldn't init mutex");
            ERROR_CHECK(pthread_mutexattr_destroy(&attr), 0,
                        "Couldn't destroy mutex attr");
            ERROR_CHECK(pthread_cond_init(&m_executorCond, NULL), 0,
                        "Couldn't init conditional variable");
        }

        launchThread();
    }

    TimerPool::Executor::~Executor()
    {
        pthread_mutex_lock(&m_executorMutex);
        m_shutdownStarted = true;
        pthread_cond_signal(&m_executorCond);                
        pthread_mutex_unlock(&m_executorMutex);    

        pthread_join(m_executorThread, NULL);
    }

    void* TimerPool::Executor::runEntry(void* arg)
    {
        TimerPool::Executor* e = (TimerPool::Executor*)arg;
        e->run();
        return NULL;
    }

    void TimerPool::Executor::run()
    {
        pthread_cleanup_push(&TimerPool::Executor::onThreadExit, (void*)this);      

        pthread_mutex_lock(&m_executorMutex);
        while (!m_shutdownStarted)
        {        
            while (!m_shutdownStarted && m_timer)
            {
                pthread_mutex_unlock(&m_executorMutex);
                m_timer->execute(); // assuming m_timer won't be changed unless NULL
                pthread_mutex_lock(&m_executorMutex);
                reduceCount();
            }        
        
            if (!m_shutdownStarted)
                pthread_cond_wait(&m_executorCond, &m_executorMutex);        
        }
        pthread_mutex_unlock(&m_executorMutex);

        pthread_cleanup_pop(0);
    }

    bool TimerPool::Executor::tryExecute(PooledTimer* timer)
    {
        bool res = false;
        pthread_mutex_lock(&m_executorMutex);
        if (m_timer == NULL) // m_doCount is allready initialized to 0
            m_timer = timer;

        if (m_timer == timer)
        {
            // notifying only if not running before
            if (!m_doCount)
                m_executorContainer->setRunning(this, timer);
            m_doCount++;
            pthread_cond_signal(&m_executorCond);
            res = true;
        }
        pthread_mutex_unlock(&m_executorMutex);
        return res;
    }

    PooledTimer* TimerPool::Executor::getServicedTimer()
    {
        PooledTimer* t;
        pthread_mutex_lock(&m_executorMutex);    
        t = m_timer;
        pthread_mutex_unlock(&m_executorMutex);

        return t;
    }

    void TimerPool::Executor::waitUntilServiced(PooledTimer* t, bool discardCount)
    {
        pthread_mutex_lock(&m_executorMutex);  

        /*
          If this method is called from executor's thread, we simply set
          m_timer to NULL and exit; otherwise, we wait for executor thread
          to exit (it signals that).
        */    
        if (m_timer == t && pthread_self() == m_executorThread)
        {
            m_executorContainer->setIdle(this, m_timer);
            m_doCount = 0;
            m_timer = NULL;
        }

        while (m_timer == t)
        {
            if (discardCount)
                m_doCount = 0;
        
            pthread_cond_wait(&m_executorCond, &m_executorMutex);
        }
        pthread_mutex_unlock(&m_executorMutex);
    }

    void TimerPool::Executor::reduceCount()
    {
        if (m_timer == NULL)
            return;
    
        m_doCount--;
        if (m_doCount <= 0)
        {
            m_executorContainer->setIdle(this, m_timer);
            m_doCount = 0;
            m_timer = NULL;

            // this is for waitUntilServiced
            pthread_cond_broadcast(&m_executorCond);            
        }
    }

    void TimerPool::Executor::launchThread()    
    { 
        pthread_attr_t attr;
        ERROR_CHECK(pthread_attr_init(&attr), 0, "Couldn't init pthread attr");
        ERROR_CHECK(pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE),
                    0, "Couldn't set joinable pthread attr");
        ERROR_CHECK(pthread_create(&m_executorThread, &attr, &runEntry, (void*)this), 0, "Couln't create thread");
        ERROR_CHECK(pthread_attr_destroy(&attr), 0, "Couldn't destroy thread attributes");   
    }

    void TimerPool::Executor::onThreadExit(void* arg)
    {
        Executor* e = (Executor*)arg;
        e->onThreadExitHandler();
    }

    void TimerPool::Executor::onThreadExitHandler()
    {
        pthread_mutex_lock(&m_executorMutex);
        if (!m_shutdownStarted)
        {
            reduceCount();
            launchThread();
        }    
        pthread_mutex_unlock(&m_executorMutex);
    }

    TimerPool::Executor* TimerPool::ExecutorContainer::getExecutor(PooledTimer* preferredAssociate, bool strict)
    {
        TimerPool::Executor* res = NULL;

        pthread_mutex_lock(&m_fieldGuard);

        map<PooledTimer*, Executor*>::iterator running =
            m_running.find(preferredAssociate);
        if (running != m_running.end())
        {
            res = (*running).second;
        } else if (!strict)   
        {           
            if (m_idle.size() == 0)
            {            
                pushIdle(new TimerPool::Executor(this));
            } 
            res = topIdle();
        }
        pthread_mutex_unlock(&m_fieldGuard);
        return res;
    }

    void TimerPool::ExecutorContainer::setIdle(Executor* executor, PooledTimer* pt)
    {    
        pthread_mutex_lock(&m_fieldGuard);
        // see comment in shutdown()
        if (!m_shutdownStarted)
        {        
            m_running.erase(pt);
            pushIdle(executor);
        }
        pthread_mutex_unlock(&m_fieldGuard);
    }

    void TimerPool::ExecutorContainer::setRunning(Executor* executor, PooledTimer* associate)
    {    
        // see comment in shutdown()
        // note: executor can be in either idle or running container.
        // we move it from idle to running only if it is necessary.
        pthread_mutex_lock(&m_fieldGuard);
        map<PooledTimer*, Executor*>::iterator running =
            m_running.find(associate);
        if (running == m_running.end())
        {
            m_idle.erase(executor);
            m_running.insert(pair<PooledTimer*, Executor*>(associate, executor));   
        }

        pthread_mutex_unlock(&m_fieldGuard);    
    }

    TimerPool::ExecutorContainer::ExecutorContainer(int initialExecutorCount) :
        m_shutdownStarted(false)
    {    
        pthread_mutexattr_t attr;
        ERROR_CHECK(pthread_mutexattr_init(&attr), 0, "Couldn't init mutex attr");
        ERROR_CHECK(pthread_mutex_init(&m_fieldGuard, &attr), 0,
                    "Couldn't init mutex");
        ERROR_CHECK(pthread_mutexattr_destroy(&attr), 0,
                    "Couldn't destroy mutex attr");

        int zz = initialExecutorCount > 0 ? initialExecutorCount : 1;

        for (int i = 0; i < zz; i++)
            pushIdle(new Executor(this));
    }

    TimerPool::ExecutorContainer::~ExecutorContainer()
    {
        pthread_mutex_destroy(&m_fieldGuard);
    }

    void TimerPool::ExecutorContainer::shutdown()
    {
        // We cannot shutdown executor (which can only be done while holding
        // executor's mutex) while fieldGuard is locked,
        // as executor thread is locking those mutexes in opposite order.
        // As at this moment TimerPool main thread should be allready exited
        // (therefore, no new executors can be created) and m_shutdownStarted
        // prevents both executor stl containers from being changed in setIdle(), we 
        // make copy of m_running to shutdown it after m_fieldGuard unlock.
        // m_idle can be shutdown from inside lock, because idle executors aren't
        // using ExecutorContainer.
        // Also, there is no possibility for setRunning to be called (as
        // TimerPool main thread is dead, as mentioned before).
        pthread_mutex_lock(&m_fieldGuard);
        m_shutdownStarted = true;
        Executor* e;
        while ((e = popIdle()) != NULL)
            delete e;

        stack<Executor*> toShutdown;
        for (map<PooledTimer*, Executor*>::iterator i = m_running.begin();
             i != m_running.end(); i++)
            toShutdown.push((*i).second);
    
        pthread_mutex_unlock(&m_fieldGuard);

        while (!toShutdown.empty())
        {
            delete toShutdown.top();
            toShutdown.pop();
        }
    }

    TimerPool::Executor* TimerPool::ExecutorContainer::popIdle()
    {
        if (m_idle.empty())
            return NULL;
        Executor* res = (*m_idle.begin());
        m_idle.erase(m_idle.begin());
        return res;
    }

    TimerPool::Executor* TimerPool::ExecutorContainer::topIdle()
    {
        if (m_idle.empty())
            return NULL;
        Executor* res = (*m_idle.begin());
        return res;
    }

    void TimerPool::ExecutorContainer::pushIdle(Executor* e)
    {
        m_idle.insert(e);
    }

}
