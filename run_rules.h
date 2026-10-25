#pragma once

#include <QString>
#include <QtGlobal>

QString rulesetName(const QString &category, const QString &subcategory);
QString expectedLevelCode(int levelIndex);
QString formatIgt(float seconds);
QString formatRtaNoLoads(qint64 milliseconds);
QString expectedEndTriggerCode(int levelIndex);
bool isExpectedEndSignal(int levelIndex, int signalId);
