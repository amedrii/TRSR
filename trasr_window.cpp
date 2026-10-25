#include <QApplication>
#include "attempt_log.h"
#include "tra_process.h"
#include "run_rules.h"
#include "run_monitor.h"
#include "screen_completion.h"
#include "trasr_window.h"
#include <QComboBox>
#include <QDateTime>
#include <QFont>
#include <QFormLayout>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QDialog>
#include <QDir>
#include <QFileInfo>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QProcess>
#include <QPixmap>
#include <QSettings>
#include <QStringList>

#include <memory>
#include <QTimer>
#include <QStackedWidget>
#include <QUuid>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <thread>
#include <utility>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

enum class ClientNotification
{
    MatchFound,
    Victory,
    Defeat
};

static void playClientNotification(ClientNotification notification)
{
    // Original short "bronze chime" patterns. They do not reuse TRA audio.
    std::thread([notification]() {
        const std::initializer_list<std::pair<DWORD, DWORD>> tones =
            notification == ClientNotification::MatchFound
                ? std::initializer_list<std::pair<DWORD, DWORD>>{{392, 80}, {523, 80}, {659, 140}}
                : notification == ClientNotification::Victory
                    ? std::initializer_list<std::pair<DWORD, DWORD>>{{523, 80}, {659, 80}, {784, 80}, {1047, 180}}
                    : std::initializer_list<std::pair<DWORD, DWORD>>{{494, 120}, {392, 120}, {294, 180}};
        for (const auto &[frequency, duration] : tones) {
            Beep(frequency, duration);
        }
    }).detach();
}

static void bringMatchWindowToFront(QMainWindow &window)
{
    if (window.isMinimized()) {
        window.showNormal();
    }
    window.raise();
    window.activateWindow();

    FLASHWINFO flashInfo{};
    flashInfo.cbSize = sizeof(flashInfo);
    flashInfo.hwnd = reinterpret_cast<HWND>(window.winId());
    flashInfo.dwFlags = FLASHW_ALL | FLASHW_TIMERNOFG;
    flashInfo.uCount = 3;
    flashInfo.dwTimeout = 0;
    FlashWindowEx(&flashInfo);
    SetForegroundWindow(flashInfo.hwnd);
}

static QString countryFlagEmoji(const QString &countryCode)
{
    const QString code = countryCode.trimmed().toUpper();
    if (code.size() != 2 || !code.at(0).isLetter() || !code.at(1).isLetter()) {
        return {};
    }

    const uint first = 0x1F1E6u + code.at(0).unicode() - u'A';
    const uint second = 0x1F1E6u + code.at(1).unicode() - u'A';
    QString flag;
    flag.append(QChar::highSurrogate(first));
    flag.append(QChar::lowSurrogate(first));
    flag.append(QChar::highSurrogate(second));
    flag.append(QChar::lowSurrogate(second));
    return flag;
}

int runTrasrApplication(QApplication &app)
{

    QCoreApplication::setOrganizationName(QStringLiteral("TRASR"));
    QCoreApplication::setApplicationName(QStringLiteral("TRASR"));

    // QSettings stores login and UI selections between application launches.
    QSettings settings;

    QString playerId =
        settings.value(QStringLiteral("matchmaking/playerId")).toString();

    if (playerId.isEmpty()) {
        playerId = QUuid::createUuid().toString(QUuid::WithoutBraces);

        settings.setValue(
            QStringLiteral("matchmaking/playerId"),
            playerId
            );
    }

    QMainWindow window;
    window.setWindowTitle(QStringLiteral("TRASR"));

    QDialog loginDialog(&window);
    loginDialog.setWindowTitle(QStringLiteral("TRSR login"));
    loginDialog.setModal(true);

    auto *loginLayout = new QVBoxLayout(&loginDialog);

    auto *loginIntro = new QLabel(
        QStringLiteral("Link your Speedrun.com account to use TRSR matchmaking.")
        );
    loginIntro->setWordWrap(true);

    auto *speedrunApiKeyLabel = new QLabel(
        QStringLiteral("Speedrun.com API key")
        );

    auto *speedrunApiKeyInput = new QLineEdit;
    speedrunApiKeyInput->setEchoMode(QLineEdit::Password);
    speedrunApiKeyInput->setPlaceholderText(
        QStringLiteral("Paste from speedrun.com/api/auth")
        );

    auto *linkSpeedrunButton = new QPushButton(
        QStringLiteral("Link Speedrun.com")
        );

    auto *speedrunStatus = new QLabel(
        QStringLiteral("Speedrun.com: checking saved login...")
        );
    speedrunStatus->setAlignment(Qt::AlignCenter);

    loginLayout->addWidget(loginIntro);
    loginLayout->addWidget(speedrunApiKeyLabel);
    loginLayout->addWidget(speedrunApiKeyInput);
    loginLayout->addWidget(linkSpeedrunButton);
    loginLayout->addWidget(speedrunStatus);

    auto *accountStatus = new QLabel(
        QStringLiteral("Speedrun.com: not linked")
        );
    accountStatus->setAlignment(Qt::AlignCenter);

    auto *logoutButton = new QPushButton(
        QStringLiteral("Log out")
        );

    auto *adminButton = new QPushButton(QStringLiteral("Admin"));
    adminButton->setObjectName(QStringLiteral("secondaryButton"));
    adminButton->hide();

    auto *content = new QWidget;
    content->setObjectName(QStringLiteral("appRoot"));
    auto *layout = new QVBoxLayout(content);
    layout->setContentsMargins(18, 18, 18, 18);
    layout->setSpacing(14);

    auto *levelLabel = new QLabel(QStringLiteral("Level"));
    auto *levelSelector = new QComboBox;

    levelSelector->addItems(QStringList{
        QStringLiteral("Mountain Caves"),
        QStringLiteral("City of Vilcabamba"),
        QStringLiteral("The Lost Valley"),
        QStringLiteral("Tomb of Qualopec"),
        QStringLiteral("St. Francis Folly"),
        QStringLiteral("The Coliseum"),
        QStringLiteral("Midas Palace"),
        QStringLiteral("Tomb of Tihocan"),
        QStringLiteral("Temple of Khamoon"),
        QStringLiteral("Obelisk of Khamoon"),
        QStringLiteral("Sanctuary of the Scion"),
        QStringLiteral("Natla's Mines"),
        QStringLiteral("The Great Pyramid"),
        QStringLiteral("Final Conflict"),
        QStringLiteral("Croft Manor")
    });

    auto *categoryLabel = new QLabel(QStringLiteral("Category"));
    auto *categorySelector = new QComboBox;

    categorySelector->addItems(QStringList{
        QStringLiteral("Any%"),
        QStringLiteral("100%")
    });

    auto *subcategoryLabel = new QLabel(QStringLiteral("Subcategory"));
    auto *subcategorySelector = new QComboBox;

    subcategorySelector->addItems(QStringList{
        QStringLiteral("No Bug Jump"),
        QStringLiteral("Bug Jump"),
        QStringLiteral("Better Than Glitchless"),
        QStringLiteral("Glitchless")
    });

    // Restore the user's previous local selection.
    const int savedLevelIndex = levelSelector->findText(
        settings.value(
                    QStringLiteral("selectedLevel"),
                    QStringLiteral("Mountain Caves")
                    ).toString()
        );

    if (savedLevelIndex >= 0) {
        levelSelector->setCurrentIndex(savedLevelIndex);
    }

    const int savedCategoryIndex = categorySelector->findText(
        settings.value(
                    QStringLiteral("selectedCategory"),
                    QStringLiteral("Any%")
                    ).toString()
        );

    if (savedCategoryIndex >= 0) {
        categorySelector->setCurrentIndex(savedCategoryIndex);
    }

    const int savedSubcategoryIndex = subcategorySelector->findText(
        settings.value(
                    QStringLiteral("selectedSubcategory"),
                    QStringLiteral("No Bug Jump")
                    ).toString()
        );

    if (savedSubcategoryIndex >= 0) {
        subcategorySelector->setCurrentIndex(savedSubcategoryIndex);
    }

    auto *status = new QLabel;
    status->setAlignment(Qt::AlignCenter);
    status->setWordWrap(true);

    auto *serverStatus = new QLabel(
        QStringLiteral("Matchmaking server: checking...")
        );
    serverStatus->setAlignment(Qt::AlignCenter);

    auto *screenVerifierStatus = new QLabel(
        QStringLiteral("Screen verifier: starting after login")
        );
    screenVerifierStatus->setAlignment(Qt::AlignCenter);

    auto *opponentStatus = new QLabel(
        QStringLiteral("Opponent: waiting for match")
        );
    opponentStatus->setAlignment(Qt::AlignCenter);

    auto *readyButton = new QPushButton(
        QStringLiteral("Ready on Replay Settings")
        );
    readyButton->setEnabled(false);

    auto *autoStartStatus = new QLabel(
        QStringLiteral("Ready check: join a match first")
        );
    autoStartStatus->setAlignment(Qt::AlignCenter);
    autoStartStatus->setWordWrap(true);

    auto *forfeitButton = new QPushButton(
        QStringLiteral("Forfeit match")
        );
    forfeitButton->setEnabled(false);

    auto *acceptMatchButton = new QPushButton(QStringLiteral("Accept match"));
    acceptMatchButton->setObjectName(QStringLiteral("primaryButton"));
    acceptMatchButton->hide();
    auto *declineMatchButton = new QPushButton(QStringLiteral("Decline"));
    declineMatchButton->setObjectName(QStringLiteral("dangerButton"));
    declineMatchButton->hide();

    // UI only for now; queue joining is the next matchmaking step.
    auto *randomQueueButton = new QPushButton(
        QStringLiteral("Join random queue")
        );

    auto *rulesetQueueButton = new QPushButton(
        QStringLiteral("Join selected ruleset queue")
        );

    auto *cancelQueueButton = new QPushButton(
        QStringLiteral("Cancel queue")
        );

    auto *queueStatus = new QLabel(
        QStringLiteral("Matchmaking: not queued")
        );
    queueStatus->setAlignment(Qt::AlignCenter);

    auto setQueueUiState = [randomQueueButton, rulesetQueueButton,
                            cancelQueueButton, levelSelector,
                            categorySelector, subcategorySelector](
                               bool canJoin, bool canCancel, bool canChangeRuleset
                               ) {
        randomQueueButton->setEnabled(canJoin);
        rulesetQueueButton->setEnabled(canJoin);
        cancelQueueButton->setEnabled(canCancel);
        levelSelector->setEnabled(canChangeRuleset);
        categorySelector->setEnabled(canChangeRuleset);
        subcategorySelector->setEnabled(canChangeRuleset);
    };

    setQueueUiState(true, false, true);

    QFont statusFont;
    statusFont.setPointSize(16);
    statusFont.setBold(true);
    status->setFont(statusFont);

    // Save a changed ruleset between launches.
    auto refreshSelection =
        [levelSelector, categorySelector, subcategorySelector]() {
            QSettings settings;

            const QString level = levelSelector->currentText();
            const QString category = categorySelector->currentText();
            const QString subcategory = subcategorySelector->currentText();

            settings.setValue(QStringLiteral("selectedLevel"), level);
            settings.setValue(QStringLiteral("selectedCategory"), category);
            settings.setValue(
                QStringLiteral("selectedSubcategory"),
                subcategory
                );

        };

    refreshSelection();

    auto *header = new QFrame;
    header->setObjectName(QStringLiteral("appHeader"));
    auto *headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(18, 14, 18, 14);

    auto *brandLayout = new QVBoxLayout;
    brandLayout->setSpacing(0);
    auto *brand = new QLabel(QStringLiteral("TRSR"));
    brand->setObjectName(QStringLiteral("brand"));
    auto *tagline = new QLabel(QStringLiteral("Tomb Raider Speedrunning"));
    tagline->setObjectName(QStringLiteral("tagline"));
    brandLayout->addWidget(brand);
    brandLayout->addWidget(tagline);
    headerLayout->addLayout(brandLayout, 1);

    auto *accountLayout = new QVBoxLayout;
    accountLayout->setSpacing(4);
    accountLayout->addWidget(accountStatus, 0, Qt::AlignRight);
    accountLayout->addWidget(adminButton);
    accountLayout->addWidget(logoutButton);
    headerLayout->addLayout(accountLayout);

    auto *screens = new QStackedWidget;

    auto *queuePage = new QWidget;
    auto *queuePageLayout = new QVBoxLayout(queuePage);
    queuePageLayout->setContentsMargins(4, 14, 4, 4);
    queuePageLayout->setSpacing(12);

    auto *rulesetBox = new QGroupBox(QStringLiteral("Ruleset"));
    auto *rulesetLayout = new QFormLayout(rulesetBox);
    rulesetLayout->setLabelAlignment(Qt::AlignLeft);
    rulesetLayout->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    rulesetLayout->addRow(levelLabel, levelSelector);
    rulesetLayout->addRow(categoryLabel, categorySelector);
    rulesetLayout->addRow(subcategoryLabel, subcategorySelector);

    auto *queueBox = new QGroupBox(QStringLiteral("Find a match"));
    auto *queueLayout = new QVBoxLayout(queueBox);
    queueLayout->setSpacing(10);
    rulesetQueueButton->setObjectName(QStringLiteral("primaryButton"));
    randomQueueButton->setObjectName(QStringLiteral("secondaryButton"));
    auto *queueButtons = new QHBoxLayout;
    queueButtons->addWidget(rulesetQueueButton, 2);
    queueButtons->addWidget(randomQueueButton, 1);
    queueLayout->addLayout(queueButtons);

    queuePageLayout->addWidget(rulesetBox);
    queuePageLayout->addWidget(queueBox);
    queuePageLayout->addStretch(1);

    auto *matchPage = new QWidget;
    auto *matchLayout = new QVBoxLayout(matchPage);
    matchLayout->setContentsMargins(4, 14, 4, 4);
    matchLayout->setSpacing(12);

    auto *versusPanel = new QFrame;
    versusPanel->setObjectName(QStringLiteral("versusPanel"));
    auto *versusLayout = new QHBoxLayout(versusPanel);
    versusLayout->setContentsMargins(16, 12, 16, 12);
    versusLayout->setSpacing(14);

    auto *localPlayerCard = new QWidget;
    auto *localPlayerLayout = new QVBoxLayout(localPlayerCard);
    localPlayerLayout->setContentsMargins(0, 0, 0, 0);
    localPlayerLayout->setSpacing(5);
    auto *localPlayerCaption = new QLabel(QStringLiteral("YOU"));
    localPlayerCaption->setObjectName(QStringLiteral("playerCaption"));
    localPlayerCaption->setAlignment(Qt::AlignCenter);
    auto *localPlayerAvatar = new QLabel(QStringLiteral("?"));
    localPlayerAvatar->setObjectName(QStringLiteral("playerAvatar"));
    localPlayerAvatar->setAlignment(Qt::AlignCenter);
    localPlayerAvatar->setFixedSize(64, 64);
    auto *localPlayerName = new QLabel;
    localPlayerName->setObjectName(QStringLiteral("playerName"));
    localPlayerName->setTextFormat(Qt::PlainText);
    localPlayerName->setAlignment(Qt::AlignCenter);
    localPlayerName->setWordWrap(true);
    localPlayerLayout->addWidget(localPlayerCaption);
    localPlayerLayout->addWidget(localPlayerAvatar, 0, Qt::AlignCenter);
    localPlayerLayout->addWidget(localPlayerName);

    auto *versusLabel = new QLabel(QStringLiteral("VS"));
    versusLabel->setObjectName(QStringLiteral("versusLabel"));
    versusLabel->setAlignment(Qt::AlignCenter);

    auto *opponentPlayerCard = new QWidget;
    auto *opponentPlayerLayout = new QVBoxLayout(opponentPlayerCard);
    opponentPlayerLayout->setContentsMargins(0, 0, 0, 0);
    opponentPlayerLayout->setSpacing(5);
    auto *opponentPlayerCaption = new QLabel(QStringLiteral("OPPONENT"));
    opponentPlayerCaption->setObjectName(QStringLiteral("playerCaption"));
    opponentPlayerCaption->setAlignment(Qt::AlignCenter);
    auto *opponentPlayerAvatar = new QLabel(QStringLiteral("?"));
    opponentPlayerAvatar->setObjectName(QStringLiteral("playerAvatar"));
    opponentPlayerAvatar->setAlignment(Qt::AlignCenter);
    opponentPlayerAvatar->setFixedSize(64, 64);
    auto *opponentPlayerName = new QLabel;
    opponentPlayerName->setObjectName(QStringLiteral("playerName"));
    opponentPlayerName->setTextFormat(Qt::PlainText);
    opponentPlayerName->setAlignment(Qt::AlignCenter);
    opponentPlayerName->setWordWrap(true);
    opponentPlayerLayout->addWidget(opponentPlayerCaption);
    opponentPlayerLayout->addWidget(opponentPlayerAvatar, 0, Qt::AlignCenter);
    opponentPlayerLayout->addWidget(opponentPlayerName);

    versusLayout->addWidget(localPlayerCard, 1);
    versusLayout->addWidget(versusLabel);
    versusLayout->addWidget(opponentPlayerCard, 1);
    versusPanel->hide();

    auto *matchBox = new QGroupBox(QStringLiteral("Match status"));
    auto *matchBoxLayout = new QVBoxLayout(matchBox);
    matchBoxLayout->setSpacing(10);
    queueStatus->setObjectName(QStringLiteral("queueStatus"));
    queueStatus->setWordWrap(true);
    opponentStatus->setObjectName(QStringLiteral("opponentStatus"));
    opponentStatus->setWordWrap(true);
    matchBoxLayout->addWidget(queueStatus);
    matchBoxLayout->addWidget(opponentStatus);
    auto *proposalButtons = new QHBoxLayout;
    proposalButtons->addWidget(acceptMatchButton, 2);
    proposalButtons->addWidget(declineMatchButton, 1);
    matchBoxLayout->addLayout(proposalButtons);
    matchBoxLayout->addWidget(autoStartStatus);
    matchBoxLayout->addWidget(readyButton);

    cancelQueueButton->setObjectName(QStringLiteral("secondaryButton"));
    forfeitButton->setObjectName(QStringLiteral("dangerButton"));
    matchBoxLayout->addWidget(cancelQueueButton);
    matchBoxLayout->addWidget(forfeitButton);

    auto *connectionBox = new QGroupBox(QStringLiteral("Connection"));
    auto *connectionLayout = new QVBoxLayout(connectionBox);
    serverStatus->setObjectName(QStringLiteral("connectionStatus"));
    connectionLayout->addWidget(serverStatus);

    matchLayout->addWidget(versusPanel);
    matchLayout->addWidget(matchBox);
    matchLayout->addWidget(connectionBox);
    matchLayout->addStretch(1);

    auto *runPage = new QWidget;
    auto *runLayout = new QVBoxLayout(runPage);
    runLayout->setContentsMargins(4, 26, 4, 4);
    auto *runBox = new QGroupBox(QStringLiteral("Run"));
    auto *runBoxLayout = new QVBoxLayout(runBox);
    status->setObjectName(QStringLiteral("runStatus"));
    status->setAlignment(Qt::AlignCenter);
    runBoxLayout->addWidget(status);
    runLayout->addWidget(runBox);
    runLayout->addStretch(1);

    auto *resultsPage = new QWidget;
    auto *resultsLayout = new QVBoxLayout(resultsPage);
    resultsLayout->setContentsMargins(18, 30, 18, 18);
    resultsLayout->setSpacing(14);

    auto *resultsBox = new QGroupBox(QStringLiteral("Match result"));
    auto *resultsBoxLayout = new QVBoxLayout(resultsBox);
    resultsBoxLayout->setSpacing(9);
    auto *resultTitle = new QLabel;
    resultTitle->setObjectName(QStringLiteral("resultTitle"));
    resultTitle->setAlignment(Qt::AlignCenter);
    auto *resultRuleset = new QLabel;
    resultRuleset->setObjectName(QStringLiteral("mutedText"));
    resultRuleset->setAlignment(Qt::AlignCenter);
    resultRuleset->setWordWrap(true);
    auto *resultDetail = new QLabel;
    resultDetail->setObjectName(QStringLiteral("resultDetail"));
    resultDetail->setAlignment(Qt::AlignCenter);
    resultDetail->setWordWrap(true);
    resultsBoxLayout->addWidget(resultTitle);
    resultsBoxLayout->addWidget(resultRuleset);
    resultsBoxLayout->addWidget(resultDetail);

    auto *requeueButton = new QPushButton(QStringLiteral("Requeue"));
    requeueButton->setObjectName(QStringLiteral("primaryButton"));
    auto *backToQueueButton = new QPushButton(QStringLiteral("Back to queue"));
    backToQueueButton->setObjectName(QStringLiteral("secondaryButton"));
    resultsLayout->addWidget(resultsBox);
    resultsLayout->addWidget(requeueButton);
    resultsLayout->addWidget(backToQueueButton);
    resultsLayout->addStretch(1);

    auto *adminPage = new QWidget;
    auto *adminLayout = new QVBoxLayout(adminPage);
    adminLayout->setContentsMargins(18, 22, 18, 18);
    adminLayout->setSpacing(14);
    auto *adminBox = new QGroupBox(QStringLiteral("Administration"));
    auto *adminBoxLayout = new QVBoxLayout(adminBox);
    auto *adminStatus = new QLabel(QStringLiteral("Loading server overview…"));
    adminStatus->setObjectName(QStringLiteral("mutedText"));
    adminStatus->setAlignment(Qt::AlignCenter);
    adminStatus->setWordWrap(true);
    auto *refreshAdminButton = new QPushButton(QStringLiteral("Refresh"));
    refreshAdminButton->setObjectName(QStringLiteral("primaryButton"));
    auto *backFromAdminButton = new QPushButton(QStringLiteral("Back to queue"));
    backFromAdminButton->setObjectName(QStringLiteral("secondaryButton"));
    adminBoxLayout->addWidget(adminStatus);
    adminBoxLayout->addWidget(refreshAdminButton);
    adminLayout->addWidget(adminBox);
    adminLayout->addWidget(backFromAdminButton);
    adminLayout->addStretch(1);

    screens->addWidget(queuePage);
    screens->addWidget(matchPage);
    screens->addWidget(runPage);
    screens->addWidget(resultsPage);
    screens->addWidget(adminPage);

    auto showQueueScreen = [header, screens, queuePage]() {
        header->show();
        screens->setCurrentWidget(queuePage);
    };
    auto showMatchScreen = [header, screens, matchPage]() {
        header->show();
        screens->setCurrentWidget(matchPage);
    };
    auto showRunScreen = [header, screens, runPage]() {
        header->hide();
        screens->setCurrentWidget(runPage);
    };
    auto showResultsScreen = [header, screens, resultsPage]() {
        header->show();
        screens->setCurrentWidget(resultsPage);
    };
    auto showAdminScreen = [header, screens, adminPage]() {
        header->show();
        screens->setCurrentWidget(adminPage);
    };

    QObject::connect(backToQueueButton, &QPushButton::clicked,
                     &window, showQueueScreen);
    QObject::connect(backFromAdminButton, &QPushButton::clicked,
                     &window, showQueueScreen);
    QObject::connect(
        requeueButton, &QPushButton::clicked, &window,
        [showMatchScreen, rulesetQueueButton]() {
            showMatchScreen();
            rulesetQueueButton->click();
        });

    QObject::connect(randomQueueButton, &QPushButton::clicked,
                     &window, showMatchScreen);
    QObject::connect(rulesetQueueButton, &QPushButton::clicked,
                     &window, showMatchScreen);
    auto *screenFlowTimer = new QTimer(&window);
    screenFlowTimer->setInterval(100);

    QObject::connect(
        screenFlowTimer, &QTimer::timeout, &window,
        [screens, runPage, resultsPage, queueStatus, showQueueScreen, showMatchScreen]() {
            if (screens->currentWidget() == runPage
                || screens->currentWidget() == resultsPage) {
                return;
            }

            const QString message = queueStatus->text();

            if (message.startsWith(QStringLiteral("Matchmaking: joining"))
                || message.contains(QStringLiteral("waiting for"))
                || message.startsWith(QStringLiteral("Match found"))) {
                showMatchScreen();
            } else if (message.startsWith(QStringLiteral("Matchmaking:"))
                       || message.startsWith(QStringLiteral("Match complete"))) {
                showQueueScreen();
            }
        });

    screenFlowTimer->start();

    layout->addWidget(header);
    layout->addWidget(screens, 1);

    window.setCentralWidget(content);
    window.setMinimumSize(540, 590);
    window.resize(540, 680);
    window.setStyleSheet(QStringLiteral(R"(
        QWidget#appRoot { background: #10100f; color: #ece4cf; font-family: "Bahnschrift"; font-size: 13px; }
        QFrame#appHeader { background: #1b1a18; border: 1px solid #3b362b; border-radius: 8px; }
        QLabel#brand { color: #d7b86a; font-family: "Georgia"; font-size: 26px; font-weight: 700; letter-spacing: 3px; }
        QLabel#tagline, QLabel#mutedText { color: #a49a85; }
        QStackedWidget { border: 1px solid #3b362b; background: #171614; border-radius: 7px; }
        QGroupBox { color: #d7b86a; font-weight: 600; border: 1px solid #3b362b; border-radius: 7px; margin-top: 10px; padding: 13px 12px 12px 12px; }
        QGroupBox::title { subcontrol-origin: margin; left: 11px; padding: 0 5px; }
        QLabel { color: #ece4cf; }
        QFrame#versusPanel { background: #1b1a18; border: 1px solid #51452e; border-radius: 8px; }
        QLabel#playerCaption { color: #a49a85; font-size: 10px; font-weight: 700; letter-spacing: 1px; }
        QLabel#playerAvatar { background: #2b2822; border: 1px solid #806a38; border-radius: 32px; color: #d7b86a; font-family: "Georgia"; font-size: 24px; font-weight: 700; }
        QLabel#playerName { color: #f0e7d0; font-weight: 700; }
        QLabel#versusLabel { color: #d7b86a; font-family: "Georgia"; font-size: 22px; font-weight: 700; }
        QLabel#queueStatus, QLabel#opponentStatus, QLabel#connectionStatus, QLabel#verifierStatus { color: #b7ae9b; }
        QLabel#runStatus { color: #f0e7d0; font-size: 17px; font-weight: 700; padding: 12px 4px; }
        QLabel#resultTitle { color: #d7b86a; font-family: "Georgia"; font-size: 28px; font-weight: 700; padding: 12px 4px 2px 4px; }
        QLabel#resultDetail { color: #f0e7d0; font-size: 15px; font-weight: 600; padding: 4px; }
        QComboBox, QLineEdit { min-height: 30px; padding: 2px 10px; background: #272521; border: 1px solid #4b4437; border-radius: 5px; color: #f0e7d0; }
        QComboBox:hover, QLineEdit:focus { border: 1px solid #b7964e; }
        QComboBox::drop-down { border: none; width: 26px; }
        QComboBox QAbstractItemView { background: #272521; border: 1px solid #4b4437; color: #f0e7d0; selection-background-color: #5e4b27; }
        QPushButton { min-height: 31px; padding: 3px 12px; background: #2a2824; border: 1px solid #4b4437; border-radius: 5px; color: #e9e0cc; font-weight: 600; }
        QPushButton:hover:enabled { background: #36322b; border-color: #a88a4b; }
        QPushButton:disabled { color: #716b5f; border-color: #312e29; background: #1d1c1a; }
        QPushButton#primaryButton { background: #b8954e; color: #16130e; border-color: #d3b66f; }
        QPushButton#primaryButton:hover:enabled { background: #d0ae65; }
        QPushButton#dangerButton:hover:enabled { color: #ffd3ca; border-color: #a45242; background: #472721; }
    )"));

    // Local development health check for the Node matchmaking server.
    auto *networkManager = new QNetworkAccessManager(&window);

    auto setPlayerAvatar = [networkManager](QLabel *avatar, const QString &avatarUrl) {
        avatar->setProperty("avatarUrl", avatarUrl);
        avatar->setPixmap(QPixmap());
        avatar->setText(QStringLiteral("?"));
        if (avatarUrl.isEmpty()) {
            return;
        }

        auto *reply = networkManager->get(QNetworkRequest(QUrl(avatarUrl)));
        QObject::connect(reply, &QNetworkReply::finished, [reply, avatar, avatarUrl]() {
            const QByteArray bytes = reply->readAll();
            QPixmap source;
            if (reply->error() == QNetworkReply::NoError
                && avatar->property("avatarUrl").toString() == avatarUrl
                && source.loadFromData(bytes)) {
                const QSize size = avatar->size();
                const QPixmap scaled = source.scaled(
                    size, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
                QPixmap circular(size);
                circular.fill(Qt::transparent);
                QPainter painter(&circular);
                painter.setRenderHint(QPainter::Antialiasing);
                QPainterPath mask;
                mask.addEllipse(circular.rect());
                painter.setClipPath(mask);
                painter.drawPixmap((size.width() - scaled.width()) / 2,
                                   (size.height() - scaled.height()) / 2, scaled);
                avatar->setText({});
                avatar->setPixmap(circular);
            }
            reply->deleteLater();
        });
    };

    auto hideMatchPlayers = [versusPanel]() {
        versusPanel->hide();
    };
    auto showMatchPlayers =
        [playerId, versusPanel, localPlayerAvatar, localPlayerName,
         opponentPlayerAvatar, opponentPlayerName, setPlayerAvatar](const QJsonArray &players) {
            QJsonObject localPlayer;
            QJsonObject opponentPlayer;
            for (const QJsonValue &value : players) {
                const QJsonObject player = value.toObject();
                if (player.value(QStringLiteral("playerId")).toString() == playerId) {
                    localPlayer = player;
                } else {
                    opponentPlayer = player;
                }
            }
            if (localPlayer.isEmpty() || opponentPlayer.isEmpty()) {
                versusPanel->hide();
                return;
            }

            const auto populateCard = [setPlayerAvatar](const QJsonObject &player,
                                                        QLabel *avatar, QLabel *name) {
                const QJsonObject profile = player.value(QStringLiteral("profile")).toObject();
                const QString displayName = profile.value(QStringLiteral("displayName")).toString(
                    player.value(QStringLiteral("speedrunUserId")).toString());
                const QString flag = countryFlagEmoji(
                    profile.value(QStringLiteral("countryCode")).toString());
                name->setText(flag.isEmpty() ? displayName : flag + QStringLiteral(" ") + displayName);
                setPlayerAvatar(avatar, profile.value(QStringLiteral("avatarUrl")).toString());
            };

            populateCard(localPlayer, localPlayerAvatar, localPlayerName);
            populateCard(opponentPlayer, opponentPlayerAvatar, opponentPlayerName);
            versusPanel->show();
        };

    auto refreshAdminOverview = [networkManager, adminStatus, refreshAdminButton]() {
        QSettings settings;
        const QString sessionToken = settings.value(
                                                 QStringLiteral("matchmaking/sessionToken")).toString();
        if (sessionToken.isEmpty()) {
            adminStatus->setText(QStringLiteral("Administrator session unavailable."));
            return;
        }
        refreshAdminButton->setEnabled(false);
        adminStatus->setText(QStringLiteral("Loading server overview…"));
        QNetworkRequest request(QUrl(QStringLiteral("https://api.trsr.app/admin/overview")));
        request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
        auto *reply = networkManager->post(
            request,
            QJsonDocument(QJsonObject{{QStringLiteral("sessionToken"), sessionToken}})
                .toJson(QJsonDocument::Compact));
        QObject::connect(reply, &QNetworkReply::finished,
                         [reply, adminStatus, refreshAdminButton]() {
                             const QJsonObject response = QJsonDocument::fromJson(reply->readAll()).object();
                             refreshAdminButton->setEnabled(true);
                             if (reply->error() != QNetworkReply::NoError) {
                                 const QString error = response.value(QStringLiteral("error")).toString();
                                 adminStatus->setText(error.isEmpty()
                                                          ? QStringLiteral("Could not load server overview.")
                                                          : error);
                                 reply->deleteLater();
                                 return;
                             }
                             const QJsonObject overview = response.value(QStringLiteral("overview")).toObject();
                             adminStatus->setText(
                                 QStringLiteral("Queued players: %1\nActive matches: %2\nRunners in level: %3\nResults in the last 24 hours: %4")
                                     .arg(overview.value(QStringLiteral("queuedPlayers")).toInt())
                                     .arg(overview.value(QStringLiteral("activeMatches")).toInt())
                                     .arg(overview.value(QStringLiteral("runnersInLevel")).toInt())
                                     .arg(overview.value(QStringLiteral("resultsLast24Hours")).toInt()));
                             reply->deleteLater();
                         });
    };

    QObject::connect(adminButton, &QPushButton::clicked, &window,
                     [showAdminScreen, refreshAdminOverview]() {
                         showAdminScreen();
                         refreshAdminOverview();
                     });
    QObject::connect(refreshAdminButton, &QPushButton::clicked, &window,
                     refreshAdminOverview);

    auto *screenDetector = new QProcess(&window);
    screenDetector->setProcessChannelMode(QProcess::MergedChannels);

    auto launchScreenDetector = [screenDetector, screenVerifierStatus]() {
        const QString level = screenDetector->property("selectedLevel").toString();
        const QDir appDirectory(QCoreApplication::applicationDirPath());
        const QString bundledDetector = appDirectory.filePath(
            QStringLiteral("tools/level_end_detector/level_end_detector.exe"));
        const QString scriptPath = appDirectory.filePath(
            QStringLiteral("tools/level_end_detector.py"));

        if (level.isEmpty()
            || (!QFileInfo::exists(bundledDetector) && !QFileInfo::exists(scriptPath))) {
            screenVerifierStatus->setText(
                QStringLiteral("Screen verifier: unavailable")
                );
            return;
        }

        QString program;
        QStringList arguments{
            QStringLiteral("--level"), level,
            QStringLiteral("--event-file"), screenCompletionEventPath()
        };
        if (QFileInfo::exists(bundledDetector)) {
            program = bundledDetector;
        } else {
            program = QStringLiteral("py");
            arguments.prepend(scriptPath);
            arguments.prepend(QStringLiteral("-3.13"));
        }

        QString tesseractPath = appDirectory.filePath(
            QStringLiteral("tools/tesseract/tesseract.exe"));
        if (!QFileInfo::exists(tesseractPath)) {
            tesseractPath = QStringLiteral("C:/Program Files/Tesseract-OCR/tesseract.exe");
        }
        if (QFileInfo::exists(tesseractPath)) {
            arguments << QStringLiteral("--tesseract") << tesseractPath;
        }

        screenDetector->start(program, arguments);
        screenVerifierStatus->setText(
            QStringLiteral("Screen verifier: watching %1").arg(level)
            );
    };

    auto restartScreenDetector = [screenDetector, launchScreenDetector](
                                     const QString &level
                                     ) {
        screenDetector->setProperty("selectedLevel", level);

        if (screenDetector->state() == QProcess::NotRunning) {
            launchScreenDetector();
            return;
        }

        screenDetector->setProperty("restartWhenStopped", true);
        screenDetector->terminate();
    };

    QObject::connect(
        screenDetector,
        qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
        &window,
        [screenDetector, screenVerifierStatus, launchScreenDetector](
            int, QProcess::ExitStatus
            ) {
            if (!screenDetector->property("restartWhenStopped").toBool()) {
                screenVerifierStatus->setText(
                    QStringLiteral("Screen verifier: stopped")
                    );
                return;
            }

            screenDetector->setProperty("restartWhenStopped", false);
            QTimer::singleShot(50, screenDetector, launchScreenDetector);
        }
        );

    QObject::connect(
        screenDetector,
        &QProcess::errorOccurred,
        &window,
        [screenVerifierStatus](QProcess::ProcessError) {
            screenVerifierStatus->setText(
                QStringLiteral("Screen verifier: unavailable")
                );
        }
        );

    auto submitOnlineResult = [networkManager, queueStatus, playerId](
                                  const AttemptRecord &attempt
                                  ) {
        QSettings settings;
        const QString sessionToken = settings.value(
                                                 QStringLiteral("matchmaking/sessionToken")
                                                 ).toString();

        if (sessionToken.isEmpty()) {
            queueStatus->setText(
                QStringLiteral("Finish saved locally: login required for submission")
                );
            return;
        }

        const QJsonObject body{
            {QStringLiteral("sessionToken"), sessionToken},
            {QStringLiteral("level"), attempt.level},
            {QStringLiteral("category"), attempt.category},
            {QStringLiteral("subcategory"), attempt.subcategory},
            {QStringLiteral("igtMilliseconds"),
             static_cast<double>(std::round(attempt.igtSeconds * 1000.0f))},
            {QStringLiteral("buildHash"), attempt.buildHash}
        };

        QNetworkRequest request(
            QUrl(QStringLiteral("https://api.trsr.app/matches/")
                 + playerId + QStringLiteral("/result"))
            );
        request.setHeader(
            QNetworkRequest::ContentTypeHeader,
            QStringLiteral("application/json")
            );

        auto *reply = networkManager->post(
            request,
            QJsonDocument(body).toJson(QJsonDocument::Compact)
            );

        QObject::connect(
            reply,
            &QNetworkReply::finished,
            [reply, queueStatus, playerId]() {
                const QJsonObject response =
                    QJsonDocument::fromJson(reply->readAll()).object();

                if (reply->error() != QNetworkReply::NoError) {
                    const QString error =
                        response.value(QStringLiteral("error")).toString();
                    queueStatus->setText(
                        error.isEmpty()
                            ? QStringLiteral("Finish saved locally: online submission failed")
                            : QStringLiteral("Finish saved locally: %1").arg(error)
                        );
                } else {
                    const QString winnerPlayerId =
                        response.value(QStringLiteral("winnerPlayerId")).toString();
                    if (!winnerPlayerId.isEmpty()) {
                        queueStatus->setText(
                            winnerPlayerId == playerId
                                ? QStringLiteral("Finish submitted — You win!")
                                : QStringLiteral("Finish submitted — Opponent wins")
                            );
                    } else {
                        queueStatus->setText(
                            response.value(QStringLiteral("status")).toString()
                                    == QStringLiteral("already_submitted")
                                ? QStringLiteral("Finish was already submitted")
                                : QStringLiteral("Finish submitted")
                            );
                    }
                }

                reply->deleteLater();
            }
            );
    };
    auto *matchPollTimer = new QTimer(&window);
    auto *matchPresenceTimer = new QTimer(&window);
    matchPresenceTimer->setInterval(2000);
    auto hasActiveMatch = std::make_shared<bool>(false);
    // A staging exit is a no-contest. Once either player enters the level,
    // the same control becomes a normal forfeit.
    auto matchStarted = std::make_shared<bool>(false);
    auto proposalNotified = std::make_shared<bool>(false);
    auto proposalAccepted = std::make_shared<bool>(false);
    auto latestPresence = std::make_shared<RunPresence>();
    auto autoStartPresence = std::make_shared<QString>();
    auto autoStartNavigationScheduled = std::make_shared<bool>(false);
    auto autoStartLaunchScheduled = std::make_shared<bool>(false);
    auto autoStartAwaitingRun = std::make_shared<bool>(false);
    auto *menuNavigator = new QProcess(&window);
    menuNavigator->setProcessChannelMode(QProcess::SeparateChannels);
    matchPollTimer->setInterval(1000);

    auto respondToMatchProposal =
        [networkManager, playerId, hasActiveMatch, proposalAccepted, proposalNotified,
         acceptMatchButton, declineMatchButton, queueStatus, opponentStatus,
         matchPollTimer, setQueueUiState, showQueueScreen, hideMatchPlayers](const QString &decision) {
            QSettings settings;
            const QString sessionToken = settings.value(
                QStringLiteral("matchmaking/sessionToken")).toString();
            if (sessionToken.isEmpty()) {
                queueStatus->setText(QStringLiteral("Match offer failed: login required"));
                return;
            }
            acceptMatchButton->setEnabled(false);
            declineMatchButton->setEnabled(false);
            const QJsonObject body{
                {QStringLiteral("sessionToken"), sessionToken},
                {QStringLiteral("decision"), decision}
            };
            QNetworkRequest request(QUrl(
                QStringLiteral("https://api.trsr.app/matches/") + playerId
                + QStringLiteral("/proposal")));
            request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
            auto *reply = networkManager->post(
                request, QJsonDocument(body).toJson(QJsonDocument::Compact));
            QObject::connect(reply, &QNetworkReply::finished,
                             [reply, decision, hasActiveMatch, proposalAccepted, proposalNotified,
                              acceptMatchButton, declineMatchButton, queueStatus, opponentStatus,
                              matchPollTimer, setQueueUiState, showQueueScreen,
                              hideMatchPlayers]() {
                const QJsonObject response = QJsonDocument::fromJson(reply->readAll()).object();
                if (reply->error() != QNetworkReply::NoError) {
                    const QString error = response.value(QStringLiteral("error")).toString();
                    queueStatus->setText(error.isEmpty()
                                             ? QStringLiteral("Match offer request failed")
                                             : QStringLiteral("Match offer: %1").arg(error));
                    acceptMatchButton->setEnabled(true);
                    declineMatchButton->setEnabled(true);
                    reply->deleteLater();
                    return;
                }
                const QString status = response.value(QStringLiteral("status")).toString();
                if (decision == QStringLiteral("accept")
                    && (status == QStringLiteral("accepted") || status == QStringLiteral("matched"))) {
                    *proposalAccepted = true;
                    queueStatus->setText(status == QStringLiteral("matched")
                                             ? QStringLiteral("Match accepted — revealing ruleset…")
                                             : QStringLiteral("Match accepted — waiting for opponent"));
                    opponentStatus->setText(QStringLiteral("Opponent: deciding"));
                    reply->deleteLater();
                    return;
                }
                *hasActiveMatch = false;
                *proposalAccepted = false;
                *proposalNotified = false;
                acceptMatchButton->hide();
                declineMatchButton->hide();
                hideMatchPlayers();
                matchPollTimer->stop();
                setQueueUiState(true, false, true);
                queueStatus->setText(status == QStringLiteral("expired")
                                         ? QStringLiteral("Match offer expired")
                                         : QStringLiteral("Match offer declined"));
                showQueueScreen();
                reply->deleteLater();
            });
        };

    QObject::connect(acceptMatchButton, &QPushButton::clicked, &window,
                     [respondToMatchProposal]() { respondToMatchProposal(QStringLiteral("accept")); });
    QObject::connect(declineMatchButton, &QPushButton::clicked, &window,
                     [respondToMatchProposal]() { respondToMatchProposal(QStringLiteral("decline")); });

    auto *healthReply = networkManager->get(
        QNetworkRequest(
            QUrl(QStringLiteral("https://api.trsr.app/health"))
            )
        );

    QObject::connect(
        logoutButton,
        &QPushButton::clicked,
        &window,
        [&window]() {
            QSettings settings;
            settings.remove(QStringLiteral("matchmaking/sessionToken"));
            settings.remove(QStringLiteral("matchmaking/speedrunUserId"));
            settings.sync();
            window.close();
        }
        );

    const QString savedSessionToken =
        settings.value(QStringLiteral("matchmaking/sessionToken")).toString();

    if (savedSessionToken.isEmpty()) {
        speedrunStatus->setText(
            QStringLiteral("Link your Speedrun.com account to continue.")
            );
    } else {
        QJsonObject sessionRequestBody;
        sessionRequestBody.insert(
            QStringLiteral("sessionToken"),
            savedSessionToken
            );

        QNetworkRequest sessionRequest{
            QUrl(QStringLiteral("https://api.trsr.app/auth/session"))
        };
        sessionRequest.setHeader(
            QNetworkRequest::ContentTypeHeader,
            QStringLiteral("application/json")
            );

        auto *sessionReply = networkManager->post(
            sessionRequest,
            QJsonDocument(sessionRequestBody).toJson(QJsonDocument::Compact)
            );

        QObject::connect(
            sessionReply,
            &QNetworkReply::finished,
            [sessionReply, speedrunStatus, accountStatus, adminButton, &loginDialog]() {
                const QJsonObject response =
                    QJsonDocument::fromJson(sessionReply->readAll()).object();

                if (sessionReply->error() != QNetworkReply::NoError
                    || response.value(QStringLiteral("status")).toString()
                           != QStringLiteral("valid")) {
                    QSettings settings;
                    settings.remove(QStringLiteral("matchmaking/sessionToken"));
                    settings.remove(QStringLiteral("matchmaking/speedrunUserId"));

                    speedrunStatus->setText(
                        QStringLiteral("Link your Speedrun.com account to continue.")
                        );
                    sessionReply->deleteLater();
                    return;
                }

                const QJsonObject profile =
                    response.value(QStringLiteral("profile")).toObject();
                const QString displayName =
                    profile.value(QStringLiteral("displayName")).toString();

                accountStatus->setText(
                    QStringLiteral("Speedrun.com: linked as %1")
                        .arg(displayName)
                    );
                speedrunStatus->setText(
                    QStringLiteral("Speedrun.com: linked as %1")
                        .arg(displayName)
                    );
                adminButton->setVisible(
                    displayName.compare(QStringLiteral("amedrii"), Qt::CaseInsensitive) == 0);

                loginDialog.accept();
                sessionReply->deleteLater();
            }
            );
    }

    QObject::connect(
        linkSpeedrunButton,
        &QPushButton::clicked,
        &loginDialog,
        [networkManager, speedrunApiKeyInput, linkSpeedrunButton,
         speedrunStatus, accountStatus, adminButton, &loginDialog]() {
            const QString apiKey = speedrunApiKeyInput->text().trimmed();

            if (apiKey.isEmpty()) {
                speedrunStatus->setText(
                    QStringLiteral("Speedrun.com: enter an API key first")
                    );
                return;
            }

            linkSpeedrunButton->setEnabled(false);
            speedrunStatus->setText(
                QStringLiteral("Speedrun.com: linking...")
                );

            QJsonObject requestBody;
            requestBody.insert(QStringLiteral("apiKey"), apiKey);

            QNetworkRequest request{
                QUrl(QStringLiteral("https://api.trsr.app/auth/speedrun"))
            };
            request.setHeader(
                QNetworkRequest::ContentTypeHeader,
                QStringLiteral("application/json")
                );

            auto *reply = networkManager->post(
                request,
                QJsonDocument(requestBody).toJson(QJsonDocument::Compact)
                );

            QObject::connect(
                reply,
                &QNetworkReply::finished,
                [reply, speedrunApiKeyInput, linkSpeedrunButton,
                 speedrunStatus, accountStatus, adminButton, &loginDialog]() {
                    const QJsonObject response =
                        QJsonDocument::fromJson(reply->readAll()).object();

                    if (reply->error() != QNetworkReply::NoError) {
                        const QString serverError =
                            response.value(QStringLiteral("error")).toString();

                        speedrunStatus->setText(
                            serverError.isEmpty()
                                ? QStringLiteral("Speedrun.com: %1").arg(reply->errorString())
                                : QStringLiteral("Speedrun.com: %1").arg(serverError)
                            );

                        linkSpeedrunButton->setEnabled(true);
                        reply->deleteLater();
                        return;
                    }

                    if (response.value(QStringLiteral("status")).toString()
                        != QStringLiteral("linked")) {
                        speedrunStatus->setText(
                            QStringLiteral("Speedrun.com: invalid API key")
                            );
                        linkSpeedrunButton->setEnabled(true);
                        reply->deleteLater();
                        return;
                    }

                    const QJsonObject profile =
                        response.value(QStringLiteral("profile")).toObject();

                    const QString displayName =
                        profile.value(QStringLiteral("displayName")).toString();

                    QSettings settings;
                    settings.setValue(
                        QStringLiteral("matchmaking/sessionToken"),
                        response.value(QStringLiteral("sessionToken")).toString()
                        );
                    settings.setValue(
                        QStringLiteral("matchmaking/speedrunUserId"),
                        profile.value(QStringLiteral("id")).toString()
                        );
                    speedrunApiKeyInput->clear();

                    speedrunStatus->setText(
                        QStringLiteral("Speedrun.com: linked as %1")
                            .arg(displayName)
                        );

                    accountStatus->setText(
                        QStringLiteral("Speedrun.com: linked as %1")
                            .arg(displayName)
                        );
                    adminButton->setVisible(
                        displayName.compare(QStringLiteral("amedrii"), Qt::CaseInsensitive) == 0);

                    loginDialog.accept();

                    reply->deleteLater();
                }
                );
        }
        );

    QObject::connect(
        healthReply,
        &QNetworkReply::finished,
        &window,
        [healthReply, serverStatus]() {
            if (healthReply->error() == QNetworkReply::NoError) {
                serverStatus->setText(
                    QStringLiteral("Matchmaking server: connected")
                    );
            } else {
                serverStatus->setText(
                    QStringLiteral("Matchmaking server: offline")
                    );
            }

            healthReply->deleteLater();
        }
        );

    auto navigatorOutput = std::make_shared<QByteArray>();
    QObject::connect(
        menuNavigator,
        &QProcess::readyReadStandardOutput,
        &window,
        [menuNavigator, navigatorOutput, autoStartPresence, autoStartStatus,
         readyButton]() {
            *navigatorOutput += menuNavigator->readAllStandardOutput();
            while (true) {
                const int newline = navigatorOutput->indexOf('\n');
                if (newline < 0) {
                    break;
                }
                const QByteArray line = navigatorOutput->left(newline);
                navigatorOutput->remove(0, newline + 1);
                const QJsonObject event = QJsonDocument::fromJson(line).object();
                const QString name = event.value(QStringLiteral("event")).toString();
                if (name == QStringLiteral("replay_settings_verified")) {
                    *autoStartPresence = QStringLiteral("ready");
                    readyButton->setEnabled(false);
                    autoStartStatus->setText(
                        QStringLiteral("Ready: waiting for opponent's menu check")
                        );
                } else if (name == QStringLiteral("menu_navigation_failed")) {
                    autoStartPresence->clear();
                    readyButton->setEnabled(true);
                    autoStartStatus->setText(
                        QStringLiteral("Ready check failed: %1")
                            .arg(event.value(QStringLiteral("error")).toString())
                        );
                }
            }
        }
        );

    QObject::connect(
        menuNavigator,
        qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
        &window,
        [autoStartPresence, autoStartStatus, readyButton]() {
            if (*autoStartPresence == QStringLiteral("checking_replay_settings")) {
                autoStartPresence->clear();
                readyButton->setEnabled(true);
                autoStartStatus->setText(
                    QStringLiteral("Ready check ended before Replay Settings was verified")
                    );
            }
        }
        );

    QObject::connect(
        readyButton,
        &QPushButton::clicked,
        &window,
        [menuNavigator, hasActiveMatch, levelSelector, autoStartPresence,
         autoStartStatus, readyButton]() {
            if (!*hasActiveMatch || menuNavigator->state() != QProcess::NotRunning) {
                return;
            }
            const QDir appDirectory(QCoreApplication::applicationDirPath());
            const QString bundledNavigator = appDirectory.filePath(
                QStringLiteral("tools/menu_navigator/menu_navigator.exe"));
            const QString scriptPath = appDirectory.filePath(
                QStringLiteral("tools/menu_navigator.py"));
            if (!QFileInfo::exists(bundledNavigator) && !QFileInfo::exists(scriptPath)) {
                autoStartStatus->setText(QStringLiteral("Ready check unavailable: verifier file is missing"));
                return;
            }
            *autoStartPresence = QStringLiteral("checking_replay_settings");
            readyButton->setEnabled(false);
            autoStartStatus->setText(
                QStringLiteral("Checking Replay Settings — keep Start Game selected…"));
            QString program;
            QStringList arguments{
                QStringLiteral("--level"), levelSelector->currentText(),
                QStringLiteral("--verify-replay-settings")
            };
            if (QFileInfo::exists(bundledNavigator)) {
                program = bundledNavigator;
            } else {
                program = QStringLiteral("py");
                arguments.prepend(scriptPath);
                arguments.prepend(QStringLiteral("-3.13"));
            }
            QString tesseractPath = appDirectory.filePath(
                QStringLiteral("tools/tesseract/tesseract.exe"));
            if (!QFileInfo::exists(tesseractPath)) {
                tesseractPath = QStringLiteral("C:/Program Files/Tesseract-OCR/tesseract.exe");
            }
            if (QFileInfo::exists(tesseractPath)) {
                arguments << QStringLiteral("--tesseract") << tesseractPath;
            }
            menuNavigator->start(program, arguments);
        }
        );
    QObject::connect(
        forfeitButton,
        &QPushButton::clicked,
        &window,
        [networkManager, playerId, hasActiveMatch, matchStarted, autoStartPresence, menuNavigator,
         forfeitButton, readyButton, queueStatus, matchPresenceTimer, setQueueUiState,
         levelSelector, categorySelector, subcategorySelector, resultTitle, resultRuleset,
         resultDetail, showResultsScreen, showQueueScreen, &window]() {
            if (!*hasActiveMatch) {
                return;
            }
            const bool leavingStaging = !*matchStarted;
            if (QMessageBox::question(
                    &window,
                    leavingStaging ? QStringLiteral("Leave staging?")
                                   : QStringLiteral("Forfeit match?"),
                    leavingStaging
                        ? QStringLiteral("This cancels the match before it starts. No result is recorded.")
                        : QStringLiteral("This immediately awards the match to your opponent."),
                    QMessageBox::Yes | QMessageBox::No,
                    QMessageBox::No
                    ) != QMessageBox::Yes) {
                return;
            }
            QSettings settings;
            const QString sessionToken = settings.value(
                                                     QStringLiteral("matchmaking/sessionToken")).toString();
            if (sessionToken.isEmpty()) {
                queueStatus->setText(leavingStaging
                                         ? QStringLiteral("Leave staging failed: login required")
                                         : QStringLiteral("Forfeit failed: login required"));
                return;
            }
            forfeitButton->setEnabled(false);
            const QJsonObject body{{QStringLiteral("sessionToken"), sessionToken}};
            QNetworkRequest request(
                QUrl(QStringLiteral("https://api.trsr.app/matches/")
                     + playerId + (leavingStaging
                                       ? QStringLiteral("/cancel")
                                       : QStringLiteral("/forfeit")))
                );
            request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
            auto *reply = networkManager->post(
                request, QJsonDocument(body).toJson(QJsonDocument::Compact));
            QObject::connect(
                reply, &QNetworkReply::finished,
                [reply, leavingStaging, hasActiveMatch, matchStarted, autoStartPresence, menuNavigator, forfeitButton,
                 readyButton, queueStatus, matchPresenceTimer, setQueueUiState,
                 levelSelector, categorySelector, subcategorySelector, resultTitle,
                 resultRuleset, resultDetail, showResultsScreen, showQueueScreen]() {
                    const QJsonObject response =
                        QJsonDocument::fromJson(reply->readAll()).object();
                    if (reply->error() != QNetworkReply::NoError) {
                        const QString serverError = response.value(QStringLiteral("error")).toString();
                        queueStatus->setText(
                            (leavingStaging ? QStringLiteral("Leave staging failed: ")
                                            : QStringLiteral("Forfeit failed: "))
                            + (serverError.isEmpty() ? reply->errorString() : serverError));
                        forfeitButton->setEnabled(true);
                        reply->deleteLater();
                        return;
                    }
                    *hasActiveMatch = false;
                    *matchStarted = false;
                    autoStartPresence->clear();
                    if (menuNavigator->state() != QProcess::NotRunning) {
                        menuNavigator->terminate();
                    }
                    matchPresenceTimer->stop();
                    readyButton->setEnabled(false);
                    forfeitButton->setEnabled(false);
                    setQueueUiState(true, false, true);
                    if (leavingStaging) {
                        queueStatus->setText(QStringLiteral("Match cancelled before start — no contest"));
                        showQueueScreen();
                        reply->deleteLater();
                        return;
                    }
                    queueStatus->setText(
                        response.value(QStringLiteral("status")).toString() == QStringLiteral("forfeited")
                            ? QStringLiteral("Match forfeited — opponent wins")
                            : QStringLiteral("Match was already decided")
                        );
                    resultTitle->setText(
                        response.value(QStringLiteral("status")).toString() == QStringLiteral("forfeited")
                            ? QStringLiteral("Match forfeited")
                            : QStringLiteral("Match already decided"));
                    resultRuleset->setText(
                        QStringLiteral("%1 — %2 — %3")
                            .arg(levelSelector->currentText())
                            .arg(categorySelector->currentText())
                            .arg(subcategorySelector->currentText()));
                    resultDetail->setText(
                        response.value(QStringLiteral("status")).toString() == QStringLiteral("forfeited")
                            ? QStringLiteral("Opponent wins")
                            : QStringLiteral("Check the result before requeueing."));
                    showResultsScreen();
                    reply->deleteLater();
                }
                );
        }
        );

    QObject::connect(
        matchPresenceTimer,
        &QTimer::timeout,
        &window,
        [networkManager, playerId, opponentStatus, readyButton, forfeitButton,
         autoStartStatus, menuNavigator, hasActiveMatch, matchStarted, latestPresence,
         autoStartPresence, autoStartNavigationScheduled, autoStartLaunchScheduled,
         autoStartAwaitingRun, matchPresenceTimer, queueStatus, setQueueUiState,
         levelSelector, categorySelector, subcategorySelector, resultTitle, resultRuleset,
         resultDetail, showResultsScreen, showQueueScreen, &window]() {
            if (!*hasActiveMatch) {
                return;
            }
            const bool localRunActive = latestPresence->state == QStringLiteral("running")
                                        || latestPresence->state == QStringLiteral("restarted");
            if (localRunActive && *autoStartPresence == QStringLiteral("ready")) {
                autoStartPresence->clear();
                autoStartStatus->setText(QStringLiteral("Run started — IGT duel in progress"));
            }
            readyButton->setEnabled(autoStartPresence->isEmpty()
                                    && !localRunActive
                                    && menuNavigator->state() == QProcess::NotRunning);
            forfeitButton->setEnabled(true);
            forfeitButton->setText(*matchStarted
                                       ? QStringLiteral("Forfeit match")
                                       : QStringLiteral("Leave staging (no contest)"));

            QSettings settings;
            const QString sessionToken = settings.value(
                                                     QStringLiteral("matchmaking/sessionToken")).toString();
            if (sessionToken.isEmpty()) {
                return;
            }

            const QString presenceState = *autoStartPresence == QStringLiteral("ready")
                                              ? QStringLiteral("ready")
                                              : latestPresence->state;
            const QJsonObject body{
                {QStringLiteral("sessionToken"), sessionToken},
                {QStringLiteral("state"), presenceState},
                {QStringLiteral("igtMilliseconds"),
                 latestPresence->igtSeconds < 0.0f
                     ? QJsonValue()
                     : QJsonValue(static_cast<double>(std::round(
                           latestPresence->igtSeconds * 1000.0f)))}
            };
            QNetworkRequest request(
                QUrl(QStringLiteral("https://api.trsr.app/matches/")
                     + playerId + QStringLiteral("/presence"))
                );
            request.setHeader(QNetworkRequest::ContentTypeHeader,
                              QStringLiteral("application/json"));
            auto *reply = networkManager->post(
                request, QJsonDocument(body).toJson(QJsonDocument::Compact));

            QObject::connect(
                reply,
                &QNetworkReply::finished,
                [reply, playerId, opponentStatus, autoStartStatus, menuNavigator,
                 autoStartPresence, autoStartNavigationScheduled, autoStartLaunchScheduled,
                 latestPresence, autoStartAwaitingRun, hasActiveMatch, matchStarted, matchPresenceTimer,
                 readyButton, forfeitButton, queueStatus, setQueueUiState, levelSelector,
                 categorySelector, subcategorySelector, resultTitle, resultRuleset,
                 resultDetail, showResultsScreen, showQueueScreen, &window]() {
                    if (!*hasActiveMatch) {
                        reply->deleteLater();
                        return;
                    }
                    const QJsonObject response =
                        QJsonDocument::fromJson(reply->readAll()).object();
                    if (reply->error() != QNetworkReply::NoError) {
                        if (response.value(QStringLiteral("status")).toString()
                            == QStringLiteral("start_rejected")) {
                            autoStartPresence->clear();
                            readyButton->setEnabled(true);
                            autoStartStatus->setText(
                                QStringLiteral("Start rejected: %1 Return to Replay Settings and Ready again.")
                                    .arg(response.value(QStringLiteral("error")).toString()));
                        }
                        opponentStatus->setText(
                            QStringLiteral("Opponent: status unavailable"));
                        reply->deleteLater();
                        return;
                    }

                    const QString winnerPlayerId =
                        response.value(QStringLiteral("winnerPlayerId")).toString();
                    const QJsonArray players =
                        response.value(QStringLiteral("players")).toArray();
                    bool anyPlayerStarted = false;
                    for (const QJsonValue &player : players) {
                        if (!player.toObject().value(QStringLiteral("startedAtUtc")).toString().isEmpty()) {
                            anyPlayerStarted = true;
                            break;
                        }
                    }
                    *matchStarted = anyPlayerStarted;
                    forfeitButton->setText(anyPlayerStarted
                                               ? QStringLiteral("Forfeit match")
                                               : QStringLiteral("Leave staging (no contest)"));
                    if (response.value(QStringLiteral("status")).toString() == QStringLiteral("none")) {
                        *hasActiveMatch = false;
                        *matchStarted = false;
                        matchPresenceTimer->stop();
                        autoStartPresence->clear();
                        readyButton->setEnabled(false);
                        forfeitButton->setEnabled(false);
                        setQueueUiState(true, false, true);
                        queueStatus->setText(QStringLiteral("Match cancelled before start — no contest"));
                        showQueueScreen();
                        reply->deleteLater();
                        return;
                    }
                    if (!winnerPlayerId.isEmpty()) {
                        *hasActiveMatch = false;
                        *matchStarted = false;
                        matchPresenceTimer->stop();
                        autoStartPresence->clear();
                        *autoStartNavigationScheduled = false;
                        *autoStartLaunchScheduled = false;
                        *autoStartAwaitingRun = false;
                        if (menuNavigator->state() != QProcess::NotRunning) {
                            menuNavigator->terminate();
                        }
                        readyButton->setEnabled(false);
                        forfeitButton->setEnabled(false);
                        setQueueUiState(true, false, true);
                        const bool won = winnerPlayerId == playerId;
                        playClientNotification(won ? ClientNotification::Victory
                                                   : ClientNotification::Defeat);
                        bringMatchWindowToFront(window);
                        opponentStatus->setText(
                            won ? QStringLiteral("Result: You win")
                                : QStringLiteral("Result: Opponent wins"));
                        queueStatus->setText(
                            won ? QStringLiteral("Match complete - You win")
                                : QStringLiteral("Match complete - Opponent wins"));
                        double yourMilliseconds = -1.0;
                        double opponentMilliseconds = -1.0;
                        for (const QJsonValue &player : players) {
                            const QJsonObject candidate = player.toObject();
                            const double milliseconds = candidate.value(
                                                                     QStringLiteral("igtMilliseconds")).toDouble(-1.0);
                            if (candidate.value(QStringLiteral("playerId")).toString() == playerId) {
                                yourMilliseconds = milliseconds;
                            } else {
                                opponentMilliseconds = milliseconds;
                            }
                        }
                        resultTitle->setText(won ? QStringLiteral("Victory")
                                                 : QStringLiteral("Defeat"));
                        resultRuleset->setText(
                            QStringLiteral("%1 — %2 — %3")
                                .arg(levelSelector->currentText())
                                .arg(categorySelector->currentText())
                                .arg(subcategorySelector->currentText()));
                        const bool bothTimesAvailable = yourMilliseconds >= 0.0
                                                        && opponentMilliseconds >= 0.0;
                        QString resultText = bothTimesAvailable
                                                 ? (won ? QStringLiteral("You won on IGT")
                                                        : QStringLiteral("Opponent won on IGT"))
                                                 : (won ? QStringLiteral("Opponent did not finish in time")
                                                        : QStringLiteral("You did not finish in time"));
                        if (yourMilliseconds >= 0.0 || opponentMilliseconds >= 0.0) {
                            resultText += QStringLiteral("\nYou: %1   Opponent: %2")
                            .arg(yourMilliseconds >= 0.0
                                     ? formatIgt(static_cast<float>(yourMilliseconds / 1000.0))
                                     : QStringLiteral("—"))
                                .arg(opponentMilliseconds >= 0.0
                                         ? formatIgt(static_cast<float>(opponentMilliseconds / 1000.0))
                                         : QStringLiteral("—"));
                        }
                        resultDetail->setText(resultText);
                        showResultsScreen();
                        reply->deleteLater();
                        return;
                    }
                    QString opponentState;
                    QJsonObject opponent;
                    for (const QJsonValue &player : players) {
                        const QJsonObject candidate = player.toObject();
                        if (candidate.value(QStringLiteral("playerId")).toString()
                            != playerId) {
                            opponent = candidate;
                            break;
                        }
                    }

                    if (opponent.isEmpty()) {
                        opponentStatus->setText(
                            QStringLiteral("Opponent: waiting for opponent"));
                    } else {
                        const QString state =
                            opponent.value(QStringLiteral("state")).toString();
                        opponentState = state;
                        const double milliseconds =
                            opponent.value(QStringLiteral("igtMilliseconds")).toDouble(-1.0);
                        QString detail;
                        if (state == QStringLiteral("ready")) {
                            detail = QStringLiteral("Ready");
                        } else if (state == QStringLiteral("arming")) {
                            detail = QStringLiteral("Navigating menus");
                        } else if (state == QStringLiteral("armed")) {
                            detail = QStringLiteral("Armed");
                        } else if (state == QStringLiteral("running")) {
                            detail = QStringLiteral("Running");
                        } else if (state == QStringLiteral("restarted")) {
                            detail = QStringLiteral("Restarted");
                        } else if (state == QStringLiteral("finished")) {
                            detail = QStringLiteral("Finished");
                        } else {
                            detail = QStringLiteral("Waiting to start");
                        }
                        if (milliseconds >= 0.0) {
                            detail += QStringLiteral(" — ")
                                      + formatIgt(static_cast<float>(milliseconds / 1000.0));
                        }
                        opponentStatus->setText(
                            QStringLiteral("Opponent: %1").arg(detail));
                    }
                    const int goDelayMilliseconds = response.value(
                                                                QStringLiteral("navigationDelayMilliseconds")).toInt(-1);
                    if (*autoStartPresence == QStringLiteral("ready")) {
                        if (goDelayMilliseconds > 0) {
                            autoStartStatus->setText(
                                QStringLiteral("Both players ready — GO in %1 seconds")
                                    .arg((goDelayMilliseconds + 999) / 1000));
                        } else if (!response.value(QStringLiteral("navigationStartAtUtc"))
                                        .toString().isEmpty()) {
                            autoStartStatus->setText(
                                QStringLiteral("GO — start the selected level now"));
                        }
                    }

                    const QString finishDeadlineUtc = response.value(
                                                                  QStringLiteral("finishDeadlineUtc")).toString();
                    if (!finishDeadlineUtc.isEmpty()) {
                        const qint64 remainingMilliseconds = QDateTime::fromString(
                                                                 finishDeadlineUtc, Qt::ISODateWithMs).toMSecsSinceEpoch()
                                                             - QDateTime::currentDateTimeUtc().toMSecsSinceEpoch();
                        const int remainingSeconds = std::max(
                            0, static_cast<int>((remainingMilliseconds + 999) / 1000));
                        const bool youFinishedFirst = response.value(
                                                                  QStringLiteral("firstFinisherPlayerId")).toString() == playerId;
                        autoStartStatus->setText(
                            youFinishedFirst
                                ? QStringLiteral("Finish submitted — comparison window: %1 seconds")
                                      .arg(remainingSeconds)
                                : QStringLiteral("Opponent finished — submit within %1 seconds to compare IGT")
                                      .arg(remainingSeconds));
                    }
                    reply->deleteLater();
                }
                );
        }
        );
    QObject::connect(
        matchPollTimer,
        &QTimer::timeout,
        &window,
        [networkManager, matchPollTimer, matchPresenceTimer, queueStatus, opponentStatus,
         acceptMatchButton, declineMatchButton, setQueueUiState, levelSelector,
         categorySelector, subcategorySelector, playerId, hasActiveMatch, matchStarted,
         proposalNotified, proposalAccepted, showQueueScreen, showMatchPlayers,
         hideMatchPlayers, &window]() {
            auto *reply = networkManager->get(QNetworkRequest(QUrl(
                QStringLiteral("https://api.trsr.app/matches/") + playerId)));
            QObject::connect(
                reply, &QNetworkReply::finished,
                [reply, matchPollTimer, matchPresenceTimer, queueStatus, opponentStatus,
                 acceptMatchButton, declineMatchButton, setQueueUiState, levelSelector,
                 categorySelector, subcategorySelector, playerId, hasActiveMatch, matchStarted,
                 proposalNotified, proposalAccepted, showQueueScreen, showMatchPlayers,
                 hideMatchPlayers, &window]() {
                    if (reply->error() != QNetworkReply::NoError) {
                        reply->deleteLater();
                        return;
                    }
                    const QJsonObject response =
                        QJsonDocument::fromJson(reply->readAll()).object();
                    const QString responseStatus = response.value(QStringLiteral("status")).toString();
                    if (responseStatus == QStringLiteral("proposal")) {
                        *hasActiveMatch = false;
                        matchPresenceTimer->stop();
                        hideMatchPlayers();
                        const QJsonArray acceptedPlayers = response.value(
                            QStringLiteral("acceptedPlayerIds")).toArray();
                        const bool youAccepted = std::any_of(
                            acceptedPlayers.cbegin(), acceptedPlayers.cend(),
                            [&playerId](const QJsonValue &value) {
                                return value.toString() == playerId;
                            });
                        *proposalAccepted = youAccepted;
                        acceptMatchButton->setVisible(true);
                        declineMatchButton->setVisible(true);
                        acceptMatchButton->setEnabled(!youAccepted);
                        declineMatchButton->setEnabled(!youAccepted);
                        if (!*proposalNotified) {
                            *proposalNotified = true;
                            playClientNotification(ClientNotification::MatchFound);
                            bringMatchWindowToFront(window);
                        }
                        const QDateTime expiresAt = QDateTime::fromString(
                            response.value(QStringLiteral("proposalExpiresAtUtc")).toString(),
                            Qt::ISODateWithMs);
                        const int secondsLeft = expiresAt.isValid()
                            ? std::max(0, static_cast<int>(
                                  (QDateTime::currentDateTimeUtc().msecsTo(expiresAt) + 999) / 1000))
                            : 0;
                        queueStatus->setText(youAccepted
                            ? QStringLiteral("Match accepted — waiting for opponent (%1 seconds)")
                                  .arg(secondsLeft)
                            : QStringLiteral("Match found — accept within %1 seconds")
                                  .arg(secondsLeft));
                        opponentStatus->setText(youAccepted
                            ? QStringLiteral("Opponent: deciding")
                            : QStringLiteral("Opponent: match offer pending"));
                        setQueueUiState(false, false, false);
                        reply->deleteLater();
                        return;
                    }
                    if (responseStatus != QStringLiteral("matched")) {
                        if (*proposalNotified) {
                            *proposalNotified = false;
                            *proposalAccepted = false;
                            acceptMatchButton->hide();
                            declineMatchButton->hide();
                            hideMatchPlayers();
                            matchPollTimer->stop();
                            setQueueUiState(true, false, true);
                            queueStatus->setText(responseStatus == QStringLiteral("expired")
                                ? QStringLiteral("Match offer expired")
                                : QStringLiteral("Match offer no longer available"));
                            showQueueScreen();
                        }
                        reply->deleteLater();
                        return;
                    }
                    const QJsonObject match =
                        response.value(QStringLiteral("match")).toObject();
                    showMatchPlayers(match.value(QStringLiteral("players")).toArray());
                    levelSelector->setCurrentText(match.value(QStringLiteral("level")).toString());
                    categorySelector->setCurrentText(match.value(QStringLiteral("category")).toString());
                    subcategorySelector->setCurrentText(match.value(QStringLiteral("subcategory")).toString());
                    queueStatus->setText(
                        QStringLiteral("Match found: %1 — %2 — %3")
                            .arg(match.value(QStringLiteral("level")).toString())
                            .arg(match.value(QStringLiteral("category")).toString())
                            .arg(match.value(QStringLiteral("subcategory")).toString()));
                    matchPollTimer->stop();
                    *hasActiveMatch = true;
                    *matchStarted = false;
                    *proposalNotified = false;
                    *proposalAccepted = false;
                    acceptMatchButton->hide();
                    declineMatchButton->hide();
                    matchPresenceTimer->start();
                    setQueueUiState(false, false, false);
                    reply->deleteLater();
                });
        });

    QObject::connect(
        cancelQueueButton, &QPushButton::clicked, &window,
        [networkManager, matchPollTimer, queueStatus, setQueueUiState, playerId]() {
            QSettings settings;
            const QString sessionToken = settings.value(
                                                     QStringLiteral("matchmaking/sessionToken")).toString();
            if (sessionToken.isEmpty()) {
                queueStatus->setText(QStringLiteral("Matchmaking: link Speedrun.com first"));
                return;
            }
            setQueueUiState(false, false, false);
            const QJsonObject requestBody{
                {QStringLiteral("playerId"), playerId},
                {QStringLiteral("sessionToken"), sessionToken}
            };
            QNetworkRequest request(QUrl(QStringLiteral("https://api.trsr.app/queue/cancel")));
            request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
            auto *reply = networkManager->post(
                request, QJsonDocument(requestBody).toJson(QJsonDocument::Compact));
            QObject::connect(
                reply, &QNetworkReply::finished,
                [reply, matchPollTimer, queueStatus, setQueueUiState]() {
                    if (reply->error() != QNetworkReply::NoError) {
                        queueStatus->setText(QStringLiteral("Matchmaking: could not cancel queue"));
                        setQueueUiState(false, true, false);
                        reply->deleteLater();
                        return;
                    }
                    matchPollTimer->stop();
                    queueStatus->setText(QStringLiteral("Matchmaking: queue cancelled"));
                    setQueueUiState(true, false, true);
                    reply->deleteLater();
                });
        });

    QObject::connect(
        randomQueueButton, &QPushButton::clicked, &window,
        [networkManager, queueStatus, setQueueUiState, levelSelector,
         categorySelector, subcategorySelector, matchPollTimer, matchPresenceTimer,
         playerId, hasActiveMatch, matchStarted, hideMatchPlayers]() {
            QSettings settings;
            const QString sessionToken = settings.value(
                                                     QStringLiteral("matchmaking/sessionToken")).toString();
            if (sessionToken.isEmpty()) {
                queueStatus->setText(QStringLiteral("Matchmaking: link Speedrun.com first"));
                return;
            }
            setQueueUiState(false, false, false);
            hideMatchPlayers();
            queueStatus->setText(QStringLiteral("Matchmaking: joining random queue..."));
            const QJsonObject requestBody{
                {QStringLiteral("playerId"), playerId},
                {QStringLiteral("sessionToken"), sessionToken},
                {QStringLiteral("queueType"), QStringLiteral("random")}
            };
            QNetworkRequest request(QUrl(QStringLiteral("https://api.trsr.app/queue")));
            request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
            auto *reply = networkManager->post(
                request, QJsonDocument(requestBody).toJson(QJsonDocument::Compact));
            QObject::connect(
                reply, &QNetworkReply::finished,
                [reply, queueStatus, setQueueUiState, levelSelector,
                 categorySelector, subcategorySelector, matchPollTimer, matchPresenceTimer,
                 hasActiveMatch, matchStarted]() {
                    if (reply->error() != QNetworkReply::NoError) {
                        queueStatus->setText(QStringLiteral("Matchmaking: server request failed"));
                        setQueueUiState(true, false, true);
                        reply->deleteLater();
                        return;
                    }
                    const QJsonObject response = QJsonDocument::fromJson(reply->readAll()).object();
                    const QString responseStatus = response.value(QStringLiteral("status")).toString();
                    if (responseStatus == QStringLiteral("waiting")) {
                        queueStatus->setText(QStringLiteral("Matchmaking: waiting for opponent"));
                        setQueueUiState(false, true, false);
                        matchPollTimer->start();
                    } else if (responseStatus == QStringLiteral("proposal")) {
                        queueStatus->setText(QStringLiteral("Match found — checking offer..."));
                        setQueueUiState(false, false, false);
                        matchPollTimer->start();
                    } else if (responseStatus == QStringLiteral("matched")) {
                        const QJsonObject match = response.value(QStringLiteral("match")).toObject();
                        levelSelector->setCurrentText(match.value(QStringLiteral("level")).toString());
                        categorySelector->setCurrentText(match.value(QStringLiteral("category")).toString());
                        subcategorySelector->setCurrentText(match.value(QStringLiteral("subcategory")).toString());
                        queueStatus->setText(
                            QStringLiteral("Match found: %1 — %2 — %3")
                                .arg(match.value(QStringLiteral("level")).toString())
                                .arg(match.value(QStringLiteral("category")).toString())
                                .arg(match.value(QStringLiteral("subcategory")).toString()));
                        matchPollTimer->stop();
                        *hasActiveMatch = true;
                        *matchStarted = false;
                        matchPresenceTimer->start();
                        setQueueUiState(false, false, false);
                    } else {
                        queueStatus->setText(QStringLiteral("Matchmaking: ")
                                             + response.value(QStringLiteral("error")).toString());
                        setQueueUiState(true, false, true);
                    }
                    reply->deleteLater();
                });
        });

    QObject::connect(
        rulesetQueueButton, &QPushButton::clicked, &window,
        [networkManager, queueStatus, setQueueUiState, levelSelector,
         categorySelector, subcategorySelector, matchPollTimer, matchPresenceTimer,
         playerId, hasActiveMatch, matchStarted, hideMatchPlayers]() {
            QSettings settings;
            const QString sessionToken = settings.value(
                                                     QStringLiteral("matchmaking/sessionToken")).toString();
            if (sessionToken.isEmpty()) {
                queueStatus->setText(QStringLiteral("Matchmaking: link Speedrun.com first"));
                return;
            }
            setQueueUiState(false, false, false);
            hideMatchPlayers();
            queueStatus->setText(
                QStringLiteral("Matchmaking: joining selected ruleset queue..."));
            const QJsonObject requestBody{
                {QStringLiteral("playerId"), playerId},
                {QStringLiteral("sessionToken"), sessionToken},
                {QStringLiteral("queueType"), QStringLiteral("ruleset")},
                {QStringLiteral("level"), levelSelector->currentText()},
                {QStringLiteral("category"), categorySelector->currentText()},
                {QStringLiteral("subcategory"), subcategorySelector->currentText()}
            };
            QNetworkRequest request(QUrl(QStringLiteral("https://api.trsr.app/queue")));
            request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
            auto *reply = networkManager->post(
                request, QJsonDocument(requestBody).toJson(QJsonDocument::Compact));
            QObject::connect(
                reply, &QNetworkReply::finished,
                [reply, queueStatus, setQueueUiState, levelSelector,
                 categorySelector, subcategorySelector, matchPollTimer, matchPresenceTimer,
                 hasActiveMatch, matchStarted]() {
                    if (reply->error() != QNetworkReply::NoError) {
                        queueStatus->setText(QStringLiteral("Matchmaking: server request failed"));
                        setQueueUiState(true, false, true);
                        reply->deleteLater();
                        return;
                    }
                    const QJsonObject response = QJsonDocument::fromJson(reply->readAll()).object();
                    const QString responseStatus = response.value(QStringLiteral("status")).toString();
                    if (responseStatus == QStringLiteral("waiting")) {
                        queueStatus->setText(
                            QStringLiteral("Matchmaking: waiting for ruleset opponent"));
                        setQueueUiState(false, true, false);
                        matchPollTimer->start();
                    } else if (responseStatus == QStringLiteral("proposal")) {
                        queueStatus->setText(QStringLiteral("Match found — checking offer..."));
                        setQueueUiState(false, false, false);
                        matchPollTimer->start();
                    } else if (responseStatus == QStringLiteral("matched")) {
                        const QJsonObject match = response.value(QStringLiteral("match")).toObject();
                        levelSelector->setCurrentText(match.value(QStringLiteral("level")).toString());
                        categorySelector->setCurrentText(match.value(QStringLiteral("category")).toString());
                        subcategorySelector->setCurrentText(match.value(QStringLiteral("subcategory")).toString());
                        queueStatus->setText(
                            QStringLiteral("Match found: %1 — %2 — %3")
                                .arg(match.value(QStringLiteral("level")).toString())
                                .arg(match.value(QStringLiteral("category")).toString())
                                .arg(match.value(QStringLiteral("subcategory")).toString()));
                        matchPollTimer->stop();
                        *hasActiveMatch = true;
                        *matchStarted = false;
                        matchPresenceTimer->start();
                        setQueueUiState(false, false, false);
                    } else {
                        queueStatus->setText(QStringLiteral("Matchmaking: ")
                                             + response.value(QStringLiteral("error")).toString());
                        setQueueUiState(true, false, true);
                    }
                    reply->deleteLater();
                });
        });
    QObject::connect(
        levelSelector,
        &QComboBox::currentTextChanged,
        &window,
        [refreshSelection](const QString &) {
            refreshSelection();
        }
        );

    QObject::connect(
        categorySelector,
        &QComboBox::currentTextChanged,
        &window,
        [refreshSelection](const QString &) {
            refreshSelection();
        }
        );

    QObject::connect(
        subcategorySelector,
        &QComboBox::currentTextChanged,
        &window,
        [refreshSelection](const QString &) {
            refreshSelection();
        }
        );

    // Ranked RTA starts and resumes on observed game frames. A precise 60 Hz
    // monitor bounds external read-only timing uncertainty to roughly one frame.
    auto runMonitor = std::make_shared<RunMonitor>();
    auto updateStatus =
        [status, levelSelector, categorySelector, subcategorySelector, submitOnlineResult,
         runMonitor, latestPresence, hasActiveMatch, screens, queuePage, matchPage,
         runPage, showQueueScreen, showMatchScreen, showRunScreen]() {
            const RunSelection selection{
                levelSelector->currentIndex(),
                levelSelector->currentText(),
                categorySelector->currentText(),
                subcategorySelector->currentText()
            };

            status->setText(runMonitor->update(selection));
            *latestPresence = runMonitor->presence();
            if (latestPresence->state == QStringLiteral("running")
                || latestPresence->state == QStringLiteral("restarted")) {
                showRunScreen();
            } else if (screens->currentWidget() == runPage) {
                if (*hasActiveMatch) {
                    showMatchScreen();
                } else {
                    showQueueScreen();
                }
            }

            if (const auto savedAttempt = runMonitor->takeSavedAttempt()) {
                submitOnlineResult(*savedAttempt);
            }
        };
    QTimer timer;
    timer.setTimerType(Qt::PreciseTimer);
    QObject::connect(&timer, &QTimer::timeout, &window, updateStatus);

    loginDialog.resize(380, 180);

    if (loginDialog.exec() != QDialog::Accepted) {
        return 0;
    }

    QObject::connect(
        levelSelector,
        &QComboBox::currentTextChanged,
        &window,
        restartScreenDetector
        );

    restartScreenDetector(levelSelector->currentText());
    timer.start(16);
    updateStatus();

    window.resize(440, 480);
    window.show();

    return app.exec();
}
