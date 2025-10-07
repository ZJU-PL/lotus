#ifndef FILEUTILS_H
#define FILEUTILS_H

#include <string>

/// \brief Util class for file operation
class FileUtil {
public:
  /// create a directory
  static bool createDirectory(llvm::StringRef Directory);
  /// write a string to a specific file
  static bool writeToFile(llvm::StringRef, llvm::StringRef ContentToWrite,
                          bool IsCreateDir = true);
  /// get the list of subdirectories of a specific directories
  static bool getSubDirectories(llvm::StringRef Path,
                                std::vector<std::string> &SubDirectories);
  /// read a string from a specific file
  static std::string readStringFromFile(llvm::StringRef Path, bool &IsSuccess);
  /// read a vector of char from a specific file
  static bool readCharVectorFromFile(llvm::StringRef Path,
                                     std::vector<char> &CharVector);
  /// whether file exists
  static bool isFileExist(llvm::StringRef Path);
  /// get the file name from a path
  static std::string getFileNameFromPath(llvm::StringRef path,
                                         llvm::StringRef separator);
};

#endif // FILEUTILS_H