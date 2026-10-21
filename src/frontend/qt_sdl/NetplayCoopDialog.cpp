/*
    Copyright 2016-2025 melonDS team, 2026 KH Days co-op mod

    This file is part of melonDS.

    melonDS is free software: you can redistribute it and/or modify it under
    the terms of the GNU General Public License as published by the Free
    Software Foundation, either version 3 of the License, or (at your option)
    any later version.

    melonDS is distributed in the hope that it will be useful, but WITHOUT ANY
    WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
    FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with melonDS. If not, see http://www.gnu.org/licenses/.
*/

#include "NetplayCoopDialog.h"

#include <QCloseEvent>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QRadioButton>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>

#include "EmuInstance.h"
#include "EmuThread.h"
#include "NetplayController.h"

namespace NP = melonDS::Netplay;

static NetplayCoopDialog* OpenDialog = nullptr;

void NetplayCoopDialog::open(QWidget* parent, EmuInstance* inst, const AutoStart* autostart)
{
    if (OpenDialog)
    {
        OpenDialog->show();
        OpenDialog->raise();
        OpenDialog->activateWindow();
        return;
    }
    OpenDialog = new NetplayCoopDialog(parent, inst);
    OpenDialog->setAttribute(Qt::WA_DeleteOnClose);
    OpenDialog->show();

    if (autostart)
    {
        NetplayCoopDialog* dlg = OpenDialog;
        (autostart->Host ? dlg->HostButton : dlg->JoinButton)->setChecked(true);
        dlg->NameEdit->setText(autostart->Name);
        dlg->AddressEdit->setText(autostart->Address);
        dlg->PortSpin->setValue(autostart->Port);
        dlg->DelaySpin->setValue(autostart->Delay);
        dlg->AutoPlayers = autostart->Host ? autostart->Players : 0;
        dlg->Bind = autostart->Bind;
        // after the ROM has booted
        QTimer::singleShot(1500, dlg, &NetplayCoopDialog::onConnect);
    }
}

NetplayCoopDialog::NetplayCoopDialog(QWidget* parent, EmuInstance* inst) :
    QDialog(parent), Inst(inst)
{
    setWindowTitle("Netplay - melonDS");

    HostButton = new QRadioButton("Host a session");
    JoinButton = new QRadioButton("Join a session");
    HostButton->setChecked(true);
    NameEdit = new QLineEdit("PLAYER");
    NameEdit->setMaxLength(NP::kNameLength);
    AddressEdit = new QLineEdit;
    AddressEdit->setPlaceholderText("the host's address (IP or name)");
    PortSpin = new QSpinBox;
    PortSpin->setRange(1024, 65535);
    PortSpin->setValue(NP::kDefaultPort);
    DelaySpin = new QSpinBox;
    DelaySpin->setRange(1, 15);
    DelaySpin->setValue(4);
    DelaySpin->setSuffix(" frames");
    DelaySpin->setToolTip("How far ahead every player's input is sent. 4 frames covers about 65 ms "
                          "of one-way delay; raise it if the game stutters.");

    auto* form = new QFormLayout;
    auto* modes = new QHBoxLayout;
    modes->addWidget(HostButton);
    modes->addWidget(JoinButton);
    form->addRow(modes);
    form->addRow("Your name:", NameEdit);
    form->addRow("Host address:", AddressEdit);
    form->addRow("UDP port:", PortSpin);
    form->addRow("Input delay:", DelaySpin);

    ConnectButton = new QPushButton("Host");
    StartButton = new QPushButton("Start");
    LeaveButton = new QPushButton("Leave");
    StartButton->setEnabled(false);
    LeaveButton->setEnabled(false);
    auto* buttons = new QHBoxLayout;
    buttons->addWidget(ConnectButton);
    buttons->addWidget(StartButton);
    buttons->addWidget(LeaveButton);

    Players = new QListWidget;
    Status = new QLabel("Load the patched ROM in this window, then host or join. Every player "
                        "needs the same patched ROM and this melonDS build.");
    Status->setWordWrap(true);

    auto* layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addLayout(buttons);
    layout->addWidget(new QLabel("Players:"));
    layout->addWidget(Players);
    layout->addWidget(Status);

    Timer = new QTimer(this);
    connect(Timer, &QTimer::timeout, this, &NetplayCoopDialog::onTick);
    connect(ConnectButton, &QPushButton::clicked, this, &NetplayCoopDialog::onConnect);
    connect(StartButton, &QPushButton::clicked, this, &NetplayCoopDialog::onStart);
    connect(LeaveButton, &QPushButton::clicked, this, &NetplayCoopDialog::onLeave);
    connect(HostButton, &QRadioButton::toggled, this, &NetplayCoopDialog::onModeChanged);
    onModeChanged();

    // a session already running (the dialog was closed and opened again)
    if (NetplayController::Current())
    {
        setEditable(false);
        LeaveButton->setEnabled(true);
        StartSent = NetplayController::Current()->Running();
        Timer->start(30);
    }
}

void NetplayCoopDialog::onModeChanged()
{
    bool host = HostButton->isChecked();
    AddressEdit->setEnabled(!host);
    DelaySpin->setEnabled(host);
    ConnectButton->setText(host ? "Host" : "Join");
}

void NetplayCoopDialog::setEditable(bool editable)
{
    HostButton->setEnabled(editable);
    JoinButton->setEnabled(editable);
    NameEdit->setEnabled(editable);
    AddressEdit->setEnabled(editable && JoinButton->isChecked());
    PortSpin->setEnabled(editable);
    DelaySpin->setEnabled(editable && HostButton->isChecked());
    ConnectButton->setEnabled(editable);
}

void NetplayCoopDialog::onConnect()
{
    std::string error;
    NetplayController* np = NetplayController::Create(Inst, &error);
    if (!np)
    {
        QMessageBox::warning(this, "Netplay", QString::fromStdString(error));
        return;
    }

    std::string name = NameEdit->text().toStdString();
    int port = PortSpin->value();
    bool ok = HostButton->isChecked() ? np->Host(port, name, Bind.toStdString())
                                      : np->Join(AddressEdit->text().trimmed().toStdString(), port, name);
    if (!ok)
    {
        Status->setText(QString::fromStdString("Could not " + std::string(HostButton->isChecked() ? "host" : "join")
                                               + ": " + np->Net().Error()));
        NetplayController::Destroy();
        return;
    }

    StartSent = false;
    setEditable(false);
    LeaveButton->setEnabled(true);
    Timer->start(30);
    onTick();
}

void NetplayCoopDialog::onStart()
{
    NetplayController* np = NetplayController::Current();
    if (!np) return;
    if (!np->StartGame(DelaySpin->value()))
        Status->setText(QString::fromStdString(np->Net().Error()));
    onTick();
}

void NetplayCoopDialog::onLeave()
{
    NetplayController* np = NetplayController::Current();
    if (np && np->Running())
    {
        EmuThread* thread = Inst->getEmuThread();
        thread->sendMessage({.type = EmuThread::msg_NetplayStop, .param = QString("left")});
        thread->waitMessage();
    }
    NetplayController::Destroy();
    Timer->stop();
    Players->clear();
    StartButton->setEnabled(false);
    LeaveButton->setEnabled(false);
    setEditable(true);
    Status->setText("Left the session.");
}

void NetplayCoopDialog::onTick()
{
    NetplayController* np = NetplayController::Current();
    if (!np)
    {
        Timer->stop();
        return;
    }
    NP::Session& net = np->Net();
    if (!StartSent) np->PollLobby();

    Players->clear();
    for (int s = 0; s < NP::kMaxConsoles; s++)
    {
        if (!net.LobbyUsed(s)) continue;
        QString line = QString("P%1  %2").arg(s + 1).arg(QString::fromStdString(net.LobbyName(s)));
        if (s == 0) line += "  (host)";
        if (s == net.LocalSlot()) line += "  - you";
        else if (net.GetPhase() == NP::Session::Phase::Running && !net.Connected(s))
            line += "  - left (can join again)";
        Players->addItem(line);
    }

    switch (net.GetPhase())
    {
    case NP::Session::Phase::Connecting:
        Status->setText("Connecting to the host...");
        break;
    case NP::Session::Phase::Lobby:
        if (net.IsHosting())
            Status->setText(QString("Hosting on UDP port %1. Players on other networks join with your "
                                    "public IP address; your router must forward this UDP port to "
                                    "this PC. Start when everyone is in.").arg(PortSpin->value()));
        else if (net.Rejoined())
            Status->setText("The game is already running: getting it from the host, then you are "
                            "back in your seat...");
        else
            Status->setText("In the lobby. Waiting for the host to start.");
        StartButton->setEnabled(net.IsHosting() && net.LobbyCount() >= 2);
        if (net.IsHosting() && AutoPlayers > 1 && net.LobbyCount() >= AutoPlayers)
        {
            AutoPlayers = 0;
            // the last guest's save follows its WELCOME
            QTimer::singleShot(500, this, &NetplayCoopDialog::onStart);
        }
        break;
    case NP::Session::Phase::Running:
        StartButton->setEnabled(false);
        if (!StartSent)
        {
            StartSent = true;
            Inst->getEmuThread()->sendMessage({.type = EmuThread::msg_NetplayStart});
        }
        Status->setText(QString("Playing: P%1 of %2. Frame %3.%4")
                            .arg(net.LocalSlot() + 1).arg(net.Start().NumPlayers)
                            .arg(np->FrameNumber())
                            .arg(net.Desynced() ? "  Out of sync: repairing..." : ""));
        break;
    case NP::Session::Phase::Ended:
    case NP::Session::Phase::Idle:
        Status->setText(QString::fromStdString("Session ended: " + (net.Error().empty() ? std::string("left") : net.Error())));
        if (!np->Running())
        {
            NetplayController::Destroy();
            Timer->stop();
            StartButton->setEnabled(false);
            LeaveButton->setEnabled(false);
            setEditable(true);
        }
        break;
    }
}

void NetplayCoopDialog::closeEvent(QCloseEvent* event)
{
    // before the game, closing leaves; during it, the session goes on (Leave ends it)
    NetplayController* np = NetplayController::Current();
    if (np && !np->Running() && !StartSent)
        NetplayController::Destroy();
    OpenDialog = nullptr;
    event->accept();
}
