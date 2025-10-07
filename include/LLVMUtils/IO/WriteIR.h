#pragma once

namespace llvm
{
	class Module;
}

namespace util
{
namespace io
{

void writeModuleToText(const llvm::Module& module, const char* fileName);
void writeModuleToBitCode(const llvm::Module& module, const char* fileName);
void writeModuleToFile(const llvm::Module& module, const char* fileName, bool isText);

}
}