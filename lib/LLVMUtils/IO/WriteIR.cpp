#include "LLVMUtils/IO/WriteIR.h"

#include <llvm/Bitcode/BitcodeWriter.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/ToolOutputFile.h>

using namespace llvm;

namespace util
{
namespace io
{

// Writes an LLVM module to a text file.
void writeModuleToText(const Module& module, const char* fileName)
{
	std::error_code ec;
	ToolOutputFile out(fileName, ec, sys::fs::OF_None);
	if (ec)
	{
		errs() << ec.message() << "\n";
		std::exit(-3);
	}

	module.print(out.os(), nullptr);

	out.keep();
}

// Writes an LLVM module to a bitcode file.
void writeModuleToBitCode(const Module& module, const char* fileName)
{
	std::error_code ec;
	ToolOutputFile out(fileName, ec, sys::fs::OF_None);
	if (ec)
	{
		errs() << ec.message() << "\n";
		std::exit(-3);
	}

	WriteBitcodeToFile(module, out.os());

	out.keep();
}

// Writes an LLVM module to a file in the specified format.
void writeModuleToFile(const Module& module, const char* fileName, bool isText)
{
	if (isText)
		writeModuleToText(module, fileName);
	else
		writeModuleToBitCode(module, fileName);
}

}
}
