#include "no_load_rta_timer.h"

#include <algorithm>

void NoLoadRtaTimer::reset()
{
    m_clock.invalidate();
    m_lastSampleMilliseconds = 0;
    m_elapsedMilliseconds = 0;
    m_started = false;
    m_valid = true;
    m_pausedForLoad = false;
}

void NoLoadRtaTimer::observe(
    bool startFrameObserved,
    bool loadingAvailable,
    bool loading
    )
{
    if (!m_clock.isValid()) {
        m_clock.start();
    }

    const qint64 nowMilliseconds = m_clock.elapsed();
    const qint64 elapsedSinceLastSample = std::max<qint64>(
        0, nowMilliseconds - m_lastSampleMilliseconds);
    m_lastSampleMilliseconds = nowMilliseconds;

    // A ranked no-load timer must never guess whether a transition is a load.
    // A missing memory read invalidates the attempt; it is never treated as a
    // free pause that could lower a submitted time.
    if (!loadingAvailable) {
        m_valid = false;
        m_pausedForLoad = false;
        return;
    }

    if (!m_valid) {
        return;
    }

    if (!m_started) {
        if (startFrameObserved && !loading) {
            m_started = true;
            m_elapsedMilliseconds = 0;
            m_pausedForLoad = false;
        }
        return;
    }

    if (loading) {
        m_pausedForLoad = true;
        return;
    }

    // Do not charge the polling interval containing the load -> gameplay
    // transition. This keeps the uncertainty bounded to one monitor tick.
    if (m_pausedForLoad) {
        m_pausedForLoad = false;
        return;
    }

    m_elapsedMilliseconds += elapsedSinceLastSample;
}

bool NoLoadRtaTimer::started() const
{
    return m_started;
}

bool NoLoadRtaTimer::valid() const
{
    return m_valid;
}

bool NoLoadRtaTimer::pausedForLoad() const
{
    return m_pausedForLoad;
}

qint64 NoLoadRtaTimer::elapsedMilliseconds() const
{
    return m_elapsedMilliseconds;
}
