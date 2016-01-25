#ifndef __CHATD_H__
#define __CHATD_H__

#include <libws.h>
#include <stdint.h>
#include <string>
#include <buffer.h>
#include <map>
#include <set>
#include <list>
#include <deque>
#include <promise.h>
#include <base/timers.h>
#include "base64.h"

#define CHATD_LOG_DEBUG(fmtString,...) KARERE_LOG_DEBUG(krLogChannel_chatd, fmtString, ##__VA_ARGS__)
#define CHATD_LOG_INFO(fmtString,...) KARERE_LOG_INFO(krLogChannel_chatd, fmtString, ##__VA_ARGS__)
#define CHATD_LOG_WARNING(fmtString,...) KARERE_LOG_WARNING(krLogChannel_chatd, fmtString, ##__VA_ARGS__)
#define CHATD_LOG_ERROR(fmtString,...) KARERE_LOG_ERROR(krLogChannel_chatd, fmtString, ##__VA_ARGS__)

namespace chatd
{
// command opcodes
enum Opcode
{
    OP_KEEPALIVE = 0,
    OP_JOIN = 1,
    OP_OLDMSG = 2,
    OP_NEWMSG = 3,
    OP_SEEN = 5,
    OP_RECEIVED = 6,
    OP_RETENTION = 7,
    OP_HIST = 8,
    OP_RANGE = 9,
    OP_MSGID = 10,
    OP_REJECT = 11,
    OP_BROADCAST = 12,
    OP_HISTDONE = 13,
    OP_LAST = OP_HISTDONE
};

// privilege levels
enum Priv
{
    PRIV_NOCHANGE = -2,
    PRIV_NOTPRESENT = -1,
    PRIV_RDONLY = 0,
    PRIV_RDWR = 1,
    PRIV_FULL = 2,
    PRIV_OPER = 3
};

/// This type is used for ordered indexes in the message buffer
typedef int32_t Idx;

/// We want to fit in the positive range of a signed int64_t, for compatibility with sqlite which has no unsigned types
/// Don't use an enum as its implicit type may screw up our value
#define CHATD_IDX_RANGE_MIDDLE 0
#define CHATD_IDX_INVALID 0x7fffffff

class Id
{
public:
    uint64_t val;
    std::string toString() const { return base64urlencode(&val, sizeof(val)); }
    Id(const uint64_t& from=0): val(from){}
    Id(const char* b64, size_t len=0) { base64urldecode(b64, len?len:strlen(b64), &val, sizeof(val)); }
    bool operator==(const Id& other) const { return val == other.val; }
    Id& operator=(const Id& other) { val = other.val; return *this; }
    Id& operator=(const uint64_t& aVal) { val = aVal; return *this; }
    operator const uint64_t&() const { return val; }
    bool operator<(const Id& other) const { return val < other.val; }
    static const Id null() { return static_cast<uint64_t>(0); }
};

class Url
{
protected:
    uint16_t getPortFromProtocol() const;
public:
    std::string protocol;
    std::string host;
    uint16_t port;
    std::string path;
    bool isSecure;
    Url(const std::string& url) { parse(url); }
    Url(): isSecure(false) {}
    void parse(const std::string& url);
    bool isValid() const { return !host.empty(); }
};
class Message: public Buffer
{
public:
    enum Status
    {
        kSending,
        kServerReceived,
        kServerRejected,
        kDelivered,
        kLastOwnMessageStatus = kDelivered, //if a status is <= this, we created the msg, oherwise not
        kNotSeen,
        kSeen
    };
    enum Type: unsigned char
    {
        kTypeInvalid = 255,
        //first are types encoded in the crypto/chatd protocol
        kTypeRegularMessage = 0,
        kTypeEdit = 1,
        kTypeUser = 32,//from this on, till kTypeInternal are types encoded in the payload
        kTypeInternal = 128, //if this flag is set, the message is not passed to the application
        kTypeJoin = kTypeInternal + 0,
        kTypeLeave = kTypeInternal + 1,
        kTypeKeys = kTypeInternal + 2
    };

private:
//avoid setting the id and flag pairs one by one by making them accessible only by setXXX(Id,bool)
    Id mId;
    Id mEdits;
    bool mIdIsXid = false;
    bool mEditsIsXid = false;
public:
    typedef void(*DecryptedCb)(Message&);
    Id userid;
    uint32_t ts;
    bool isEncrypted;
    unsigned char type;
    mutable void* userp;
    DecryptedCb onDecrypted = nullptr;
    const Id& id() const { return mId; }
    const Id& edits() const { return mEdits; }
    bool isSending() const { return mIdIsXid; }
    bool editsIsXid() const { return mEditsIsXid; }
    void setId(const Id& aId, bool isXid) { mId = aId; mIdIsXid = isXid; }
    void setEdits(const Id& aEdits, bool isXid) { mEdits = aEdits; mEditsIsXid = isXid; }
    Message(const Id& aMsgid, const Id& aUserid, uint32_t aTs, Buffer&& buf, bool aEncrypted,
          Type aType=kTypeInvalid, void* aUserp=nullptr, bool aIsSending=false)
        :Buffer(std::forward<Buffer>(buf)), mId(aMsgid), mIdIsXid(aIsSending), userid(aUserid),
          ts(aTs), isEncrypted(aEncrypted), type(aType), userp(aUserp){}
    Message(const Id& aMsgid, const Id& aUserid, uint32_t aTs, const char* msg, size_t msglen,
            bool aEncrypted, Type aType=kTypeInvalid, void* aUserp=nullptr, bool aIsSending=false)
        :Buffer(msg, msglen), mId(aMsgid), mIdIsXid(aIsSending), userid(aUserid), ts(aTs),
            isEncrypted(aEncrypted), type(aType), userp(aUserp) {}
    static const char* statusToStr(unsigned status)
    {
        return (status > kSeen) ? "(invalid status)" : statusNames[status];
    }
protected:
    static const char* statusNames[];
    friend class Messages;
};

class Messages;

enum ChatState
{kChatStateOffline = 0, kChatStateConnecting, kChatStateJoining, kChatStateOnline};
static inline const char* chatStateToStr(unsigned state);

class DbInterface;
class Listener
{
public:
/// This is the first call chatd makes to the Listener, passing it necessary objects and
/// retrieving info about the local history database
/// @param messages - the Messages object that can be used to access the message buffer etc
/// @param out - The interface for sending messages
/// @param[out] oldestDbId, @param[out] newestDbId The range of messages
/// that we have in the local database
/// @param newestDbIdx - the index of the newestDbId message, so that the message buffer indexes
/// will be adjusted to match the ones in the local db
    virtual void init(Messages& messages, DbInterface*& dbIntf) = 0;
/// Called when that chatroom instance is being destroyed (e.g. on application close)
    virtual void onDestroy(){}
/// A new message was received. This can be just sent by a peer, or retrieved from the server.
/// @param idx The index of the message in the buffer @param status - The 'seen' status of the
/// message. Normally it should be 'not seen', until we call setMessageSeen() on it
    virtual void onRecvNewMessage(Idx idx, Message& msg, Message::Status status){}
/// A history message has been received.
/// @param isFromDb The message can be received from the server, or from the app's local
/// history db, via fetchDbHistory() - this parameter specifies the source
    virtual void onRecvHistoryMessage(Idx idx, Message& msg, Message::Status status, bool isFromDb){}
///The retrieval of the requested history batch, via getHistory(), was completed
/// @param isFromDb Whether the history was retrieved from localDb, via fetchDbHistory() or from
/// the server
    virtual void onHistoryDone(bool isFromDb) {}
    virtual void onUnsentMsgLoaded(const Message& msg){}
/// A message sent by us was received acknoledged by the server, assigning it a MSGID.
/// At this stage, the message state is "received-by-server"
/// @param msgxid - The request-response match id that we generated for the sent message. Normally
/// the application doesn't need to care about it
/// @param msgid - The msgid assigned by the server for that message
/// @param idx - The buffer index of the message where the message was put
    virtual void onMessageConfirmed(const Id& msgxid, const Id& msgid, Idx idx){}
/// A message was rejected by the server for some reason. As the message is not yet in the buffer,
/// has no msgid assigned from the server, the only identifier for it is the msgxid
    virtual void onMessageRejected(const Id& msgxid){}
/// A message was delivered, seen, etc. When the seen/received pointers are advanced,
/// this will be called for each message of the pointer-advanced range, so the application
/// doesn't need to iterate over ranges by itself
    virtual void onMessageStatusChange(Idx idx, Message::Status newStatus, const Message& msg){}
/// This method will never be called by chatd itself, as it has no notion about message editing.
/// It should be called by crypto/message packaging filter when it receives a message that is
///an edit of another (earlier) message. This will tell the GUI to replace that message
/// @param oldIdx - the index of the old, edited message, @param newIdx - the index of the new message
/// @param newmsg - The new message
    virtual void onMessageEdited(Idx oldIdx, Idx newIdx, const Message& newmsg){}
/// The chatroom connection (to the chatd server shard) state state has changed.
    virtual void onOnlineStateChange(ChatState state){}
/// A user has joined the room, or their privilege has changed
    virtual void onUserJoined(const Id& userid, char privilege){}
/// A user has left the chatroom
    virtual void onUserLeft(const Id& userid) {}
///Unread message count has changed
    virtual void onUnreadChanged() {}
};
class ICrypto
{
public:
    void init(Messages& messages) {}
    /**
     * @brief encrypt Encrypts a message, putting the contents in the specified buffer
     * @param src The message to encrypt
     * @param dest The destination buffer where to write the encrypted data
     * @return Whether the encryption was successful. In case keys were not available
     * immediately, \c false must be returned. When the encrypt operation will be
     * successful, the crypto module must call Messages::onCanEncryptAgain().
     * This will result in encrypt() for that message called again, and for subsequent
     * messages in the output queue, until the queue is empty, another(or this)
     * \c encrypt() call return false, or the connection goes offline. It is possible
     * that this method is called multiple times for the same message (regardless of
     * its return value) in case the client reconnects. When reconnected, the client
     * re-encrypts and resends all unconfirmed output messages in order.
     */
    virtual bool encrypt(const Message& src, Buffer& dest) { return true; }
/// @brief Called by the client for received messages to decrypt them.
/// The crypto module \b must also set the type of the message, so that the client
/// knows whether to pass it to the application (i.e. contains an actual message)
/// or should not (i.e. contains a crypto system packet)
    virtual promise::Promise<void> decrypt(Message& src, Idx idx)
    { //test implementation
        src.type = Message::kTypeRegularMessage;
        promise::Promise<void> pms;
        mega::setTimeout([pms, &src]() mutable
        {
            src.isEncrypted = false;
            printf("resolving, size = %zu\n", src.dataSize());
            pms.resolve();
        }, 1000);
        return pms;
    }
/// The chatroom connection (to the chatd server shard) state state has changed.
    virtual void onOnlineStateChange(ChatState state){}
/// A user has joined the room, or their privilege has changed
    virtual void onUserJoined(const Id& userid, char privilege){}
/// A user has left the chatroom
    virtual void onUserLeft(const Id& userid) {}
/// @brief Called when a message is received/read that was not passed to \c decrypt().
/// In other words - called only for unencrypted messages, read from history database.
/// In combination with \c decrypt(), the crypto module should receive all messages that
/// the client receives/loads.
    virtual void onMessage(bool isNew, Idx idx, Message& msg, Message::Status status){}
/// History fetch request finished
    virtual void onHistoryDone() {}
///The crypto module is destroyed when that chatid is left or the client is destroyed
    virtual ~ICrypto(){}
};

class Command: public Buffer
{
private:
    Command(const Command&) = delete;
protected:
    static const char* opcodeNames[];
public:
    Command(Command&& other): Buffer(std::forward<Buffer>(other)) {assert(!other.buf() && !other.bufSize() && !other.dataSize());}
    Command(uint8_t opcode): Buffer(64) { write(0, opcode); }
    template<class T>
    Command&& operator+(const T& val)
    {
        append(val);
        return std::move(*this);
    }
    Command&& operator+(const Buffer& msg)
    {
        append<uint32_t>(msg.dataSize());
        append(msg.buf(), msg.dataSize());
        return std::move(*this);
    }
    uint8_t opcode() const { return *reinterpret_cast<uint8_t*>(read(0,1)); }
    const char* opcodeName() const { return opcodeToStr(opcode()); }
    static const char* opcodeToStr(uint8_t code)
    {
        return (code > OP_LAST) ? "(invalid opcpde)" : opcodeNames[code];
    }
};

//for exception message purposes
static inline std::string operator+(const char* str, const Id& id)
{
    std::string result(str);
    result.append(id.toString());
    return result;
}
static inline std::string& operator+(std::string&& str, const Id& id)
{
    str.append(id.toString());
    return str;
}

class Client;

class Connection
{
protected:
    Client& mClient;
    int mShardNo;
    std::set<Id> mChatIds;
    ws_t mWebSocket = nullptr;
    Url mUrl;
    megaHandle mPingTimer = 0;
    bool mTerminating = false;
    std::unique_ptr<promise::Promise<void> > mConnectPromise;
    std::unique_ptr<promise::Promise<void> > mDisconnectPromise;
    Connection(Client& client, int shardNo): mClient(client), mShardNo(shardNo){}
    int getState() { return mWebSocket ? ws_get_state(mWebSocket) : WS_STATE_CLOSED_CLEANLY; }
    bool isOnline() const
    {
        return (mWebSocket && (ws_get_state(mWebSocket) == WS_STATE_CONNECTED));
    }
    static void websockConnectCb(ws_t ws, void* arg);
    static void websockCloseCb(ws_t ws, int errcode, int errtype, const char *reason,
        size_t reason_len, void *arg);
    void onSocketClose();
    promise::Promise<void> reconnect();
    promise::Promise<void> disconnect();
    void reset();
    bool sendCommand(Command&& cmd);
    void rejoinExistingChats();
    void resendPending();
    void join(const Id& chatid);
    void hist(const Id& chatid, long count);
    void execCommand(const StaticBuffer& buf);
    friend class Client;
    friend class Messages;
public:
    ~Connection()
    {
        if (mPingTimer)
            ::mega::cancelInterval(mPingTimer);
    }
};

enum HistFetchState
{
    kHistNotFetching = 0, ///< History is not being fetched, and there is probably history to fetch available
    kHistNoMore = 1, ///< History is not being fetched, and we don't have any more history neither in db nor on server
    kHistFetchingFlag = 2, ///< Set in case we are fetching either from server or db
    kHistFetchingFromServer = 0 | kHistFetchingFlag, ///< We are currently fetching history from server
    kHistFetchingFromDb = 1 | kHistFetchingFlag ///< We are currently fetching history from db
};

typedef std::map<Id,char> UserPrivMap;

// message storage subsystem
// the message buffer can grow in two directions and is always contiguous, i.e. there are no "holes"
// there is no guarantee as to ordering
class Messages
{
protected:
    Connection& mConnection;
    Client& mClient;
    Id mChatId;
    Idx mForwardStart;
    std::vector<Message*> mForwardList;
    std::vector<Message*> mBackwardList;
    struct SendingItem
    {
        Message* msg;
        Message* edit;
        SendingItem(Message* aMsg, Message* aEdit=nullptr): msg(aMsg), edit(aEdit){}
    };
    typedef std::list<SendingItem> OutputQueue;
    OutputQueue mSending;
    OutputQueue::iterator mNextUnsent;
    bool mIsFirstJoin = true;
    std::map<Id, Idx> mIdToIndexMap;
    Id mLastReceivedId;
    Idx mLastReceivedIdx = CHATD_IDX_INVALID;
    Id mLastSeenId;
    Idx mLastSeenIdx = CHATD_IDX_INVALID;
    Listener* mListener;
    ChatState mOnlineState = kChatStateOffline;
    bool mInitialFetchHistoryCalled = false;
//    UserPrivMap mUsers;
    /// User-supplied initial range, that we use until we see the message with mOldestKnownMsgId
    /// Before that happens, missing messages are supposed to be in a database and
    /// incrementally fetched from there as needed. After we see the mOldestKnownMsgId,
    /// we disable this range and recalculate range() only from the buffer items
    Id mOldestKnownMsgId;
    Id mNewestKnownMsgId;
    unsigned mLastHistFetchCount = 0; ///< The number of history messages that have been fetched so far by the currently active or the last history fetch. It is reset upon new history fetch initiation
    HistFetchState mHistFetchState = kHistNotFetching;
    DbInterface* mDbInterface = nullptr;
    ICrypto* mCrypto;
    Messages(Connection& conn, const Id& chatid, Listener* listener, ICrypto* crypto);
    void push_forward(Message* msg) { mForwardList.push_back(msg); }
    void push_back(Message* msg) { mBackwardList.push_back(msg); }
    Message* first() const { return (!mBackwardList.empty()) ? mBackwardList.front() : mForwardList.back(); }
    Message* last() const { return (!mForwardList.empty())? mForwardList.front() : mBackwardList.back(); }
    void clear()
    {
        for (auto& msg: mBackwardList)
            delete msg;
        mBackwardList.clear();
        for (auto& msg: mForwardList)
            delete msg;
        mForwardList.clear();
    }
    // msgid can be 0 in case of rejections
    Idx confirm(const Id& msgxid, const Id& msgid);
    Idx msgIncoming(bool isNew, Message* msg, bool isLocal=false);
    void onUserJoin(const Id& userid, char priv);
    void onJoinComplete();
    void loadAndProcessUnsent();
    void initialFetchHistory(Id serverNewest);
    void requestHistoryFromServer(int32_t count);
    void getHistoryFromDb(unsigned count);
    void onLastReceived(const Id& msgid);
    void onLastSeen(const Id& msgid);
    bool sendCommand(Command&& cmd);
    void join();
    bool msgSend(const Message& message);
    void setOnlineState(ChatState state);
    void enqueueMsgForSend(Message* msg);
    bool flushOutputQueue(bool fromStart=false);
    void range();
    void onHistDone(); //called upont receipt of HISTDONE from server
    friend class Connection;
    friend class Client;
public:
    unsigned initialHistoryFetchCount = 32; //< This is the amount of messages that will be requested from server _only_ in case local db is empty
//    const UserPrivMap& users() const { return mUsers; }
    ~Messages();
    const Id& chatId() const { return mChatId; }
    Client& client() const { return mClient; }
    Idx lownum() const { return mForwardStart - mBackwardList.size(); }
    Idx highnum() const { return mForwardStart + mForwardList.size()-1;}
    Idx size() const { return mForwardList.size() + mBackwardList.size(); }
    bool empty() const { return mForwardList.empty() && mBackwardList.empty();}
    ChatState onlineState() const { return mOnlineState; }
    Message::Status getMsgStatus(Idx idx, const Id& userid);
    Listener* listener() const { return mListener; }
    bool isFetchingHistory() const { return mHistFetchState & kHistFetchingFlag; }
    HistFetchState histFetchState() const { return mHistFetchState; }
    unsigned lastHistFetchCount() const { return mLastHistFetchCount; }
    inline Message* findOrNull(Idx num) const
    {
        if (num < mForwardStart) //look in mBackwardList
        {
            Idx idx = mForwardStart - num - 1; //always >= 0
            if (idx >= mBackwardList.size())
                return nullptr;
            return mBackwardList[idx];
        }
        else
        {
            Idx idx = num - mForwardStart;
            if (idx >= mForwardList.size())
                return nullptr;
            return mForwardList[idx];
        }
    }
    Message& at(Idx num) const
    {
        Message* msg = findOrNull(num);
        if (!msg)
        {
            throw std::runtime_error("Messages::operator[idx]: idx = "+std::to_string(num)+" is outside of [lownum:highnum] range");
        }
        return *msg;
    }

    Message& operator[](Idx num) const { return at(num); }
    bool hasNum(Idx num) const
    {
        if (num < mForwardStart)
            return (mForwardStart - num <= mBackwardList.size());
        else
            return (num < mForwardStart + mForwardList.size());
    }
    Idx msgIndexFromId(const Id& id)
    {
        auto it = mIdToIndexMap.find(id);
        return (it == mIdToIndexMap.end()) ? CHATD_IDX_INVALID : it->second;
    }
    bool getHistory(int count); ///@ returns whether the fetch is from network (true), or database (false), so the app knows whether to display a progress bar/ui or not
    bool setMessageSeen(Idx idx);
    bool historyFetchIsFromDb() const { return (mOldestKnownMsgId != 0); }
    void onCanEncryptAgain() { flushOutputQueue(); }
// Message output methods
    Message* msgSubmit(const char* msg, size_t msglen, Message::Type type, void* userp);
//Queues a message as a edit message for \c orig. \attention Will delete a previous edit if
//the original was not yet ack-ed by the server. That is, there can be only one pending
//edit for a not-yet-sent message, and if there was a previous one, it will be deleted.
//The user is responsible to clear any reference to a previous edit to avoid a dangling pointer.
    Message* msgModify(const Id& oriId, bool isXid, const char* msg, size_t msglen, void* userp, const Id& id=Id::null());
    int unreadMsgCount() const;
    void setListener(Listener* newListener) { mListener = newListener; }
protected:
    void doMsgSubmit(Message* msg);
//===
};

class Client
{
protected:
/// maps the chatd shard number to its corresponding Shard connection
    std::map<int, std::shared_ptr<Connection>> mConnections;
/// maps a chatid to the handling Shard connection
    std::map<Id, Connection*> mConnectionForChatId;
/// maps chatids to the Message object
    std::map<Id, std::shared_ptr<Messages>> mMessagesForChatId;
    Id mUserId;
    Id mMsgTransactionId;
    static bool sWebsockCtxInitialized;
    Connection& chatidConn(const Id& chatid)
    {
        auto it = mConnectionForChatId.find(chatid);
        if (it == mConnectionForChatId.end())
            throw std::runtime_error("chatidConn: Unknown chatid "+chatid);
        return *it->second;
    }
    const Id& nextTransactionId() { mMsgTransactionId.val++; return mMsgTransactionId; }
    void msgConfirm(const Id& msgxid, const Id& msgid);
public:
    static ws_base_s sWebsocketContext;
    size_t pingIntervalSec = 30;
    const Id& userId() const { return mUserId; }
    Client(const Id& userId);
    ~Client(){}
    Messages& chatidMessages(const Id& chatid) const
    {
        auto it = mMessagesForChatId.find(chatid);
        if (it == mMessagesForChatId.end())
            throw std::runtime_error("chatidMessages: Unknown chatid "+chatid);
        return *it->second;
    }

    void join(const Id& chatid, int shardNo, const std::string& url, Listener* listener, ICrypto* crypto);
    void leave(Id chatid);
    friend class Connection;
    friend class Messages;
};

class DbInterface
{
public:
    virtual void getHistoryInfo(Id& oldestDbId, Id& newestDbId, Idx& newestDbIdx) = 0;
    /// Called when the client was requested to fetch history, and it knows the db contains the requested
    /// history range.
    /// @param startIdx - the start index of the requested history range
    /// @param count - the number of messages to return
    /// @param[out] messages - The app should put the messages in this vector, the most recent message being
    /// at position 0 in the vector, and the oldest being the last. If the returned message count is less
    /// than the requested by \c count, the client considers there is no more history in the db. However,
    /// if the application-specified \c oldestDbId in the call to \n init() has not been retrieved yet,
    /// an assertion will be triggered. Therefore, the application must always try to read not less than
    /// \c count messages, in case they are avaialble in the db.
    virtual void fetchDbHistory(Idx startIdx, unsigned count, std::vector<Message*>& messages) = 0;
    virtual void saveMsgToSending(Message& msg) = 0;
    virtual void deleteMsgFromSending(const Id& msgxid) = 0;
    virtual void loadSendingTable(std::vector<Message*>& messages) = 0;
    virtual void addMsgToHistory(const Message& msg, Idx idx) = 0;
    virtual void updateMsgInSending(const Message& data) = 0;
    virtual void updateSendingEditId(const Id& msgxid, const Id& msgid) = 0;
    virtual Idx getIdxOfMsgid(Id msgid) = 0;
    virtual Idx getPeerMsgCountAfterIdx(Idx idx) = 0;
    virtual ~DbInterface(){}
};

static inline const char* chatStateToStr(unsigned state)
{
    static const char* chatStates[] =
    { "Offline", "Connecting", "Joining", "Online"};

    if (state > chatd::kChatStateOnline)
        return "(unkown state)";
    else
        return chatStates[state];
}


}
namespace std
{
    template<>
    struct hash<chatd::Id> { size_t operator()(const chatd::Id& id) const { return hash<uint64_t>()(id.val); } };
}



#endif




