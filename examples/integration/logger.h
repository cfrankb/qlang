/*
    cs3-runtime-sdl
    Copyright (C) 2025 Francois Blanchette

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/
// logger.h
/// Logging macros for info, warning, error, and fatal messages.
/// Usage: LOGI("Message %s", str); LOGF("Fatal error %d", code);

#pragma once
#ifndef __LOGGER_H__
#define __LOGGER_H__

#include <cstdio>
#include <mutex>
#include <string>

class Logger
{
public:
    enum Level
    {
        L_INFO,
        L_WARN,
        L_ERROR,
        L_FATAL
    };
    static void setLevel(Level minLevel) { m_minLevel = minLevel; }
    static void setOutputFile(const std::string &filename);
    static void log(Level level, const char *tag, const char *format, ...);
    static Level level() { return m_minLevel; };

private:
    static Level m_minLevel;
    static FILE *m_output;
    static std::mutex m_mutex;
};

#if defined(USE_QFILE)
#include <QtLogging>
#include <QDebug>
#define printf qDebug
#define LOGI(...) qInfo(__VA_ARGS__)
#define LOGW(...) qWarning(__VA_ARGS__)
#define LOGE(...) qCritical(__VA_ARGS__)
#define LOGF(...) qFatal(__VA_ARGS__)
#else
// __FILE_NAME__ is a GCC/Clang extension (MSVC has only __FILE__); the logger
// trims whatever it is given down to a basename.
#if defined(__FILE_NAME__)
#define LOG_FILE_NAME __FILE_NAME__
#else
#define LOG_FILE_NAME __FILE__
#endif
#define LOGI(...)                          \
    if (Logger::level() <= Logger::L_INFO) \
    Logger::log(Logger::L_INFO, LOG_FILE_NAME, __VA_ARGS__)
#define LOGW(...)                          \
    if (Logger::level() <= Logger::L_WARN) \
    Logger::log(Logger::L_WARN, LOG_FILE_NAME, __VA_ARGS__)
#define LOGE(...)                           \
    if (Logger::level() <= Logger::L_ERROR) \
    Logger::log(Logger::L_ERROR, LOG_FILE_NAME, __VA_ARGS__)
#define LOGF(...)                                             \
    Logger::log(Logger::L_FATAL, LOG_FILE_NAME, __VA_ARGS__); \
    exit(1)
#endif

#endif