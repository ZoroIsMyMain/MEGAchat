#include <iostream>
#include "chatWindow.h"
#include "assert.h"
#include <QMenu>
#include "MainWindow.h"

ChatWindow::ChatWindow(QWidget* parent, megachat::MegaChatApi* megaChatApi, megachat::MegaChatRoom *cRoom, const char * title)
    : QDialog(0),
      ui(new Ui::ChatWindowUi)
{
    nSending = 0;
    loadedMessages = 0;
    nManualSending = 0;
    mPendingLoad = 0;
    mChatRoom = cRoom;
    mMegaChatApi = megaChatApi;
    mChatItemWidget = (ChatItemWidget *) parent;
    mMegaApi = mChatItemWidget->mMegaApi;
    mLogger = ((MainWindow *)mChatItemWidget->parent())->mLogger;

    ui->setupUi(this);
    ui->mSplitter->setStretchFactor(0,1);
    ui->mSplitter->setStretchFactor(1,0);
    ui->mMessageList->setContextMenuPolicy(Qt::CustomContextMenu);
    ui->mTitleLabel->setText(title);
    ui->mChatdStatusDisplay->hide();
    connect(ui->mMsgSendBtn,  SIGNAL(clicked()), this, SLOT(onMsgSendBtn()));
    connect(ui->mMessageEdit, SIGNAL(sendMsg()), this, SLOT(onMsgSendBtn()));
    connect(ui->mMessageEdit, SIGNAL(editLastMsg()), this, SLOT(editLastMsg()));
    connect(ui->mMessageList, SIGNAL(requestHistory()), this, SLOT(onMsgListRequestHistory()));
    connect(ui->mMembersBtn,  SIGNAL(clicked(bool)), this, SLOT(onMembersBtn(bool)));
    connect(ui->mMessageList->verticalScrollBar(), SIGNAL(valueChanged(int)), this, SLOT(onScroll(int)));

    #ifndef KARERE_DISABLE_WEBRTC
        connect(ui->mVideoCallBtn, SIGNAL(clicked(bool)), this, SLOT(onVideoCallBtn(bool)));
        connect(ui->mAudioCallBtn, SIGNAL(clicked(bool)), this, SLOT(onAudioCallBtn(bool)));
    #else
        ui->mAudioCallBtn->hide();
        ui->mVideoCallBtn->hide();
    #endif

    if (!mChatRoom->isGroup())
        ui->mMembersBtn->hide();
    else
        setAcceptDrops(true);

    setWindowFlags(Qt::Window | Qt::WindowSystemMenuHint | Qt::WindowMinimizeButtonHint | Qt::WindowCloseButtonHint);
    setAttribute(Qt::WA_DeleteOnClose);

    if (!mChatRoom->isActive())
        ui->mMessageEdit->setEnabled(false);

    QDialog::show();
    this->megaChatRoomListenerDelegate =  new ::megachat::QTMegaChatRoomListener(megaChatApi, this);
}

void ChatWindow::openChatRoom()
{
    bool result = this->mMegaChatApi->openChatRoom(mChatRoom->getChatId(),megaChatRoomListenerDelegate);
    if(result)
    {
        mMegaChatApi->loadMessages(mChatRoom->getChatId(),NMESSAGES_LOAD);
    }
}

ChatWindow::~ChatWindow()
{
    mMegaChatApi->closeChatRoom(mChatRoom->getChatId(),megaChatRoomListenerDelegate);
    mChatItemWidget->invalidChatWindowHandle();
    delete megaChatRoomListenerDelegate;
    delete mChatRoom;
    delete ui;
}

void ChatWindow::onMsgSendBtn()
{
    QString qtext = ui->mMessageEdit->toPlainText();
    ui->mMessageEdit->setText(QString());
    if (qtext.isEmpty())
        return;

    std::string msgStd = qtext.toUtf8().toStdString();
    megachat::MegaChatMessage *tempMessage= mMegaChatApi->sendMessage(mChatRoom->getChatId(), msgStd.c_str());
    nSending++;
    addMsgWidget(tempMessage, loadedMessages + nSending);
}

void ChatWindow::moveManualSendingToSending(megachat::MegaChatMessage * msg)
{
    nSending++;
    nManualSending--;
    addMsgWidget(msg, loadedMessages + nSending);
}

void ChatWindow::onChatRoomUpdate(megachat::MegaChatApi* api, megachat::MegaChatRoom *chat)
{
    // TODO: update UI based on changes reported by `chat`: title, chatd connection, peers names,
    // own privilege...
}

void ChatWindow::onMessageUpdate(megachat::MegaChatApi* api, megachat::MegaChatMessage *msg)
{
    if(msg->isDeleted())
    {
        eraseChatMessage(msg, false);
        return;
    }

    megachat::MegaChatHandle msgId = getMessageId(msg);
    ChatMessage *chatMessage = findChatMessage(msgId);

    if (msg->hasChanged(megachat::MegaChatMessage::CHANGE_TYPE_CONTENT))
    {
        if (chatMessage)
        {
            chatMessage->setMessageContent(msg->getContent());
            if (msg->isEdited())
                chatMessage->markAsEdited();
        }
    }

    if (msg->hasChanged(megachat::MegaChatMessage::CHANGE_TYPE_STATUS))
    {
        if (msg->getStatus() == megachat::MegaChatMessage::STATUS_SERVER_RECEIVED)
        {
            eraseChatMessage(msg, true);
            nSending--;
            megachat::MegaChatMessage *auxMsg = msg->copy();
            addMsgWidget(auxMsg, loadedMessages);
            mChatItemWidget->setOlderMessageLoaded(msg->getMsgId());
            loadedMessages++;
        }
        else
        {
            if(msg->getStatus() == megachat::MegaChatMessage::STATUS_SENDING_MANUAL)
            {
                // TODO: Complete case
            }
            else
            {
                megachat::MegaChatHandle msgId = getMessageId(msg);
                ChatMessage *chatMessage = findChatMessage(msgId);
                if (chatMessage)
                    chatMessage->setStatus(msg->getStatus());
            }
        }
     }
}

void ChatWindow::deleteChatMessage(megachat::MegaChatMessage *msg)
{
    megachat::MegaChatHandle msgId = getMessageId(msg);
    std::map<megachat::MegaChatHandle, ChatMessage *>::iterator itMessages;
    itMessages = mMsgsWidgetsMap.find(msgId);
    if (itMessages != mMsgsWidgetsMap.end())
    {
        mMegaChatApi->deleteMessage(mChatRoom->getChatId(), msgId);
    }
}

bool ChatWindow::eraseChatMessage(megachat::MegaChatMessage *msg, bool temporal)
{
    megachat::MegaChatHandle msgId = temporal ? msg->getTempId(): msg->getMsgId();

    std::map<megachat::MegaChatHandle, ChatMessage *>::iterator itMessages;
    itMessages = mMsgsWidgetsMap.find(msgId);
    if (itMessages != mMsgsWidgetsMap.end())
    {
        ChatMessage * auxMessage = itMessages->second;
        int row = ui->mMessageList->row(auxMessage->getWidgetItem());
        QListWidgetItem * auxItem = ui->mMessageList->takeItem(row);
        mMsgsWidgetsMap.erase(itMessages);
        delete auxItem;
        return true;
    }
    return false;
}

ChatMessage * ChatWindow::findChatMessage(megachat::MegaChatHandle msgId)
{
    std::map<megachat::MegaChatHandle, ChatMessage *>::iterator itMessages;
    itMessages = mMsgsWidgetsMap.find(msgId);
    if (itMessages != mMsgsWidgetsMap.end())
    {
        return itMessages->second;
    }
    return NULL;
}

megachat::MegaChatHandle ChatWindow::getMessageId(megachat::MegaChatMessage *msg)
{
    if (msg->getStatus()== megachat::MegaChatMessage::STATUS_SENDING)
    {
        return msg->getTempId();
    }
    else if (msg->getStatus() == megachat::MegaChatMessage::STATUS_SENDING_MANUAL)
    {
        return msg->getRowId();
    }
    else
    {
        return msg->getMsgId();
    }

    return megachat::MEGACHAT_INVALID_HANDLE;
}

void ChatWindow::onMessageReceived(megachat::MegaChatApi* api, megachat::MegaChatMessage *msg)
{
    mChatItemWidget->setOlderMessageLoaded(msg->getMsgId());
    addMsgWidget(msg->copy(), loadedMessages);
    loadedMessages++;
}

void ChatWindow::onMessageLoaded(megachat::MegaChatApi* api, megachat::MegaChatMessage *msg)
{
    if(msg)
    {
        if (loadedMessages == 0)
        {
            mChatItemWidget->setOlderMessageLoaded(msg->getMsgId());
        }

        if(msg->isDeleted())
            return;

        if (msg->getStatus() == megachat::MegaChatMessage::STATUS_SENDING)
        {
            nSending++;
            addMsgWidget(msg->copy(), loadedMessages + nSending);
        }
        else
        {
            if(msg->getStatus() == megachat::MegaChatMessage::STATUS_SENDING_MANUAL)
            {
                nManualSending++;
                ChatMessage *auxMessage = findChatMessage(msg->getRowId());
                if (auxMessage)
                {
                    eraseChatMessage(msg, true);
                    nSending--;
                }

                addMsgWidget(msg, loadedMessages + nSending + nManualSending);
                auxMessage = findChatMessage(msg->getTempId());

                if(auxMessage)
                {
                    auxMessage->setMessage(msg->copy());
                    auxMessage->setManualMode(true);
                }
            }
            else
            {
                addMsgWidget(msg->copy(), -loadedMessages);
                loadedMessages++;
            }
        }
    }
    else
    {


        if (!mMegaChatApi->isFullHistoryLoaded(mChatRoom->getChatId()) && mPendingLoad > 0)
        {
            mPendingLoad = NMESSAGES_LOAD - loadedMessages + nSending + nManualSending;
            int source = mMegaChatApi->loadMessages(mChatRoom->getChatId(),mPendingLoad);
            if (source == megachat::MegaChatApi::SOURCE_NONE)
            {
                mPendingLoad = 0;
            }
            else if (source == megachat::MegaChatApi::SOURCE_ERROR)
            {
                mPendingLoad = 0;
                mLogger->postLog("MegachatApi error - Load messages - source error");
            }
        }
    }
}


void ChatWindow::setMessageHeight(megachat::MegaChatMessage * msg, QListWidgetItem* item)
{
    switch (msg->getType())
    {
        case megachat::MegaChatMessage::TYPE_NODE_ATTACHMENT:
        {
        item->setSizeHint(QSize(item->sizeHint().height(), 150));
        break;
        }

        case megachat::MegaChatMessage::TYPE_CONTACT_ATTACHMENT:
        {
        item->setSizeHint(QSize(item->sizeHint().height(), 150));
        break;
        }
    }
}

QListWidgetItem* ChatWindow::addMsgWidget(megachat::MegaChatMessage * msg, int index)
{
    QListWidgetItem* item = new QListWidgetItem;
    megachat::MegaChatHandle chatId = mChatRoom->getChatId();
    ChatMessage * widget = new ChatMessage(this, mMegaChatApi, chatId, msg);
    widget->setWidgetItem(item);
    item->setSizeHint(widget->size());
    setMessageHeight(msg,item);

    megachat::MegaChatHandle msgId = getMessageId(msg);
    mMsgsWidgetsMap.insert(std::pair<megachat::MegaChatHandle, ChatMessage *>(msgId, widget));

    ui->mMessageList->insertItem(index, item);
    ui->mMessageList->setItemWidget(item, widget);
    ui->mMessageList->scrollToBottom();

    if (!widget->isMine() && msg->getStatus() == megachat::MegaChatMessage::STATUS_NOT_SEEN)
        mMegaChatApi->setMessageSeen(mChatRoom->getChatId(), msg->getMsgId());

    return item;
}

void ChatWindow::onMembersBtn(bool)
{
    if(mChatRoom->isGroup())
    {
        QMenu menu(this);
        createMembersMenu(menu);
        menu.setLayoutDirection(Qt::RightToLeft);
        menu.adjustSize();
        menu.exec(ui->mMembersBtn->mapToGlobal(
            QPoint(-menu.width()+ui->mMembersBtn->width(), ui->mMembersBtn->height())));
        menu.deleteLater();
    }
}

void ChatWindow::createMembersMenu(QMenu& menu)
{
    MainWindow *mainWin = mChatItemWidget->mMainWin;
    mega::MegaUserList *userList = mainWin->getUserContactList();
    if (mChatRoom->getPeerCount() == 0)
    {
        menu.addAction(tr("You are alone in this chatroom"))->setEnabled(false);
        return;
    }

    if(mChatRoom->getOwnPrivilege() == megachat::MegaChatRoom::PRIV_MODERATOR)
    {
        auto addEntry = menu.addMenu("Add contact to chat");
        for (int i = 0 ; i < userList->size(); i++)
        {
             auto actAdd = addEntry->addAction(tr(userList->get(i)->getEmail()));
             actAdd->setProperty("userHandle", QVariant((qulonglong)userList->get(i)->getHandle()));
             connect(actAdd, SIGNAL(triggered()), this, SLOT(onMemberAdd()));
        }
        delete userList;
    }
    else
    {
        menu.addMenu("Invalid permissions to add participants");
    }

    for (int i = 0; i < mChatRoom->getPeerCount(); i++)
    {
        QVariant userhandle = QVariant((qulonglong)mChatRoom->getPeerHandle(i));

        if(mChatRoom->getOwnPrivilege()== megachat::MegaChatRoom::PRIV_MODERATOR)
        {
            auto entry = menu.addMenu(mChatRoom->getPeerFirstname(i));

            auto actRemove = entry->addAction(tr("Remove from chat"));
            actRemove->setProperty("userHandle", userhandle);
            connect(actRemove, SIGNAL(triggered()), this, SLOT(onMemberRemove()));

            auto menuSetPriv = entry->addMenu(tr("Set privilege"));
            auto actSetPrivFullAccess = menuSetPriv->addAction(tr("Moderator"));
            actSetPrivFullAccess->setProperty("userHandle", userhandle);
            connect(actSetPrivFullAccess, SIGNAL(triggered()), this, SLOT(onMemberSetPriv()));
            auto actSetPrivReadOnly = menuSetPriv->addAction(tr("Read-only"));
            actSetPrivReadOnly->setProperty("userHandle", userhandle);
            connect(actSetPrivReadOnly, SIGNAL(triggered()), this, SLOT(onMemberSetPriv()));
            auto actSetPrivStandard = menuSetPriv->addAction(tr("Standard"));
            actSetPrivStandard->setProperty("userHandle", userhandle);
            connect(actSetPrivStandard, SIGNAL(triggered()), this, SLOT(onMemberSetPriv()));
        }
    }
}

void ChatWindow::onMemberAdd()
{
    QAction *action = qobject_cast<QAction *>(sender());
    if (!action)
       return;

    QVariant uHandle = action->property("userHandle");
    megachat::MegaChatHandle userhandle = uHandle.toLongLong();
    mMegaChatApi->inviteToChat(mChatRoom->getChatId(), userhandle, megachat::MegaChatPeerList::PRIV_STANDARD);
}

void ChatWindow::onMemberRemove()
{
    QAction *action = qobject_cast<QAction *>(sender());
    if (!action)
       return;

    QVariant uHandle = action->property("userHandle");
    megachat::MegaChatHandle userhandle = uHandle.toLongLong();
    mMegaChatApi->removeFromChat(mChatRoom->getChatId(), userhandle);
}

void ChatWindow::onMemberSetPriv()
{
      int privilege;
      QAction *action = qobject_cast<QAction *>(sender());
      if (!action)
         return;

      if(action->text() == "Read-only")
            privilege = megachat::MegaChatPeerList::PRIV_RO;

      if(action->text() == "Moderator")
            privilege = megachat::MegaChatPeerList::PRIV_MODERATOR;

      if(action->text() == "Standard")
            privilege = megachat::MegaChatPeerList::PRIV_STANDARD;

      QVariant uHandle = action->property("userHandle");
      megachat::MegaChatHandle userhandle = uHandle.toLongLong();
      megachat::MegaChatHandle chatId = mChatItemWidget->getChatId();
      this->mMegaChatApi->updateChatPermissions(chatId, userhandle, privilege);
}

void ChatWindow::onMsgListRequestHistory()
{
    if (!mMegaChatApi->isFullHistoryLoaded(mChatRoom->getChatId()))
    {
        mMegaChatApi->loadMessages(mChatRoom->getChatId(), NMESSAGES_LOAD);
    }
}
