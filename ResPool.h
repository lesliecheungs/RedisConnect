#ifndef XG_RESPOOL_H
#define XG_RESPOOL_H

#include "typedef.h"

#include <ctime>
#include <mutex>
#include <vector>
#include <string>
#include <memory>
#include <thread>
#include <sstream>
#include <iostream>
#include <iterator>
#include <typeinfo>
#include <algorithm>
#include <functional>

using namespace std;

// 一个资源池，用于管理共享资源的分配和释放。
template<typename T> class ResPool {
    // 一个内部类 Data，用于存储资源的信息，包括资源对象指针、最近一次使用时间、以及资源对象被获取的次数。
    // Data 类的构造函数接受一个共享指针指向的资源对象，并将其存储在内部。
    class Data {
        public:
            int num; // 资源对象被获取的次数
            time_t utime; // 最近一次使用时间
            shared_ptr<T> data; // 资源对象指针

            Data(shared_ptr<T> data) {
                update(data);
            }

            // 函数用于更新数据对象的状态
            // 资源池中，当一个资源对象被重新获取时，需要更新它的使用次数和最近一次使用时间。
            void update(shared_ptr<T> data) {
                this->num = 0;
                this->data = data;
                this->utime = time(NULL);
            }

            // get() 函数用于获取资源对象，并返回一个共享指针。
            // 当一个可用的资源对象被获取时，会调用 Data 类的 get() 函数来更新该资源对象的状态。
            // Data 类的 get() 函数会将该资源对象的使用次数加 1，
            // 并将最近一次使用时间更新为当前时间，并返回该资源对象的共享指针。
            shared_ptr<T> get() {
                utime = time(NULL);
                num++;

                return data;
            }
    };

protected:
    mutex mtx; // 一个互斥量对象，用于同步线程对共享资源的访问。
    int maxlen; // 一个整型变量，表示vec向量的最大容量。
    int timeout; // 一个整型变量，表示等待func函数返回结果的最大超时时间。
    vector<Data> vec; // 一个存储Data对象的向量。
    //一个函数对象，其返回类型为shared_ptr<T>，参数列表为空。
    // 这个函数对象可以存储任何无参函数，并且在需要的时候调用。
    function<shared_ptr<T> ()> func; 

public:
    // 从对象池中获取一个对象的功能
    shared_ptr<T> get() {
        // 首先判断是否设置了超时时间，如果没有则直接调用func()获取一个新的对象，并返回。
        if(timeout <= 0)
            return func();
        
        // 如果设置了超时时间，定义一个名为grasp的lambda函数，并在其中实现具体的获取对象的逻辑
        auto grasp = [&]() {
            int len = 0;
            int idx = -1;
            shared_ptr<T> tmp;
            time_t now = time(NULL);

            // 首先对对象池进行加锁
            mtx.lock();

            len = vec.size();

            // 然后遍历对象池中的所有对象
            for(int i = 0; i < len; i++) {
                Data& item = vec[i];
                // 找到第一个未被占用且未过期的对象，并返回其指针
                // 当 use_count 等于 1 时，说明除了该 Data 对象内部的 shared_ptr 之外，
                // 已经没有其它 shared_ptr 或者 weak_ptr 指向该对象，即该对象没有被占用。
                if(item.data.get() == NULL || item.data.use_count() == 1) {
                    if(tmp = item.data) {
                        if(item.num < 100 && item.utime+timeout > now) {
                            shared_ptr<T> data = item.get();
                            mtx.unlock();
                            return data;
                        }

                        item.data = NULL;
                    }
                    idx = i;
                }
            }

            mtx.unlock();

            //  vector 容器中是否存在空闲的 Data 对象
            if(idx < 0) {
                // 如果已经达到最大长度，则返回一个空的 shared_ptr<T> 对象。
                if(len >= maxlen)
                    return shared_ptr<T>();
                
                // 调用 func 函数创建一个新的 shared_ptr<T> 对象
                shared_ptr<T> data = func();

                // 表示创建对象失败，此时返回空的 shared_ptr。
                if(data.get() == NULL)
                    return data;
                
                mtx.lock();

                if(vec.size() < maxlen)
                    vec.push_back(data);
                mtx.unlock();

                return data;
            }

            shared_ptr<T> data = func();

            if(data.get() == NULL)
                return data;
            
            mtx.lock();

			vec[idx].update(data);

			mtx.unlock();

			return data;
        };

        shared_ptr<T> data = grasp();

        if(data)
            return data;
        
        time_t endtime = time(NULL)+3;
        
        while(true) {
            Sleep(10);
            if(data = grasp())
                return data;
            if(endtime < time(NULL))
                break;
        }

        return data;
    }


    void clear() {
        // lock_guard只有构造函数和析构函数, 
        // 在定义该局部对象的时候加锁（调用构造函数），出了该对象作用域的时候解锁（调用析构函数）
        lock_guard<mutex> lk(mtx);

        vec.clear();
    }

    int getLength() const {
        return maxlen;
    }

    int getTimeout() const {
        return timeout;
    }

    // 用于手动禁用一个 Data 对象
    void disable(shared_ptr<T> data) {
        lock_guard<mutex> lk(mtx);

        for(Data& item: vec) {
            if(data == item.data) {
                item.data = NULL;
                break;
            }
        }
    }

    void setLength(int maxlen) {
		lock_guard<mutex> lk(mtx);

		this->maxlen = maxlen;

		if (vec.size() > maxlen) 
            vec.clear();
	}

    void setTimeout(int timeout) {
		lock_guard<mutex> lk(mtx);

		this->timeout = timeout;

		if (timeout <= 0) 
            vec.clear();
	}

    // 设置一个能够创建T类型对象的函数
    void setCreator(function<shared_ptr<T>()> func) {
		lock_guard<mutex> lk(mtx);

		this->func = func;
		this->vec.clear();
	}

	ResPool(int maxlen = 8, int timeout = 60) {
		this->timeout = timeout;
		this->maxlen = maxlen;
	}

	ResPool(function<shared_ptr<T>()> func, int maxlen = 8, int timeout = 60) {
		this->timeout = timeout;
		this->maxlen = maxlen;
		this->func = func;
	}
};

#endif