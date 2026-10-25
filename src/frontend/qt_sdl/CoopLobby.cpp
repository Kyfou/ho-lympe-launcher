/*
    HeartGold Coop - launch screen: play alone, host a game or join a friend.
*/

#include "HGCoop.h"
#include "CoopLobby.h"
#include "GameUi.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDialog>
#include <QHBoxLayout>
#include <QHostAddress>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QNetworkInterface>
#include <QPushButton>
#include <QSettings>
#include <QShortcut>
#include <QStackedWidget>
#include <QVBoxLayout>

namespace
{

const char* kStyle = R"(
QDialog { background: qlineargradient(x1:0, y1:0, x2:1, y2:1, stop:0 #c0601c, stop:1 #3a120c); }
QLabel { color: #fff3dc; font-family: 'Segoe UI'; }
QLabel#title { font-size: 44px; font-weight: 700; font-style: italic; color: #ffd76a; }
QLabel#subtitle { font-size: 18px; color: #ffe9c4; }
QLabel#address { font-size: 34px; font-weight: 700; color: #ffffff; background: rgba(0,0,0,70);
                 border-radius: 18px; padding: 14px 24px; }
QLabel#hint { font-size: 15px; color: #f2d6b0; }
QLabel#error { font-size: 15px; font-weight: 600; color: #ffffff; background: rgba(200,40,40,170);
               border-radius: 10px; padding: 6px 12px; }
QPushButton { font-family: 'Segoe UI'; font-size: 22px; font-weight: 600; color: #4a2410;
              background: #fff6e6; border: none; border-radius: 26px; padding: 16px 30px; }
QPushButton:hover, QPushButton:focus { background: #ffffff; border: 4px solid #f6c446; }
QPushButton#primary { background: #3fa45a; color: white; }
QPushButton#primary:hover, QPushButton#primary:focus { background: #4cbf6a; border: 4px solid #ffffff; }
QPushButton#back { background: rgba(255,255,255,40); color: #fff3dc; font-size: 18px; padding: 10px 24px; }
QLineEdit { font-family: 'Segoe UI'; font-size: 28px; padding: 12px 18px; border-radius: 18px;
            border: 3px solid #f6c446; background: #fffaf0; color: #3a1a0c; }
)";

// The addresses a friend can use to reach this PC (Tailscale first: 100.x.x.x)
QStringList LocalAddresses()
{
    QStringList tailscale, others;
    for (const QHostAddress& a : QNetworkInterface::allAddresses())
    {
        if (a.protocol() != QAbstractSocket::IPv4Protocol || a.isLoopback())
            continue;
        QString s = a.toString();
        if (s.startsWith("169.254."))
            continue;
        (s.startsWith("100.") ? tailscale : others).append(s);
    }
    return tailscale + others;
}

QPushButton* BigButton(const QString& text, const char* name = nullptr)
{
    auto* b = new QPushButton(text);
    if (name)
        b->setObjectName(name);
    b->setMinimumHeight(70);
    b->setCursor(Qt::PointingHandCursor);
    return b;
}

}

bool CoopLobby_Run()
{
    QSettings settings(HoLympeDir() + "/coop.ini", QSettings::IniFormat);

    QDialog dialog;
    dialog.setWindowTitle("Pokémon HeartGold Coop");
    dialog.setStyleSheet(kStyle);
    dialog.resize(900, 720);

    auto* root = new QVBoxLayout(&dialog);
    root->setContentsMargins(60, 40, 60, 40);
    root->setSpacing(18);
    auto* title = new QLabel("HeartGold Coop");
    title->setObjectName("title");
    root->addWidget(title);

    auto* pages = new QStackedWidget;
    root->addWidget(pages, 1);

    QString mode;

    // Page 1: the choice
    auto* choice = new QWidget;
    auto* choiceLayout = new QVBoxLayout(choice);
    choiceLayout->setSpacing(16);
    auto* nameText = new QLabel("Ton pseudo (au-dessus de ton personnage, et ton nom dans une nouvelle partie) :");
    nameText->setObjectName("hint");
    choiceLayout->addWidget(nameText);
    auto* nameEdit = new QLineEdit(settings.value("playerName").toString());
    nameEdit->setMaxLength(7); // PLAYER_NAME_LENGTH
    nameEdit->setPlaceholderText("7 lettres maximum");
    choiceLayout->addWidget(nameEdit);
    auto* nameError = new QLabel;
    nameError->setObjectName("error");
    nameError->hide();
    choiceLayout->addWidget(nameError);
    auto* subtitle = new QLabel("Comment veux-tu jouer ?");
    subtitle->setObjectName("subtitle");
    choiceLayout->addWidget(subtitle);
    auto* solo = BigButton("Jouer seul");
    auto* joinButton = BigButton("Jouer en ligne");
    auto* hostButton = BigButton("Héberger sur ce PC (sans serveur)", "back");
    choiceLayout->addWidget(solo);
    choiceLayout->addWidget(joinButton);
    choiceLayout->addStretch();
    choiceLayout->addWidget(hostButton, 0, Qt::AlignLeft);
    pages->addWidget(choice);

    // Page 2: host, with the address to give
    auto* hostPage = new QWidget;
    auto* hostLayout = new QVBoxLayout(hostPage);
    hostLayout->setSpacing(14);
    auto* hostText = new QLabel("Donne cette adresse à ton ami :");
    hostText->setObjectName("subtitle");
    hostLayout->addWidget(hostText);
    QStringList addresses = LocalAddresses();
    auto* address = new QLabel(addresses.isEmpty() ? "(pas de réseau)" : addresses.first());
    address->setObjectName("address");
    address->setTextInteractionFlags(Qt::TextSelectableByMouse);
    hostLayout->addWidget(address);
    auto* hostHint = new QLabel(addresses.size() > 1
        ? "Autres adresses de ce PC : " + addresses.mid(1).join("   ") + "\nSur Internet, utilisez Tailscale (adresse en 100.x.x.x)."
        : "Sur Internet, utilisez Tailscale (adresse en 100.x.x.x).");
    hostHint->setObjectName("hint");
    hostHint->setWordWrap(true);
    hostLayout->addWidget(hostHint);
    hostLayout->addStretch();
    auto* hostRow = new QHBoxLayout;
    auto* hostBack = BigButton("Retour", "back");
    auto* hostGo = BigButton("Lancer la partie", "primary");
    hostRow->addWidget(hostBack);
    hostRow->addStretch();
    hostRow->addWidget(hostGo);
    hostLayout->addLayout(hostRow);
    pages->addWidget(hostPage);

    // Page 3: join, with the friend's address
    auto* joinPage = new QWidget;
    auto* joinLayout = new QVBoxLayout(joinPage);
    joinLayout->setSpacing(14);
    auto* joinText = new QLabel("Adresse du serveur :");
    joinText->setObjectName("subtitle");
    joinLayout->addWidget(joinText);
    auto* joinAddress = new QLineEdit(settings.value("lastAddress").toString());
    joinAddress->setPlaceholderText("ex : 140.238.12.34");
    joinLayout->addWidget(joinAddress);
    auto* joinHint = new QLabel("Tu joues normalement même si ton ami n'est pas là :\nil apparaîtra dès qu'il se connectera.");
    joinHint->setObjectName("hint");
    joinLayout->addWidget(joinHint);
    joinLayout->addStretch();
    auto* joinRow = new QHBoxLayout;
    auto* joinBack = BigButton("Retour", "back");
    auto* joinGo = BigButton("Jouer", "primary");
    joinRow->addWidget(joinBack);
    joinRow->addStretch();
    joinRow->addWidget(joinGo);
    joinLayout->addLayout(joinRow);
    pages->addWidget(joinPage);

    // The name must exist in the game's alphabet
    auto nameOk = [&] {
        QString name = nameEdit->text().trimmed();
        melonDS::u16 code[8];
        QString problem = name.isEmpty() ? QString::fromUtf8("Choisis un pseudo d'abord.")
            : !EncodeGameText(name, code, 7) ? QString::fromUtf8("Ce pseudo contient un caractère que le jeu ne connaît pas.")
            : QString();
        nameError->setText(problem);
        nameError->setVisible(!problem.isEmpty());
        if (!problem.isEmpty())
        {
            pages->setCurrentWidget(choice);
            nameEdit->setFocus();
        }
        return problem.isEmpty();
    };
    QObject::connect(solo, &QPushButton::clicked, [&] { if (nameOk()) { mode = "solo"; dialog.accept(); } });
    QObject::connect(hostButton, &QPushButton::clicked, [&] { if (nameOk()) { pages->setCurrentWidget(hostPage); hostGo->setFocus(); } });
    QObject::connect(joinButton, &QPushButton::clicked, [&] { if (nameOk()) { pages->setCurrentWidget(joinPage); joinAddress->setFocus(); } });
    QObject::connect(hostBack, &QPushButton::clicked, [&] { pages->setCurrentWidget(choice); hostButton->setFocus(); });
    QObject::connect(joinBack, &QPushButton::clicked, [&] { pages->setCurrentWidget(choice); joinButton->setFocus(); });
    QObject::connect(hostGo, &QPushButton::clicked, [&] { mode = "host"; dialog.accept(); });
    auto join = [&] {
        if (joinAddress->text().trimmed().isEmpty())
        {
            joinAddress->setFocus();
            return;
        }
        mode = "join";
        dialog.accept();
    };
    QObject::connect(joinGo, &QPushButton::clicked, join);
    QObject::connect(joinAddress, &QLineEdit::returnPressed, join);
    if (nameEdit->text().isEmpty())
        nameEdit->setFocus();
    else
        solo->setFocus();

    // Arrows move between the buttons too
    auto* down = new QShortcut(QKeySequence(Qt::Key_Down), &dialog);
    auto* up = new QShortcut(QKeySequence(Qt::Key_Up), &dialog);
    auto moveFocus = [&](bool next) {
        QWidget* w = QApplication::focusWidget();
        if (!w)
            return;
        for (QWidget* c = next ? w->nextInFocusChain() : w->previousInFocusChain(); c && c != w;
             c = next ? c->nextInFocusChain() : c->previousInFocusChain())
        {
            if (c->isVisible() && c->isEnabled() && (c->focusPolicy() & Qt::TabFocus) && c->window() == &dialog)
            {
                c->setFocus(next ? Qt::TabFocusReason : Qt::BacktabFocusReason);
                return;
            }
        }
    };
    QObject::connect(down, &QShortcut::activated, [&] { moveFocus(true); });
    QObject::connect(up, &QShortcut::activated, [&] { moveFocus(false); });

    if (dialog.exec() != QDialog::Accepted || mode.isEmpty())
        return false;

    QString name = nameEdit->text().trimmed();
    settings.setValue("playerName", name);
    qputenv("HGCOOP_NAME", name.toUtf8());

    if (mode == "host")
        qputenv("HGCOOP_MODE", "host");
    else if (mode == "join")
    {
        QString addr = joinAddress->text().trimmed();
        settings.setValue("lastAddress", addr);
        qputenv("HGCOOP_MODE", "join");
        qputenv("HGCOOP_ADDR", addr.toUtf8());
    }
    return true;
}
