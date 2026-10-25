#include "run_monitor.h"

#include "attempt_log.h"
#include "run_rules.h"
#include "tra_process.h"

// Save a verified completed attempt locally until results are submitted to the
// matchmaking service.
static bool saveCompletedAttempt(
    const QString &level, const QString &category, const QString &subcategory,
    float igt, float rtaNoLoad, const QString &buildHash, QString *error
    )
{
    return appendAttempt(
        AttemptRecord{level, category, subcategory, igt, rtaNoLoad, buildHash}, error
        );
}

std::optional<AttemptRecord> RunMonitor::takeSavedAttempt()
{
    std::optional<AttemptRecord> attempt = m_savedAttempt;
    m_savedAttempt.reset();
    return attempt;
}

RunPresence RunMonitor::presence() const
{
    return m_presence;
}

QString RunMonitor::update(const RunSelection &selection)
{
            const auto process = findTraProcess();

            if (!process.has_value()) {
                m_previousGameTime = -1.0f;
                m_unchangedPolls = 0;
                m_levelVerifiedThisRun = false;
                m_finishHandled = false;
                m_finishMessage.clear();
                m_noLoadRtaTimer.reset();
                m_presence = RunPresence{QStringLiteral("waiting"), -1.0f, -1.0f};

                return QStringLiteral("Tomb Raider: Anniversary\nnot running");
            }

            const QString gameTimeStatus =
                process->gameTimeAvailable
                    ? formatIgt(process->gameTime)
                    : QStringLiteral("--:--");

            const QString loadedLevelStatus =
                process->loadedLevelAvailable
                    ? process->loadedLevelCode
                    : QStringLiteral("--");

            const QString currentPositionStatus =
                process->currentPositionAvailable
                    ? process->currentPositionCode
                    : QStringLiteral("--");

            const QString selectedRunKey =
                selection.level + QStringLiteral("\n")
                + selection.category + QStringLiteral("\n")
                + selection.subcategory;

            if (selectedRunKey != m_activeSelectionKey) {
                m_activeSelectionKey = selectedRunKey;
                m_levelVerifiedThisRun = false;
                m_finishHandled = false;
                m_finishMessage.clear();
                m_noLoadRtaTimer.reset();
            }

            const QString selectedLevelCode =
                expectedLevelCode(selection.levelIndex);

            // TRA may preload the next level before Lara has left the old level.
            // A run becomes eligible only when its loaded and current-position
            // codes independently identify the selected level. This deliberately
            // treats an unreadable or transitional state as unverified.
            const bool expectedLevelIsActive =
                !selectedLevelCode.isEmpty()
                && process->loadedLevelAvailable
                && process->currentPositionAvailable
                && process->loadedLevelCode.compare(
                       selectedLevelCode,
                       Qt::CaseInsensitive
                       ) == 0
                && process->currentPositionCode.compare(
                       selectedLevelCode,
                       Qt::CaseInsensitive
                       ) == 0;

            // Retain verification through the legitimate end-trigger/loading
            // transition once the selected level has been observed.
            if (expectedLevelIsActive) {
                m_levelVerifiedThisRun = true;
            }

            const QString levelVerificationStatus =
                selectedLevelCode.isEmpty()
                    ? QStringLiteral("Level verification: mapping needed")
                    : m_levelVerifiedThisRun
                          ? QStringLiteral("Level verification: passed")
                          : QStringLiteral("Level verification: failed");

            const bool gameTimeRewound =
                process->gameTimeAvailable
                && m_previousGameTime > 0.0f
                && process->gameTime + 0.001f < m_previousGameTime;

            // A death or checkpoint reload begins a new level attempt. Clear
            // the verified run state before a later result screen can be read.
            if (gameTimeRewound) {
                m_levelVerifiedThisRun = false;
                m_finishHandled = false;
                m_finishMessage.clear();
                // Level runs restart on a death/checkpoint reload. Full-game
                // rules will keep this timer alive across level transitions.
                m_noLoadRtaTimer.reset();
                // Keep the restart state visible long enough for the opponent
                // heartbeat to report it at least once.
                m_restartPresencePolls = 50;
            }

            // The level code pair is the first external frame on which Lara is
            // confirmed inside the selected level. The clock starts at zero on
            // that observation and advances from a monotonic local clock.
            if (!m_finishHandled) {
                m_noLoadRtaTimer.observe(
                    expectedLevelIsActive,
                    process->loadingAvailable,
                    process->loading
                    );
            }
            const qint64 rtaNoLoadMilliseconds = m_noLoadRtaTimer.valid()
                && m_noLoadRtaTimer.started()
                ? m_noLoadRtaTimer.elapsedMilliseconds()
                : -1;
            const float rtaNoLoadSeconds = rtaNoLoadMilliseconds >= 0
                ? static_cast<float>(rtaNoLoadMilliseconds) / 1000.0f
                : -1.0f;
            const QString rtaNoLoadStatus = !m_noLoadRtaTimer.valid()
                ? QStringLiteral("unavailable")
                : rtaNoLoadSeconds >= 0.0f
                ? formatRtaNoLoads(rtaNoLoadMilliseconds)
                : QStringLiteral("--:--");

            QString runStatus;

            if (!process->gameTimeAvailable) {
                runStatus = QStringLiteral("IGT unavailable");
                m_previousGameTime = -1.0f;
                m_unchangedPolls = 0;
            } else if (process->gameTime <= 0.0f) {
                runStatus = QStringLiteral("Waiting for level");
                m_previousGameTime = 0.0f;
                m_unchangedPolls = 0;
            } else if (process->gameTime > m_previousGameTime + 0.001f) {
                runStatus = QStringLiteral("Running");
                m_unchangedPolls = 0;
            } else {
                // Twenty 100 ms polls gives a two-second grace period. This stops
                // a normal IGT update gap from being mistaken for a pause/finish.
                ++m_unchangedPolls;

                runStatus = m_unchangedPolls <= 20
                                ? QStringLiteral("Running")
                                : QStringLiteral("Finish / pause candidate: ")
                                      + formatIgt(process->gameTime);
            }

            m_previousGameTime = process->gameTime;

            // TRAE-Menu-Hook.asi blocks attempt confirmation.
            const bool integrityPassed = process->blockedModule.isEmpty();

            // The verifier recognises TRA's actual result screen, including
            // cutscene and QTE exits. This is deliberately level-agnostic:
            // portals and signals repeat between levels and are diagnostics,
            // never authority to save a completion.
            const bool screenCompletionConfirmed =
                m_screenCompletionReader.takeCompletionFor(selection.level);

            if (!m_finishHandled
                && screenCompletionConfirmed
                && m_levelVerifiedThisRun
                && m_noLoadRtaTimer.started()
                && m_noLoadRtaTimer.valid()
                && process->gameTimeAvailable
                && process->gameTime > 0.0f
                && !gameTimeRewound) {
                if (!integrityPassed) {
                    m_finishMessage = QStringLiteral("Finish rejected: ")
                        + process->blockedModule;
                } else {
                    QString saveError;

                    if (saveCompletedAttempt(
                            selection.level,
                            selection.category,
                            selection.subcategory,
                            process->gameTime,
                            rtaNoLoadSeconds,
                            process->buildHash,
                            &saveError)) {
                        m_savedAttempt = AttemptRecord{
                            selection.level,
                            selection.category,
                            selection.subcategory,
                            process->gameTime,
                            rtaNoLoadSeconds,
                            process->buildHash
                        };
                        m_finishMessage = QStringLiteral("Finish saved: RTA ")
                            + rtaNoLoadStatus;
                    } else {
                        m_finishMessage = QStringLiteral("Finish not saved: ")
                            + saveError;
                    }
                }

                // Consume this completion even when integrity fails: a result
                // screen must never be accepted after a blocked module is removed.
                m_finishHandled = true;
            }
            if (m_finishHandled) {
                m_presence = RunPresence{
                    QStringLiteral("finished"), process->gameTime, rtaNoLoadSeconds};
            } else if (m_restartPresencePolls > 0) {
                --m_restartPresencePolls;
                m_presence = RunPresence{
                    QStringLiteral("restarted"), process->gameTime, rtaNoLoadSeconds};
            } else if (m_levelVerifiedThisRun && m_noLoadRtaTimer.started()) {
                m_presence = RunPresence{
                    QStringLiteral("running"), process->gameTime, rtaNoLoadSeconds};
            } else {
                m_presence = RunPresence{
                    QStringLiteral("waiting"), process->gameTime, rtaNoLoadSeconds};
            }

            const QString integrityStatus =
                integrityPassed
                    ? QStringLiteral("Integrity: passed")
                    : QStringLiteral("Integrity failure: ")
                          + process->blockedModule;

            if (m_presence.state == QStringLiteral("running")
                || m_presence.state == QStringLiteral("restarted")) {
                return QStringLiteral("RTA (no loads)\n%1%2\n\nIGT\n%3\n\n%4")
                    .arg(
                        rtaNoLoadStatus,
                        m_noLoadRtaTimer.pausedForLoad()
                            ? QStringLiteral("  (loading)") : QString(),
                        gameTimeStatus,
                        integrityStatus
                        );
            }
            const bool isInMainMenu =
                loadedLevelStatus.compare(QStringLiteral("Pu104"), Qt::CaseInsensitive) == 0
                || loadedLevelStatus.compare(QStringLiteral("Gr104"), Qt::CaseInsensitive) == 0
                || loadedLevelStatus.compare(QStringLiteral("Eg104"), Qt::CaseInsensitive) == 0
                || loadedLevelStatus.compare(QStringLiteral("Lc104"), Qt::CaseInsensitive) == 0;

            // Keep one fixed layout. The central line changes with the game state,
            // so the window does not resize when leaving the main menu.
            const QString activityStatus = !m_finishMessage.isEmpty()
                ? m_finishMessage
                : isInMainMenu ? QStringLiteral("Main Menu") : runStatus;

            return QStringLiteral(
                    "Selected speedrun:\n%1\n%2\n%3"
                    "\n\n%4\n\nRTA (no loads)\n%5%6\n\nIGT\n%7\n\n%8"
                    )
                    .arg(
                        selection.level,
                        rulesetName(selection.category, selection.subcategory),
                        levelVerificationStatus,
                        activityStatus,
                        rtaNoLoadStatus,
                        m_noLoadRtaTimer.pausedForLoad()
                            ? QStringLiteral("  (loading)") : QString(),
                        gameTimeStatus,
                        integrityStatus
                        );

}
