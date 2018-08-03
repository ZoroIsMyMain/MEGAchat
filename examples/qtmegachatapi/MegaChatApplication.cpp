#include "MegaChatApplication.h"
#include "megaLoggerApplication.h"
#include <iostream>
#include <QDir>
#include <QInputDialog>
#include <QMessageBox>
#include <assert.h>
#include <sys/stat.h>
#include "signal.h"

using namespace std;
using namespace mega;
using namespace megachat;

int main(int argc, char **argv)
{
    MegaChatApplication app(argc,argv);
    if (argc > 2)
    {
        QApplication::quit();
    }
    else if (argc == 2)
    {
        std::string chatlink = argv[1];
        app.initAnonymous(chatlink);
    }
    else
    {
        app.readSid();
        app.init();
    }

    return app.exec();
}

MegaChatApplication::MegaChatApplication(int &argc, char **argv) : QApplication(argc, argv)
{
    mAppDir = MegaChatApi::getAppDir();
    configureLogs();

    // Keep the app open until it's explicitly closed
    setQuitOnLastWindowClosed(true);

    mLoginDialog = NULL;
    mSid = NULL;

    // Initialize the SDK and MEGAchat
    mMegaApi = new MegaApi("karere-native", mAppDir.c_str(), "Karere Native");
    mMegaChatApi = new MegaChatApi(mMegaApi);

    // Create delegate listeners
    megaListenerDelegate = new QTMegaListener(mMegaApi, this);
    mMegaApi->addListener(megaListenerDelegate);

    megaChatRequestListenerDelegate = new QTMegaChatRequestListener(mMegaChatApi, this);
    mMegaChatApi->addChatRequestListener(megaChatRequestListenerDelegate);

    megaChatNotificationListenerDelegate = new QTMegaChatNotificationListener(mMegaChatApi, this);
    mMegaChatApi->addChatNotificationListener(megaChatNotificationListenerDelegate);

    // Start GUI
    mMainWin = new MainWindow(0, mLogger, mMegaChatApi, mMegaApi);
}

MegaChatApplication::~MegaChatApplication()
{
    mMegaApi->removeListener(megaListenerDelegate);
    mMegaChatApi->removeChatRequestListener(megaChatRequestListenerDelegate);
    mMegaChatApi->removeChatNotificationListener(megaChatNotificationListenerDelegate);
    delete megaChatNotificationListenerDelegate;
    delete megaChatRequestListenerDelegate;
    delete megaListenerDelegate;

    const QObjectList lstChildren  = mMainWin->children();
    foreach(QObject* pWidget, lstChildren)
    {
        pWidget->deleteLater();
    }

    mMainWin->deleteLater();
    delete mMegaChatApi;
    delete mMegaApi;
    delete mLogger;
    delete [] mSid;
}

void MegaChatApplication::init()
{
    int initState = mMegaChatApi->init(mSid);
    if (!mSid)
    {
        assert(initState == MegaChatApi::INIT_WAITING_NEW_SESSION);
        login();
    }
    else
    {
        assert(initState == MegaChatApi::INIT_OFFLINE_SESSION
               || initState == MegaChatApi::INIT_NO_CACHE);
        mMegaApi->fastLogin(mSid);
    }
}

std::string MegaChatApplication::getChatLink()
{
    bool ok;
    std::string link;
    QString qLink;

    while (1)
    {
        qLink = QInputDialog::getText((QWidget *)this->mMainWin, tr("Anonymous preview mode"),
                tr("Enter the chat link"), QLineEdit::Normal, "", &ok);

        if (ok)
        {
            link = qLink.toStdString();
            if (link.size() > 1)
            {
                return link;
            }
        }
        else
        {
            return std::string();
        }
    }
}

void MegaChatApplication::initAnonymous(std::string chatlink)
{
    delete [] mSid;
    mSid = (char *)chatlink.c_str();
    saveSid(mSid);

    QMessageBox::information(nullptr, tr("Anonymous mode"), tr("Anonymous mode: "));
    int initState = mMegaChatApi->initAnonymous(mSid);
    if (initState == MegaChatApi::INIT_ERROR)
    {
        QApplication::quit();
    }

    mMainWin->setWindowTitle("Anonymous mode");
    if (chatlink.size() > 1)
    {
        mMegaChatApi->loadChatLink(chatlink.c_str());
        mMainWin->show();
    }
    else
    {
        QApplication::quit();
    }
}

void MegaChatApplication::login()
{
    mLoginDialog = new LoginDialog();
    connect(mLoginDialog, SIGNAL(onLoginClicked()), this, SLOT(onLoginClicked()));
    mLoginDialog->show();
}

void MegaChatApplication::onLoginClicked()
{
    QString email = mLoginDialog->getEmail();
    QString password = mLoginDialog->getPassword();
    mLoginDialog->setState(LoginDialog::loggingIn);
    mMegaApi->login(email.toUtf8().constData(), password.toUtf8().constData());
}

void MegaChatApplication::logout()
{
    mMegaApi->logout();
}

void MegaChatApplication::readSid()
{
    char buf[256];
    ifstream sidf(mAppDir + "/sid");
    if (!sidf.fail())
    {
       sidf.getline(buf, 256);
       if (!sidf.fail())
           mSid = strdup(buf);
    }
}

void MegaChatApplication::saveSid(const char* sdkSid)
{
    ofstream osidf(mAppDir + "/sid");
    assert(sdkSid);
    osidf << sdkSid;
    osidf.close();
}

void MegaChatApplication::configureLogs()
{
    std::string logPath = mAppDir + "/log.txt";
    mLogger = new MegaLoggerApplication(logPath.c_str());
    MegaApi::setLogLevel(MegaApi::LOG_LEVEL_DEBUG);
    MegaChatApi::setLogLevel(MegaChatApi::LOG_LEVEL_DEBUG);
    MegaChatApi::setLoggerObject(mLogger);
    MegaChatApi::setLogToConsole(true);
    MegaChatApi::setCatchException(false);
}

void MegaChatApplication::addChats()
{
    mMainWin->updateLocalChatListItems();
    std::list<Chat> *chatList = mMainWin->getLocalChatListItemsByStatus(chatActiveStatus);
    for (Chat &chat : (*chatList))
    {
        const megachat::MegaChatListItem *item = chat.chatItem;
        mMainWin->addChat(item);
    }
    chatList->clear();
    delete chatList;
    mMainWin->addChatListener();
}


void MegaChatApplication::addContacts()
{
    MegaUser * contact = NULL;
    MegaUserList *contactList = mMegaApi->getContacts();

    for (int i=0; i<contactList->size(); i++)
    {
        contact = contactList->get(i);
        const char *contactEmail = contact->getEmail();
        megachat::MegaChatHandle userHandle = mMegaChatApi->getUserHandleByEmail(contactEmail);
        if (megachat::MEGACHAT_INVALID_HANDLE != userHandle)
            mMainWin->addContact(contact);
    }
    delete contactList;
}

void MegaChatApplication::onUsersUpdate(mega::MegaApi * api, mega::MegaUserList * userList)
{
    mega::MegaHandle userHandle = NULL;
    mega:MegaUser *user;

    if(userList)
    {
        for(int i=0; i<userList->size(); i++)
        {
            user = userList->get(i);
            userHandle = userList->get(i)->getHandle();
            std::map<mega::MegaHandle, ContactItemWidget *>::iterator itContacts;
            itContacts = this->mMainWin->contactWidgets.find(userHandle);
            if (itContacts == this->mMainWin->contactWidgets.end())
            {
                mMainWin->addContact(user);
            }
            else
            {
                if (userList->get(i)->hasChanged(MegaUser::CHANGE_TYPE_FIRSTNAME))
                {
                    mMegaChatApi->getUserFirstname(userHandle);
                }
                else if (user->getVisibility() == MegaUser::VISIBILITY_HIDDEN && mMainWin->allItemsVisibility != true)
                {
                    mMainWin->orderContactChatList(mMainWin->allItemsVisibility, mMainWin->archivedItemsVisibility);
                }
            }
        }
    }
}

void MegaChatApplication::onChatNotification(MegaChatApi *, MegaChatHandle chatid, MegaChatMessage *msg)
{
    const char *chat = mMegaApi->userHandleToBase64((MegaHandle)chatid);
    const char *msgid = mMegaApi->userHandleToBase64((MegaHandle)msg->getMsgId());

    string log("Chat notification received in chat [");
    log.append(chat);
    log.append("], msgid: ");
    log.append(msgid);
    mLogger->postLog(log.c_str());

    delete chat;
    delete msgid;
}

void MegaChatApplication::onRequestFinish(MegaApi *api, MegaRequest *request, MegaError *e)
{
    switch (request->getType())
    {
        case MegaRequest::TYPE_LOGIN:
            if (e->getErrorCode() == MegaError::API_OK)
            {
                if (mLoginDialog)
                {
                    mLoginDialog->setState(LoginDialog::fetchingNodes);
                }
                api->fetchNodes();
            }
            else
            {
                if (mLoginDialog)
                {
                    mLoginDialog->setState(LoginDialog::badCredentials);
                    mLoginDialog->enableControls(true);
                }
                else
                {
                    login();
                }
            }
            break;
        case MegaRequest::TYPE_FETCH_NODES:
            if (e->getErrorCode() == MegaError::API_OK)
            {
                delete [] mSid;
                mSid = mMegaApi->dumpSession();
                saveSid(mSid);
                mLoginDialog->deleteLater();
                mLoginDialog = NULL;
                mMainWin->setWindowTitle(api->getMyEmail());
                mMainWin->show();
                addContacts();
                mMegaChatApi->connect();
            }
            else
            {
                QMessageBox::critical(nullptr, tr("Fetch Nodes"), tr("Error Fetching nodes: ").append(e->getErrorString()));
                mLoginDialog->deleteLater();
                mLoginDialog = NULL;
                init();
            }
            break;
        case MegaRequest::TYPE_REMOVE_CONTACT:
            if (e->getErrorCode() != MegaError::API_OK)
                QMessageBox::critical(nullptr, tr("Remove contact"), tr("Error removing contact: ").append(e->getErrorString()));
            break;

        case MegaRequest::TYPE_INVITE_CONTACT:
            if (e->getErrorCode() != MegaError::API_OK)
                QMessageBox::critical(nullptr, tr("Invite contact"), tr("Error inviting contact: ").append(e->getErrorString()));
            break;
    }
}

void MegaChatApplication::onRequestFinish(MegaChatApi *megaChatApi, MegaChatRequest *request, MegaChatError *e)
{
    switch (request->getType())
    {
         case MegaChatRequest::TYPE_CONNECT:
            if (e->getErrorCode() == MegaChatError::ERROR_OK)
            {
                MegaChatHandle myHandle = mMegaChatApi->getMyUserHandle();
                addChats();
                mMainWin->updateToolTipMyInfo(myHandle);
            }
            else
            {
                QMessageBox::critical(nullptr, tr("Chat Connection"), tr("Error stablishing connection").append(e->getErrorString()));
                mLoginDialog->deleteLater();
                mLoginDialog = NULL;
                init();
            }
            break;
         case MegaChatRequest::TYPE_GET_FIRSTNAME:
             {
             MegaChatHandle userHandle = request->getUserHandle();
             int errorCode = e->getErrorCode();
             if (errorCode == MegaChatError::ERROR_OK)
             {
                const char *firstname = request->getText();
                if ((strlen(firstname)) == 0)
                {
                    this->mMegaChatApi->getUserEmail(userHandle);
                    break;
                }
                mMainWin->updateContactFirstname(userHandle,firstname);
                mMainWin->updateMessageFirstname(userHandle,firstname);
             }
             else if (errorCode == MegaChatError::ERROR_NOENT)
             {
                this->mMegaChatApi->getUserEmail(userHandle);
             }
             break;
             }
         case MegaChatRequest::TYPE_GET_EMAIL:
            {
            MegaChatHandle userHandle = request->getUserHandle();
            if (e->getErrorCode() == MegaChatError::ERROR_OK)
            {
               const char *email = request->getText();
               mMainWin->updateContactFirstname(userHandle,email);
               mMainWin->updateMessageFirstname(userHandle,email);
            }
            else
            {
               mMainWin->updateMessageFirstname(userHandle,"Unknown contact");
            }
            break;
            }
         case MegaChatRequest::TYPE_CREATE_CHATROOM:
             if (e->getErrorCode() == MegaChatError::ERROR_OK)
             {
                MegaChatHandle handle = request->getChatHandle();
                const MegaChatListItem *chatListItem = mMainWin->getLocalChatListItem(handle);

                if (chatListItem)
                {
                    std::string title;
                    MegaChatHandle chatid = request->getChatHandle();
                    QString qTitle = QInputDialog::getText(this->mMainWin, tr("Change chat title"), tr("Leave blank for default title"));
                    if (!qTitle.isNull())
                    {
                        title = qTitle.toStdString();
                        if (!title.empty())
                        {
                            this->mMegaChatApi->setChatTitle(handle, title.c_str());
                        }
                    }
                    mMegaChatApi->setChatTitle(chatid, title.c_str());
                    const MegaChatListItem *chatListItem = mMegaChatApi->getChatListItem(chatid);
                    mMainWin->addLocalChatListItem(chatListItem);
                    delete chatListItem;
                    chatListItem = mMainWin->getLocalChatListItem(chatid);
                    mMainWin->addChat(chatListItem);
                }
             }
             break;
         case MegaChatRequest::TYPE_REMOVE_FROM_CHATROOM:
            if (e->getErrorCode() != MegaChatError::ERROR_OK)
                QMessageBox::critical(nullptr, tr("Leave chat"), tr("Error leaving chat: ").append(e->getErrorString()));
            break;
         case MegaChatRequest::TYPE_EDIT_CHATROOM_NAME:
            if (e->getErrorCode() != MegaChatError::ERROR_OK)
                QMessageBox::critical(nullptr, tr("Edit chat topic"), tr("Error modifiying chat topic: ").append(e->getErrorString()));
            break;

        case MegaChatRequest::TYPE_EXPORT_CHAT_LINK:
            if (e->getErrorCode() == MegaChatError::ERROR_OK)
            {
                QMessageBox msg;
                msg.setIcon(QMessageBox::Information);
                msg.setText("The chat link has been generated successfully");
                QString chatlink (request->getText());
                msg.setDetailedText(chatlink);
                msg.exec();
            }
            else
            {
                if(e->getErrorCode() == MegaChatError::ERROR_ARGS)
                {
                    QMessageBox::warning(nullptr, tr("Export chat link"), tr("You need to set a chat title before"));
                }
                else
                {
                    QMessageBox::critical(nullptr, tr("Export chat link"), tr("Error exporting chat link ").append(e->getErrorString()));
                }
            }
            break;

        case MegaChatRequest::TYPE_LOAD_CHAT_LINK:
        {
            MegaChatHandle chatid = request->getChatHandle();
            MegaChatListItem *chatListItem = mMegaChatApi->getChatListItem(chatid);
            if (!chatListItem)
            {
                QMessageBox::critical(nullptr, tr("Export chat link"), tr("Chat Item does not exists"));
            }
            else
            {
                if (mMegaChatApi->anonymousMode())
                {
                    mMainWin->activeControls(false);
                    mMainWin->addLocalChatListItem(chatListItem);
                }
                mMainWin->addChat(chatListItem);
            }
            break;
        }
        case MegaChatRequest::TYPE_CHAT_LINK_CLOSE:
        {
            if(e->getErrorCode() != MegaChatError::ERROR_OK)
            {
                QMessageBox::critical(nullptr, tr("Close chat link"), tr("Error setting chat to private mode ").append(e->getErrorString()));
            }
            else
            {
                QMessageBox::warning(nullptr, tr("Close chat link"), tr("The chat has been converted to private"));
            }
            break;
        }
        case MegaChatRequest::TYPE_CHAT_LINK_REMOVE:
        {
            if(e->getErrorCode() != MegaChatError::ERROR_OK)
            {
                QMessageBox::critical(nullptr, tr("Remove chat link"), tr("Error removing the chat link ").append(e->getErrorString()));
            }
            else
            {
                QMessageBox::warning(nullptr, tr("Remove chat link"), tr("The chat link has been removed"));
            }
            break;
        }
        case MegaChatRequest::TYPE_CHAT_LINK_JOIN:
        {
            if(e->getErrorCode() == MegaChatError::ERROR_OK)
            {
               MegaChatHandle chatHandle = request->getChatHandle();
               ChatItemWidget *item =  mMainWin->getChatItemWidget(chatHandle, false);
               if (item)
               {
                    ChatWindow *chatWin = item->getChatWindow();
                    if(chatWin)
                    {
                        chatWin->close();
                    }

                    mMainWin->updateLocalChatListItems();
                    mMainWin->orderContactChatList(mMainWin->allItemsVisibility, mMainWin->archivedItemsVisibility);
               }
               QMessageBox::warning(nullptr, tr("Join chat link"), tr("You have joined successfully"));
            }
            else
            {
                QMessageBox::critical(nullptr, tr("Join chat link"), tr("Error joining chat link ").append(e->getErrorString()));
            }
            break;
        }
        case MegaChatRequest::TYPE_ARCHIVE_CHATROOM:
            if (e->getErrorCode() != MegaChatError::ERROR_OK)
            {
                QMessageBox::critical(nullptr, tr("Archive chat"), tr("Error archiving chat: ").append(e->getErrorString()));
            }
            else
            {
                 mMainWin->orderContactChatList(mMainWin->allItemsVisibility, mMainWin->archivedItemsVisibility);
            }
            break;

#ifndef KARERE_DISABLE_WEBRTC
         case MegaChatRequest::TYPE_ANSWER_CHAT_CALL:
         case MegaChatRequest::TYPE_START_CHAT_CALL:
            if (e->getErrorCode() != MegaChatError::ERROR_OK)
              {
                QMessageBox::critical(nullptr, tr("Call"), tr("Error in call: ").append(e->getErrorString()));
              }
            else
            {
                megachat::MegaChatHandle chatHandle = request->getChatHandle();
                std::map<megachat::MegaChatHandle, ChatItemWidget *>::iterator itChats;
                itChats = mMainWin->chatWidgets.find(chatHandle);

                if (itChats != mMainWin->chatWidgets.end())
                {
                    ChatItemWidget *chatItemWidget = itChats->second;
                    ChatWindow *chatWin = chatItemWidget->showChatWindow();
                    chatWin->connectCall();
                }
            }
            break;

          case MegaChatRequest::TYPE_HANG_CHAT_CALL:
            if (e->getErrorCode() != MegaChatError::ERROR_OK)
              {
                QMessageBox::critical(nullptr, tr("Call"), tr("Error in call: ").append(e->getErrorString()));
              }
            else
            {
                megachat::MegaChatHandle chatHandle = request->getChatHandle();
                std::map<megachat::MegaChatHandle, ChatItemWidget *>::iterator itChats;
                itChats = mMainWin->chatWidgets.find(chatHandle);

                if (itChats != mMainWin->chatWidgets.end())
                {
                    ChatItemWidget *chatItemWidget = itChats->second;
                    ChatWindow *chatWin = chatItemWidget->showChatWindow();
                    chatWin->hangCall();
                }
            }
            break;

         case MegaChatRequest::TYPE_LOAD_AUDIO_VIDEO_DEVICES:
                mMainWin->createSettingsMenu();
            break;
#endif
    }
}

