#include "mainwindow.h"
#include "ui_mainwindow.h"
#include "qmessagebox.h"
#include <string>
#include "videoRenderer_Qt.h"
#include "../base/gcm.h"
#include "../IRtcModule.h"
#include "../base/services-dns.hpp"
#include "../base/services-http.hpp"
#include <iostream>
#include <rapidjson/document.h>
#include <sdkApi.h>
#include <ChatClient.h>

#undef emit
#define THROW_IF_FALSE(statement) \
    if (!(statement)) {\
    throw std::runtime_error("'" #statement "' failed (returned false)\n At " __FILE__ ":"+std::to_string(__LINE__)); \
    }

extern MainWindow* mainWin;
extern rtcModule::IRtcModule* rtc;
extern std::string peermail;
extern const std::string jidDomain;
extern std::unique_ptr<MyMegaApi> api;
extern std::unique_ptr<karere::Client> gClient;

using namespace std;
using namespace mega;

MainWindow::MainWindow(QWidget *parent) :
    QMainWindow(parent),
    ui(new Ui::MainWindow)
{
    ui->setupUi(this);
}
mega::http::Client* client = nullptr;

extern bool inCall;
void MainWindow::buttonPushed()
{
/*    if (!client)
        client = new mega::http::Client;
    client->get<std::string>("http://www.osnews.com/")
    .then([](std::shared_ptr<std::string> data)
    {
        cout << "response:" <<endl<<*data<<endl;
        return nullptr;
    })
    .fail([](const promise::Error& err)
    {
        return nullptr;
    });

return;
*/
    for (auto& item: gClient->contactList.contactsFullJid)
        printf("%s: %d\n", item.first.c_str(), item.second);
return;
    if (inCall)
    {
        rtc->hangupAll("hangup", nullptr);
        inCall = false;
        ui->callBtn->setText("Call");
    }
    else
    {
        std::string peerMail = ui->calleeInput->text().toLatin1().data();
        if (peerMail.empty())
        {
            QMessageBox::critical(this, "Error", "Invalid user entered in peer input box");
            return;
        }
        api->call(&MegaApi::getUserData, peerMail.c_str())
        .then([this](ReqResult result)
        {
            const char* peer = result->getText();
            if (!peer)
                throw std::runtime_error("Returned peer user is NULL");

            string peerJid = string(peer)+"@"+KARERE_XMPP_DOMAIN;
            return karere::ChatRoom::create(*gClient, peerJid);
        })
        .then([this](shared_ptr<karere::ChatRoom> room)
        {
            rtcModule::AvFlags av;
            av.audio = true;
            av.video = true;
            char sid[rtcModule::RTCM_SESSIONID_LEN+2];
            gClient->mRtc->startMediaCall(sid, room->peerFullJid().c_str(), av, nullptr);
            inCall = true;
            ui->callBtn->setText("Hangup");
            return nullptr;
        })
        .fail([this](const promise::Error& err)
        {
            if (err.type() == 0x3e9aab10)
                QMessageBox::critical(this, "Error", "Callee user not recognized");
            else
                QMessageBox::critical(this, "Error", QString("Error calling user:")+err.msg().c_str());
            return nullptr;
        });
    }

    /*
    mega::dnsLookup("google.com", 0)
    .then([](std::shared_ptr<mega::AddrInfo> result)
    {
        printf("Canonical name: %s\n", result->canonName().c_str());
        auto& ip4s = result->ip4addrs();
        for (auto& ip: ip4s)
            printf("ipv4: %s\n", ip.toString());
        auto& ip6s = result->ip6addrs();
        for (auto& ip: ip6s)
            printf("ipv6: %s\n", ip.toString());

        return nullptr;
    })
    .fail([](const promise::Error& err)
    {
        printf("DNS lookup error: %s\n", err.msg().c_str());
        return nullptr;
    });
    */
}
void MainWindow::onAudioInSelected()
{
    auto combo = ui->audioInCombo;
    int ret = gClient->mRtc->selectAudioInDevice(combo->itemText(combo->currentIndex()).toAscii().data());
    if (ret < 0)
    {
        QMessageBox::critical(this, "Error", "Selected device not present");
        return;
    }
    printf("selected audio device: %d\n", ret);
}

void MainWindow::onVideoInSelected()
{
    auto combo = ui->videoInCombo;
    int ret = gClient->mRtc->selectVideoInDevice(combo->itemText(combo->currentIndex()).toAscii().data());
    if (ret < 0)
    {
        QMessageBox::critical(this, "Error", "Selected device not present");
        return;
    }
    printf("selected video device: %d\n", ret);
}

MainWindow::~MainWindow()
{
    delete ui;
}

