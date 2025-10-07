#include "Annotation/ModRef/ExternalModRefTable.h"
#include "LLVMUtils/IO/ReadFile.h"
#include "Support/pcomb/pcomb.h"

#include <llvm/ADT/StringRef.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;
using namespace pcomb;

namespace annotation
{

/**
 * Finds a mod/ref effect summary for a given function name
 * 
 * @param name The name of the function to look up
 * @return A pointer to the summary, or nullptr if not found
 * 
 * This method is used to retrieve mod/ref effect summaries for external functions
 * that have been specified in a configuration file. Mod/ref analysis tracks which
 * functions modify or reference memory.
 */
const ModRefEffectSummary* ExternalModRefTable::lookup(const StringRef& name) const
{
	auto itr = table.find(name.str());
	if (itr == table.end())
		return nullptr;
	else
		return &itr->second;
}

/**
 * Builds a mod/ref effect table from a configuration file's content
 * 
 * @param fileContent The content of the configuration file as a string
 * @return A fully populated ExternalModRefTable
 * 
 * This method parses a configuration file containing definitions of memory
 * modification and reference behaviors for external functions. It uses a parser
 * combinator approach to interpret the configuration language, which supports:
 * - MOD entries: Indicate memory locations modified by functions
 * - REF entries: Indicate memory locations read by functions
 * - IGNORE entries: Functions to be ignored in mod/ref analysis
 * 
 * The parser creates position specifiers (arguments, return values) and memory
 * access classes (direct or reachable memory) to build a comprehensive model
 * of memory behavior.
 */
ExternalModRefTable ExternalModRefTable::buildTable(const StringRef& fileContent)
{
	ExternalModRefTable table;

	auto idx = rule(
		regex("\\d+"),
		[] (auto const& digits) -> uint8_t
		{
			auto num = std::stoul(digits.str());
			assert(num < 256);
			return num;
		}
	);

	auto id = regex("[\\w\\.]+");

	auto marg = rule(
		seq(str("Arg"), idx),
		[] (auto const& pair)
		{
			return APosition::getArgPosition(std::get<1>(pair));
		}
	);

	auto mafterarg = rule(
		seq(str("AfterArg"), idx),
		[] (auto const& pair)
		{
			return APosition::getAfterArgPosition(std::get<1>(pair));
		}	
	);

	auto mret = rule(
		str("Ret"),
		[] (auto const&)
		{
			return APosition::getReturnPosition();
		}
	);

	auto mpos = alt(mret, marg, mafterarg);

	auto modtype = rule(
		str("MOD"),
		[] (auto const&)
		{
			return ModRefType::Mod;
		}
	);

	auto reftype = rule(
		str("REF"),
		[] (auto const&)
		{
			return ModRefType::Ref;
		}
	);

	auto mtype = alt(modtype, reftype);

	auto dclass = rule(
		ch('D'),
		[] (char)
		{
			return ModRefClass::DirectMemory;
		}
	);
	auto rclass = rule(
		ch('R'),
		[] (char)
		{
			return ModRefClass::ReachableMemory;
		}
	);
	auto mclass = alt(dclass, rclass);

	auto regularEntry = rule(
		seq(
			token(id),
			token(mtype),
			token(mpos),
			token(mclass)
		),
		[&table] (auto const& tuple)
		{
			auto entry = ModRefEffect(std::get<1>(tuple), std::get<3>(tuple), std::get<2>(tuple));
			table.table[std::get<0>(tuple).str()].addEffect(std::move(entry));
			return true;
		}
	);

	auto ignoreEntry = rule(
		seq(
			token(str("IGNORE")),
			token(id)
		),
		[&table] (auto const& pair)
		{
			assert(table.lookup(std::get<1>(pair)) == nullptr && "Ignore entry should not co-exist with other entries");
			table.table.insert(std::make_pair(std::get<1>(pair).str(), ModRefEffectSummary()));
			return false;
		}
	);

	auto commentEntry = rule(
		token(regex("#.*\\n")),
		[] (auto const&)
		{
			return false;
		}
	);

	auto entry = alt(commentEntry, ignoreEntry, regularEntry);
	auto ptable = many(entry);

	auto parseResult = ptable.parse(fileContent);
	if (parseResult.hasError() || !StringRef(parseResult.getInputStream().getRawBuffer()).ltrim().empty())
	{
		auto& stream = parseResult.getInputStream();
		errs() << "Parsing mod/ref config file failed at line " << stream.getLineNumber() << ", column " << stream.getColumnNumber() << "\n";
		std::exit(-1);
	}

	return table;
}

/**
 * Loads an external mod/ref table from a file
 * 
 * @param fileName The path to the configuration file
 * @return A fully populated ExternalModRefTable
 * 
 * This method reads the configuration file and passes its content to the buildTable 
 * method to create the external mod/ref table.
 */
ExternalModRefTable ExternalModRefTable::loadFromFile(const char* fileName)
{
	auto memBuf = util::io::readFileIntoBuffer(fileName);
	return buildTable(memBuf->getBuffer());
}

}
