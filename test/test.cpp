#include <iostream>
#include <sstream>
#include "../src/TimerPool.h"

using namespace std;
using namespace timer_pool;

pthread_mutex_t cout_guard;

class TestListener : public TimerListener
{
public:
    TestListener(string name) : name(name) {
    }
    virtual void timerEvent(Timer* pTimer)
    {
        pthread_mutex_lock(&cout_guard);
        cout << name << endl;
        pthread_mutex_unlock(&cout_guard);
        pTimer->restart();
    }
private:
    string name;
};


int main() {
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutex_init(&cout_guard, &attr);
    pthread_mutexattr_destroy(&attr);

    for (int i = 0; i < 5; i++) {
        stringstream ss;
        ss << "listener " << i;
        TestListener* tl = new TestListener(ss.str());  
        Timer* pt = TIMER_POOL->createTimer();
        pt->setListener(tl);
        pt->start(1);
    }

    sleep(10000); // we don't care about freeing memory
}
