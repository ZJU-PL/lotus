#include "CFL/Graspan/Library/Logger.h"
#include <iostream>
#include <iomanip>
#include <sstream>

using namespace std;

Logger::Logger() : fileOpen(false) {
    fout.open("../resource/logFile");
    fileOpen = fout.is_open();
    
    if (!fileOpen) {
        severe(FOPEN);
    }
}

Logger::Logger(const std::string& logFile) : fileOpen(false) {
    fout.open(logFile);
    fileOpen = fout.is_open();
    
    if (!fileOpen) {
        severe(FOPEN);
    }
}

Logger::~Logger() {
    if (fout.is_open()) {
        fout.close();
    }
}

std::string Logger::formatTimestamp() const {
    time_t now = time(nullptr);
    struct tm* timeInfo = localtime(&now);
    
    ostringstream oss;
    oss << (timeInfo->tm_year + 1900) << '.'
        << setfill('0') << setw(2) << (timeInfo->tm_mon + 1) << '.'
        << setfill('0') << setw(2) << timeInfo->tm_mday << ' '
        << setfill('0') << setw(2) << timeInfo->tm_hour << ':'
        << setfill('0') << setw(2) << timeInfo->tm_min << ':'
        << setfill('0') << setw(2) << timeInfo->tm_sec << ' ';
    
    return oss.str();
}

void Logger::log(const std::string& level, const std::string& message) {
    string timestamp = formatTimestamp();
    string fullMessage = timestamp + level + ": " + message;
    
    cout << fullMessage << '\n';
    
    if (fileOpen) {
        fout << fullMessage << '\n';
    }
}

void Logger::info(const std::string& message) {
    log("INFO", message);
}

void Logger::warning(const std::string& message) {
    log("WARNING", message);
}

void Logger::severe(Error err) {
    string errorMessage;
    
    switch (err) {
        case FOPEN:
            errorMessage = "Failed to open log file";
            break;
        case FCLOSE:
            errorMessage = "Failed to close log file";
            break;
        default:
            errorMessage = "Unknown error";
            break;
    }
    
    log("SEVERE", errorMessage);
}
