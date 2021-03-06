#include "chatGroupDialog.h"
#include "ui_chatGroupDialog.h"
#include <QMessageBox>
#include "chatItemWidget.h"

ChatGroupDialog::ChatGroupDialog(QWidget *parent, megachat::MegaChatApi *megaChatApi) :
    QDialog(parent),
    ui(new Ui::ChatGroupDialog)
{
    mMainWin = (MainWindow *) parent;
    mMegaChatApi = megaChatApi;
    ui->setupUi(this);
}

ChatGroupDialog::~ChatGroupDialog()
{
    delete ui;
}

void ChatGroupDialog::createChatList(::mega::MegaUserList *contactList)
{
    ::mega::MegaUser *contact = NULL;
    for (int i = 0; i < contactList->size(); i++)
    {
        contact = contactList->get(i);
        const char *contactEmail = contact->getEmail();
        QString peerMail;
        peerMail.append(QString::fromStdString(contactEmail));
        ui->mListWidget->addItem(peerMail);
    }

    QListWidgetItem *item = 0;
    for (int i = 0; i < ui->mListWidget->count(); ++i)
    {
        item = ui->mListWidget->item(i);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(Qt::Unchecked);
    }
    delete contactList;
}

void ChatGroupDialog::on_buttonBox_accepted()
{
    megachat::MegaChatPeerList *peerList = megachat::MegaChatPeerList::createInstance();
    QListWidgetItem *item = 0;
    for (int i = 0; i < ui->mListWidget->count(); ++i)
    {
        item = ui->mListWidget->item(i);
        if (item->checkState() == Qt::Checked)
        {
            megachat::MegaChatHandle userHandle = mMegaChatApi->getUserHandleByEmail(item->text().toStdString().c_str());
            if (userHandle == megachat::MEGACHAT_INVALID_HANDLE)
            {
                QMessageBox::warning(this, tr("Chat creation"), tr("Invalid user handle"));
                return;
            }
            peerList->addPeer(userHandle, 2);
        }
    }

    if (peerList->size() == 0)
    {
        return;
    }

    megachat::MegaChatListItemList *list = mMegaChatApi->getChatListItemsByPeers(peerList);
    if (list->size() != 0)
    {
         QMessageBox msgBoxAns;
         msgBoxAns.setText("You have another chatroom with same participants do you want to reuse it ");
         msgBoxAns.setStandardButtons(QMessageBox::Yes | QMessageBox::No);
         int retVal = msgBoxAns.exec();
         if (retVal == QMessageBox::Yes)
         {
             if (list->get(0)->isArchived())
             {
                 ChatListItemController *chatController = mMainWin->getChatControllerById(list->get(0)->getChatId());
                 if (chatController)
                 {
                    ChatItemWidget *widget = chatController->getWidget();
                    if (widget)
                    {
                        widget->archiveChat(false);
                        QMessageBox::warning(this, tr("Add chatRoom"), tr("You have unarchived a chatroom to reuse it"));
                    }
                 }
             }
             else
             {
                 QMessageBox::warning(this, tr("Add chatRoom"), tr("You have decide to reuse the chatroom"));
             }
         }
         else
         {
             this->mMegaChatApi->createChat(true, peerList);
         }
    }
    else
    {
         mMegaChatApi->createChat(true, peerList);
    }
    delete peerList;
    delete list;
}

void ChatGroupDialog::on_buttonBox_rejected()
{
    close();
}
