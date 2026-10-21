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

// The netplay dialog (System > Multiplayer > Netplay): host or join a session over the internet,
// see who is in it, and (host) start it.

#ifndef NETPLAYCOOPDIALOG_H
#define NETPLAYCOOPDIALOG_H

#include <QDialog>
#include <QString>

class EmuInstance;
class QRadioButton;
class QLineEdit;
class QSpinBox;
class QPushButton;
class QListWidget;
class QLabel;
class QTimer;

class NetplayCoopDialog : public QDialog
{
    Q_OBJECT

public:
    // Command-line netplay (--netplay-host / --netplay-join): connect at once, and as host start
    // once `Players` are in.
    struct AutoStart
    {
        bool Host = true;
        QString Address;
        int Port = 7065;
        QString Name = "PLAYER";
        int Players = 0;
        int Delay = 4;
        QString Bind;
    };

    static void open(QWidget* parent, EmuInstance* inst, const AutoStart* autostart = nullptr);

private slots:
    void onConnect();
    void onStart();
    void onLeave();
    void onTick();
    void onModeChanged();

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    NetplayCoopDialog(QWidget* parent, EmuInstance* inst);
    void setEditable(bool editable);

    EmuInstance* Inst;
    QRadioButton* HostButton;
    QRadioButton* JoinButton;
    QLineEdit* NameEdit;
    QLineEdit* AddressEdit;
    QSpinBox* PortSpin;
    QSpinBox* DelaySpin;
    QPushButton* ConnectButton;
    QPushButton* StartButton;
    QPushButton* LeaveButton;
    QListWidget* Players;
    QLabel* Status;
    QTimer* Timer;
    bool StartSent = false;
    int AutoPlayers = 0;                // host: start once this many are in (0: by hand)
    QString Bind;
};

#endif
