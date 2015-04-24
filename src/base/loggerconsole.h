#ifndef LOGGERCONSOLE_H
#define LOGGERCONSOLE_H

#include "logger.h"
#include <unistd.h>

namespace karere
{
class ConsoleLogger
{
protected:
    Logger& mLogger; //we want reference to the logger because we may want to set up error/warning colors there
    bool mStdoutIsAtty;
    bool mStderrIsAtty;
public:
    ConsoleLogger(Logger& logger)
    : mLogger(logger), mStdoutIsAtty(isatty(1)), mStderrIsAtty(isatty(2))
    {}
    void logString(unsigned level, const char* msg, unsigned flags)
    {
        if (level == krLogLevelError)
        {
            if (mStderrIsAtty)
                fprintf(stderr, "\033[1;31m%s%s", msg, "\033[0m");
            else
                fputs(msg, stderr);

            if ((flags & krLogNoAutoFlush) == 0)
                fflush(stderr);
        }
        else if (level == krLogLevelWarn)
        {
            if (mStderrIsAtty)
                fprintf(stderr, "\033[1;33m%s\033[0m", msg);
            else
                fputs(msg, stderr);

            if ((flags & krLogNoAutoFlush) == 0)
                fflush(stderr);
        }
        else //get color from flags
        {
            if (mStdoutIsAtty)
                printf("%s%s\033[0m", stdoutColorSelect(flags), msg);
            else
                fputs(msg, stdout);
        }
        if ((flags & krLogNoAutoFlush) == 0)
            fflush(stdout);
    }
    const char* stdoutColorSelect(unsigned flags)
    {
        static const char* colorEscapes[krLogColorMask+1] =
        {
            "\033[30m", "\033[31m", "\033[32m", "\033[33m", "\033[34m", "\033[35m", "\033[36m", "\033[37m",
            "\033[1;30m", "\033[1;31m", "\033[1;32m", "\033[1;33m", "\033[1;34m", "\033[1;35m", "\033[1;36m", "\033[1;37m"
        };
        //printf("============== flags: %X, color: %u\n", flags, flags & krLogColorMask);
        return colorEscapes[flags & krLogColorMask];
    }
};
}
#endif // LOGGERCONSOLE_H

