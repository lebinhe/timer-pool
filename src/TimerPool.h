/**
Copyright (c) 2013, Pauls Jakovels
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met: 

1. Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer. 
2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution. 

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

The views and conclusions contained in the software and documentation are those
of the authors and should not be interpreted as representing official policies, 
either expressed or implied, of the FreeBSD Project.
**/

#ifndef TIMERPOOL_H
#define TIMERPOOL_H

#include <pthread.h>
#include <stack>
#include <set>
#include <map>
#include <iostream>

using namespace std;

#define TIMER_POOL timer_pool::TimerPool::getInstance()

namespace timer_pool
{
    class TimerListener;

    class Timer
    {
    public:
        virtual ~Timer();
        virtual unsigned getTime() = 0;
        virtual void setListener(TimerListener* new_listener) = 0;
        virtual void start(unsigned time) = 0;
        virtual void stop() = 0;
        virtual void restart() = 0;
        virtual void restart(unsigned time) = 0;
    };

    class TimerListener
    {
    public:
        virtual void timerEvent(Timer* timer) = 0;
    };

    class TimerFactory
    {
    public:
        virtual ~TimerFactory();
        virtual Timer* createTimer() = 0;
    };

    class TimerPool;

    class PooledTimer : public Timer
    {
        friend class TimerPool;
    public:

        // Note: in case when listener is being allready executed,
        // calling thread will join execution thread, to be sure
        // no live thread exists in listener's timer event handler
        // after timer is destroyed.
        virtual ~PooledTimer();
        virtual void setListener(TimerListener* new_listener);
        virtual unsigned getTime();
        virtual void start(unsigned time);
        virtual void stop();
        virtual void restart();
        virtual void restart(unsigned time);
    private:
        PooledTimer(TimerPool* tp);

        TimerPool* m_timerPool;

        void init();
        timespec getTargetTime();

        pthread_mutex_t m_fieldGuard;

        TimerListener* m_listener;
        unsigned int m_time;

        // for calls from CTimerPool
        void execute();
    };

    class TimespecComparator
    {
    public:
        bool operator()(const timespec t, const timespec c)
        {    
            return (c.tv_sec > t.tv_sec) ||
                (c.tv_sec == t.tv_sec && c.tv_nsec > t.tv_nsec);
        }
    };

    /**
       TimerPool manages PooledTimer instances. It is intended as lightweight
       timer factory. 

       Facts:
       - it is guaranteed that timer event will be fired not earlier than specified
       time (according to system timer, ofc)
       - timer pool contains one management thread and at least one worker thread
       (executors)
       - executor count is expanded if needed
       - it is possible to fire many events in row, timer event handler is called
       same times, regardless if in moment of new timer event previous is still
       processing; it is serviced by single thread, so handler shouldn't
       take care of synchronization.
       - when executing timer event handler, executor doesn't hold any locks;
       so, no possible deadlocks (one can restart timer from executor thread)
       - it is possible to kill executor thread, new will be created in place     
       - timer uses just memory and no other resources; no thread is created
       upon construction or stopped upon destruction
       - CTimerPool can be constructed instead of using getInstance() method

       Critique:
       - in case if there is no free executors on timer event, new one should
       be created (which takes some resources); good thing - it shouldn't happen
       too often for normal, non-long-blocking handlers
       - more timers which fires event at same time => each of them will get a bit
       more imprecise
       - when shutting down CTimerPool, thread, which deletes all timers, will
       wait for executors to finish - so, eternal block of executor means
       inability to shut down CTimerPool. See TODOs about this issue.
     
       Conclusion:
       - use this if you need universal, lightweight timer with high level of
       concurrency between timers in same pool
       - in case of blocking timer event handlers it could be slightly better to
       use MCBSingleThreadTimer - because it manages thread count explicitly
       (one timer - one thread; after destruction thread is destroyed as well).
    */
    class TimerPool : public TimerFactory
    {
        friend class PooledTimer;
    public:
        virtual ~TimerPool();

        static TimerPool* getInstance();

        // these give away ownership of Timer
        virtual Timer* createTimer();

        TimerPool(int initialExecutorCount);
    private:

        static TimerPool* instance;

        /**
          Inside TimerPool each timer can be 
          in either idle or running state. TimerPool::startTimer()
          puts timer in running state; TimerPool::stopTimer() puts timer
          in idle state; after executing, timer is put in idle state as well.

          m_runningTimers uses keys which are target time copies
          from pooled timers (see PooledTimer::m_targetTime). 
        */
        typedef map<timespec, PooledTimer*, TimespecComparator> RunningTimers;
        RunningTimers m_runningTimers;
        set<PooledTimer*> m_idleTimers;
        PooledTimer* getFirstOfRunningTimers(timespec* t);
        bool isRunning(PooledTimer* pt);
        bool isRunning(PooledTimer* pt, timespec* t);

        pthread_mutex_t m_mainLoopMutex;
        pthread_cond_t m_mainLoopCond;
        pthread_t m_mainThread;

        class ExecutorContainer;

        class Executor
        {
        public:
            Executor(TimerPool::ExecutorContainer* cont);
            virtual ~Executor();  
            bool tryExecute(PooledTimer* timer);
            PooledTimer* getServicedTimer();

            // waits until m_timer != timer
            // discards m_doCount, if executor works with passed timer and
            // discardCount = true
            void waitUntilServiced(PooledTimer* timer, bool discardCount = true);
        private:
            TimerPool::ExecutorContainer* m_executorContainer;

            pthread_mutex_t m_executorMutex;
            pthread_cond_t m_executorCond;
            pthread_t m_executorThread;

            PooledTimer* m_timer;
            int m_doCount;
            bool m_shutdownStarted;
        
            void launchThread();
            static void onThreadExit(void* arg);
            void onThreadExitHandler();
            static void* runEntry(void* arg);
            void run();
        
            void reduceCount();
        };

        // any executor can be idle or working; executors are owned
        // by instance of this class.
        class ExecutorContainer
        {
        public:

            ExecutorContainer(int initialExecutorCount);
            virtual ~ExecutorContainer();

            void shutdown();

            // returns Executor, which is either idle or running preferredAssociate
            // if strict == true, instead of idle will return null
            TimerPool::Executor* getExecutor(PooledTimer* preferredAssociate, bool strict = false);

            // should be called by Executor, when it becomes idle
            void setIdle(Executor* executor, PooledTimer* pt);

            // this should be called while holding executor mutex
            void setRunning(Executor* executor, PooledTimer* associate);
        
        private:        
            pthread_mutex_t m_fieldGuard;
            bool m_shutdownStarted;

            set<Executor*> m_idle;
            map<PooledTimer*, Executor*> m_running;
            TimerPool::Executor* popIdle();
            TimerPool::Executor* topIdle();
            void pushIdle(Executor* e);
        };
        ExecutorContainer m_executorContainer;

        bool m_shutdownStarted;

        static void* mainLoopStartPoint(void* arg);
        void mainLoop();
        void stopMainLoop();

        static bool isTimeReached(timespec& targetTime);
    
        void doExecute(PooledTimer* pt, timespec t);
   
        // For calls from PooledTimer
        void startTimer(PooledTimer* pt, timespec t);
        void stopTimer(PooledTimer* pt);
        void removeTimer(PooledTimer* pt);
    };

}

#endif

