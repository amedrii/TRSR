#pragma once

#include <QElapsedTimer>
#include <QtGlobal>

// A monotonic, client-owned timer which excludes only game-confirmed loading.
// It deliberately does not stop for pause menus, lost focus, or Alt-Tab.
class NoLoadRtaTimer
{
public:
    void reset();
    void observe(bool startFrameObserved, bool loadingAvailable, bool loading);

    bool started() const;
    bool valid() const;
    bool pausedForLoad() const;
    qint64 elapsedMilliseconds() const;

private:
    QElapsedTimer m_clock;
    qint64 m_lastSampleMilliseconds = 0;
    qint64 m_elapsedMilliseconds = 0;
    bool m_started = false;
    bool m_valid = true;
    bool m_pausedForLoad = false;
};
