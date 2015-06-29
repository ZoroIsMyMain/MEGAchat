#ifndef RETRYHANDLER_H
#define RETRYHANDLER_H

#include <promise.h>
#include <base/gcm.h>
#include <karereCommon.h>
#include <base/timers.h>

#define RETRY_DEBUG_LOGGING 1

#ifdef RETRY_DEBUG_LOGGING
    #define RETRY_LOG(fmtString,...) KR_LOG_WARNING("Retry: " fmtString, ##__VA_ARGS__)
#else
    #define RETRY_LOG(fmtString,...)
#endif

namespace mega
{
namespace rh
{
/** RetryController states */
typedef enum
{
    kStateNotStarted = 0, /** Not started yet, or just reset. Call start() to run */
    kStateBitRunning = 0x04, /** If this bit is set in a state code, then the controller is in a running state */
    kStateInProgress = 1 | kStateBitRunning, /** An attempt is in progress */
    kStateRetryWait = 2 | kStateBitRunning, /** Sleep before next attempt */
    kStateFinished = 3 /** Completed, output promise has been resolbed. Call reset() to use again */
} State;

/**
 * The type of the promise errors generated by the RetryController. There is only one
 * situation when RetryController generates errors itself - when it is aborted.
 */
enum { kErrorType = 0x2e7294d1 };//should resemble 'retryhdl'

/** Default settings */
enum
{
    kDefaultMaxAttemptCount = 0,
    kDefaultMaxSingleWaitTime = 60000
};

class IRetryController
{
protected:
    State mState = kStateNotStarted;
    size_t mCurrentAttemptNo = 0;
    bool mAutoDestruct = false; //used when we use this object on the heap
public:
    virtual promise::PromiseBase& start(unsigned delay=0) = 0;
    virtual void restart(unsigned delay=0) = 0;
    virtual bool abort() = 0;
    virtual void reset() = 0;
    size_t currentAttemptNo() const { return mCurrentAttemptNo; }
/** Tells the retry handler to delete itself after it has resolved the outupt promise.
 * This is convenient in a fire-and-forget scenario. Typically the user keeps
 * a copy of the output promise, obtained via getPromise(), which keeps the promise
 * alive even of the RetryController object is deleted. See the implementation of
 * the standalone function retry() for an example of that.
 * \warning This can be set only if the instance is allocated on the heap and not
 * on the stack
 */
    void setAutoDestroy() { mAutoDestruct = true; }
/** @brief
 * The state of the retry handler - whether it has not yet been started, is in progress
 * or has finished and the output promise is resolved/rejected.
 */
    State state() const { return mState; }
    virtual ~IRetryController(){};
};
template <typename CB> inline static void callFuncIfNotNull(const CB& cb) { cb(); }
inline static void callFuncIfNotNull(std::nullptr_t){}

/** @brief
 * This is a simple class that retries a promise-returning function call, until the
 * returned promise is resolved (indiating that the operation succeeded), a maximum
 * number of retries has been reached and the retry handler gives up, or it has been
 * canceled by the user. The RetryController
 * has an output promise which is resolved when the operation succeeds, or rejected
 * if the retry handler gives up. That output promise has the same value type as the
 * promise returned by the function. When the function succeeds, the output promise is
 * resolved with the value returned by the function. When the retry handler gives up,
 * it rejects the output promise with the promise::Error object returned by the last
 * (failed) call of the function.
 */
template<class Func, class CancelFunc=void*>
class RetryController: public IRetryController
{
public:
    /** @brief
     * The value type of the promise returned by the operation and by the RetryController
     * itself
     */
    typedef typename decltype(std::declval<Func>().operator()(0))::Type RetType;

protected:
    enum { kBitness = sizeof(unsigned)*8-10 }; //maximum exponent of mInitialWaitTime that fits into an unsigned
    Func mFunc;
    CancelFunc mCancelFunc;
    size_t mCurrentAttemptId = 0; //used to detect callbacks from stale attempts. Never reset (unlike mCurrentAttemptNo)
    size_t mMaxAttemptCount;
    unsigned mAttemptTimeout = 0;
    unsigned mMaxSingleWaitTime;
    unsigned short mDelayRandPct = 20;
    promise::Promise<RetType> mPromise;
    unsigned long mTimer = 0;
    unsigned short mInitialWaitTime;
    unsigned mRestart = 0;
public:
    /** Gets the output promise that is resolved. */
    promise::Promise<RetType>& getPromise() {return mPromise;}
    void setWaitRandomnessPct(unsigned short pct) { mDelayRandPct = pct; }
    /**
     * @param func - The function that does the operation being retried.
     * This can be a lambda, function object or a C funtion pointer. The function
     * must return a promise and take no arguments.
     * @param maxSingleWaitTime - the maximum wait time between retries. The wait time
     * is calculated by multiplying \c backoffStart by 2^(current retry number). If it
     * exceeds maxSingleWaitTime, then it will be set to maxSingleWaitTime.
     * @param maxAttemptCount - the maximum number of retries before giving up. If it
     * is zero, then the retries will be repeated forever.
     * @param backoffStart - the delay before the second retry, which serves as a
     * starting point of the exponential formula. By default it is 1000ms, meaning that
     * the first wait will be 1s the next 2s, then 4s etc. If it is for example 120ms,
     * then the first wait will be 120ms, the next 240ms, then 480ms and so on.
     * This can be used for high frequency initial retrying.
     */
    RetryController(Func&& func, CancelFunc&& cancelFunc, unsigned attemptTimeout,
        unsigned maxSingleWaitTime=kDefaultMaxSingleWaitTime,
        size_t maxAttemptCount=kDefaultMaxAttemptCount, unsigned short backoffStart=1000)
        :mFunc(std::forward<Func>(func)), mCancelFunc(std::forward<CancelFunc>(cancelFunc)),
         mMaxAttemptCount(maxAttemptCount), mAttemptTimeout(attemptTimeout),
         mMaxSingleWaitTime(maxSingleWaitTime),
         mInitialWaitTime(backoffStart)
    {}
    ~RetryController()
    {
        //RETRY_LOG("Deleting RetryController instance");
    }
    /** @brief Starts the retry attempts */
    promise::PromiseBase& start(unsigned delay=0)
    {
        if (mState != kStateNotStarted)
            throw std::runtime_error("RetryController: Already started or not reset after finished");
        assert(mTimer == 0);
        mCurrentAttemptId++;
        mCurrentAttemptNo = 1; //mCurrentAttempt increments immediately before the wait delay (if any)
        if (delay)
        {
            mState = kStateRetryWait;
            mTimer = ::mega::setTimeout([this]()
            {
                mTimer = 0;
                nextTry();
            }, delay);
        }
        else
        {
            nextTry();
        }
        return mPromise;
    }
    /**
     * @brief abort
     * @return whether the abort was actually pefrormed or it
     * was not needed (i.e. not yet started or already finished). When the retries
     * are aborted, the output promise is immediately rejected with an error of type
     * 1 (generic), code 2 (abort) and text "aborted".
     */
    bool abort()
    {
        if ((mState & kStateBitRunning) == 0)
            return false;

        assert(!mPromise.done());
        cancelTimer();
        if ((mState == kStateInProgress) && !std::is_same<CancelFunc, void*>::value)
            callFuncIfNotNull(mCancelFunc);
        mPromise.reject("aborted", promise::kErrAbort, promise::kErrorTypeGeneric);
        if (mAutoDestruct)
            delete this;
        return true;
    }
    /**
     * @brief reset
     * Re-initializes the retry handler after it has already finished. Then it can be
     * reused.
     * \warning After a reset(), the output promise is changed, because a promise cannot
     * be reused, so the user must use the new promise by calling getPromise()
     * after the reset().
     */
    void reset()
    {
        if (mState == kStateNotStarted)
            return;
        else if (mState != kStateFinished)
            throw std::runtime_error("RetryController::reset: Can't reset while in progress");

        assert(mTimer == 0);
        mPromise = promise::Promise<RetType>();
        mCurrentAttemptNo = 0;
        mState = kStateNotStarted;
    }
    /**
     * @brief restart
     * Restarts the attempts with the initial backoff value, i.e. as if the controller was just started,
     * but keeps the current promise object. If the controller has not yet been started, this call is
     * equivalent to start().
     * This method can't be called if the controller is in the \c finished state, in which case an exception will
     * be thrown
     */
    void restart(unsigned delay=0)
    {
        if (mState == kStateFinished)
        {
            throw std::runtime_error("restart: Already in finished state");
        }
        else if (mState == kStateInProgress)
        {
            mRestart = delay ? delay : 1; //schedNextRetry will do the actual restart once the current attempt finishes
        }
        else //kStateRetryWait or kStateNotStarted
        {
            cancelTimer();
            mState = kStateNotStarted;
            start(delay);
        }
    }
protected:
    unsigned calcWaitTime()
    {
        unsigned t = calcWaitTimeNoRandomness();
        unsigned randRange = (t * mDelayRandPct) / 100;
        t = t - randRange + (rand() % 1000) * (randRange * 2) / 1000;
        return t;
    }
    unsigned calcWaitTimeNoRandomness()
    {
        if (mCurrentAttemptNo > kBitness)
        {
            if (!mInitialWaitTime)
                return 0;
            else
                return mMaxSingleWaitTime;
        }
        unsigned t = (1 << (mCurrentAttemptNo-1)) * mInitialWaitTime;
        if (t <= mMaxSingleWaitTime)
            return t;
        else
            return mMaxSingleWaitTime;
    }
    void cancelTimer()
    {
        if (!mTimer)
            return;
        cancelTimeout(mTimer);
        mTimer = 0;
    }
//    template <class P, class=typename std::enable_if<!std::is_same<typename P::Type, void>::value, int>::type>
    template <class P>
    void attachThenHandler(P& promise, unsigned attempt)
    {
        promise.then([this, attempt](const RetType& ret)
        {
            if ((attempt != mCurrentAttemptId) || mPromise.done())
            {
                RETRY_LOG("A previous timed-out/aborted attempt returned success");
                return ret;
            }
            cancelTimer();
            mPromise.resolve(ret);
            mState = kStateFinished;
            if (mAutoDestruct)
                delete this;
            return ret;
        });
    }
//    template <class P, class=typename std::enable_if<std::is_same<typename P::Type, void>::value, int>::type>
    void attachThenHandler(promise::Promise<void>& promise, unsigned attempt)
    {
        promise.then([this, attempt]()
        {
            if ((attempt != mCurrentAttemptId) || mPromise.done())
            {
                RETRY_LOG("A previous timed-out/aborted attempt returned success");
                return;
            }
            cancelTimer();
            mPromise.resolve();
            mState = kStateFinished;
            if (mAutoDestruct)
                delete this;
        });
    }

    void nextTry()
    {
        assert(mTimer == 0);
        assert(!mPromise.done());
        auto attempt = mCurrentAttemptId;
    //set an attempt timeout timer
        if (mAttemptTimeout)
        {
            mTimer = setTimeout([this, attempt]()
            {
                assert(attempt == mCurrentAttemptId); //if we are in a next attempt, cancelTimer() should have been called and this callback should never fire
                assert(!mPromise.done()); //same reason
                mTimer = 0;
                static const promise::Error timeoutError("timeout", promise::kErrTimeout, promise::kErrorTypeGeneric);
                RETRY_LOG("Attempt %zu timed out after %u ms", mCurrentAttemptNo, mAttemptTimeout);
                if (!std::is_same<CancelFunc, std::nullptr_t>::value)
                {
                    auto id = mCurrentAttemptId;
                    callFuncIfNotNull(mCancelFunc);
                    if (id != mCurrentAttemptId) //cancelFunc failed the input promise and a retry was already scheduled as a result, we have to bail out
                        return;
                }
                schedNextRetry(timeoutError);
            }, mAttemptTimeout);
        }
        mState = kStateInProgress;

        auto pms = mFunc(mCurrentAttemptNo);
        attachThenHandler(pms, attempt);
        pms.fail([this, attempt](const promise::Error& err)
        {
            if ((attempt != mCurrentAttemptId) || mPromise.done())//mPromise changed, we already in another attempt and this callback is from the old attempt, ignore it
            {
                RETRY_LOG("A previous timed-out/aborted attempt returned failure: %s", err.msg().c_str());
                return err;
            }
            RETRY_LOG("Attempt %zu failed with message '%s'", mCurrentAttemptNo, err.what());
            cancelTimer();
            schedNextRetry(err);
            return err;
        });
    }
    bool schedNextRetry(const promise::Error& err)
    {
        assert(mTimer == 0);
        if (mRestart)
        {
            auto save = mRestart;
            mRestart = 0;
            mState = kStateNotStarted;
            start(save); //will just schedule, because we pass it a nonzero delay. Handles mCurrentAttemptId/No by itself
            return true;
        }
        mCurrentAttemptNo++; //always increment, to mark the end of the previous attempt
        mCurrentAttemptId++;
        if (mMaxAttemptCount && (mCurrentAttemptNo > mMaxAttemptCount)) //give up
        {
            mPromise.reject(err);
            mState = kStateFinished;
            if (mAutoDestruct)
                delete this;
            return false;
        }

        size_t waitTime = calcWaitTime();
        RETRY_LOG("Will retry in %u ms", waitTime);
        mState = kStateRetryWait;
        //schedule next attempt
        mTimer = setTimeout([this]()
        {
            mTimer = 0;
            nextTry();
        }, waitTime);
        return true;
    }
};
//g++ < 4.9 has a bug where one can't specify a lambda as default function parameter,
//so we define that default func parameter for retry() here
static inline void _emptyCancelFunc(){}
} //end namespace rh


/**
 * Convenience function to retry a lambda call returning a promise.
 * Internally it instantiates a RetryController instance and manages its lifetime
 * (by setting autoDestroy() and making the instance destroy itself after finishing).
 * The paramaters of this function are forwarder to the RetryController constructor.
 * @param The promise-returning (lambda) function to call. This function must take
 * no arguments.
 * @param maxSingleWaitTime - the maximum time in [ms] to wait between attempts. Default is 30 sec
 * @param maxRetries - the maximum number of attempts between giving up and rejecting
 * the returned promise. If it is zero, then it will retry forever. Default is 0
   @param backoffStart - the wait time after the first try, which is also the starting
   point of the backoff time algorithm: \c backoffStart * 2^(current_retry_number).
   See the constructor of RetryController for more details
 */
template <class Func, class CancelFunc=void(*)(void)>
static inline auto retry(Func&& func, CancelFunc&& cancelFunc = rh::_emptyCancelFunc,
    unsigned attemptTimeout = 0,
    size_t maxRetries = rh::kDefaultMaxAttemptCount,
    size_t maxSingleWaitTime = rh::kDefaultMaxSingleWaitTime,
    short backoffStart = 1000)
->decltype(func(0))
{
    auto self = new rh::RetryController<Func, CancelFunc>(
        std::forward<Func>(func), std::forward<CancelFunc>(cancelFunc), attemptTimeout,
        maxSingleWaitTime, maxRetries, backoffStart);
    auto promise = self->getPromise();
    self->setAutoDestroy();
    self->start(); //self may get destroyed synchronously here, but we have a reference to the promise
    return promise;
}

/** Similar to retry(), but returns a heap-allocated RetryController object */
template <class Func, class CancelFunc=void*>
static inline rh::RetryController<Func, CancelFunc>* createRetryController(
    Func&& func,CancelFunc&& cancelFunc = nullptr, unsigned attemptTimeout = 0,
    size_t maxRetries = rh::kDefaultMaxAttemptCount,
    size_t maxSingleWaitTime = rh::kDefaultMaxSingleWaitTime,
    short backoffStart = 1000)
{
    auto retryController = new rh::RetryController<Func, CancelFunc>(std::forward<Func>(func),
        std::forward<CancelFunc>(cancelFunc), attemptTimeout,
        maxSingleWaitTime, maxRetries, backoffStart);
    return retryController;
}

template <class P>
inline void _timeoutAttachThenHandler(P& pms)
{
    pms.then([pms](const typename P::Type& val) mutable
    {
        if (!pms.done())
            pms.resolve(val);
    });
}

inline void _timeoutAttachThenHandler(promise::Promise<void>& pms)
{
    pms.then([pms]() mutable
    {
        if (!pms.done())
            pms.resolve();
    });
}

template <class CB, class CCB=std::nullptr_t>
auto performWithTimeout(CB&& cb, unsigned timeout, CCB&& cancelCb=nullptr)
-> promise::Promise<typename decltype(cb())::Type>
{
    typedef typename decltype(cb())::Type Type;
    promise::Promise<Type> pms;
    ::mega::setTimeout([pms, cancelCb]() mutable
    {
        if (pms.done())
            return;
        pms.reject(promise::Error("Operation timed out", 1, 1));
        if (!std::is_same<CCB, void*>::value)
        {
            rh::callFuncIfNotNull(cancelCb);
        }
    }, timeout);


    auto retpms = cb();
    _timeoutAttachThenHandler(retpms);
    retpms.fail([pms](const promise::Error& err) mutable
    {
        if (!pms.done())
            pms.reject(err);
        return err;
    });
    return pms;
}
}
#endif // RETRYHANDLER_H

