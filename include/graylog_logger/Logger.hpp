//
//  Logger.hpp
//  dm-graylog-logger
//
//  Created by Jonas Nilsson on 2016-12-19.
//  Copyright © 2016 European Spallation Source. All rights reserved.
//

#pragma once

#include "graylog_logger/LoggingBase.hpp"

class Logger : private LoggingBase {
public:
    static Logger& Inst();
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;
    virtual void AddLogHandler(const LogHandler_P handler);
    using LoggingBase::Log;
    using LoggingBase::RemoveAllHandlers;
    using LoggingBase::GetHandlers;
    using LoggingBase::SetMinSeverity;
private:
    Logger();
    ~Logger();
};