#ifndef LOGGER_H
#define LOGGER_H

#include "CFL/Graspan/utilities/globalDefinitions.hpp"
#include <fstream>
#include <string>

class Logger {
private:
    std::ofstream fout;
    bool fileOpen;
    
    std::string formatTimestamp() const;
    void log(const std::string& level, const std::string& message);

public:
    Logger();
    explicit Logger(const std::string& logFile);
    ~Logger();

    void info(const std::string& message);
    void warning(const std::string& message);
    void severe(Error err);
};

#endif

