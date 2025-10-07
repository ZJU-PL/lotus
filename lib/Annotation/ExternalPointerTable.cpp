#include "Annotation/Pointer/ExternalPointerTable.h"
#include "LLVMUtils/IO/ReadFile.h"
#include "Support/pcomb/pcomb.h"

#include <llvm/Support/raw_ostream.h>

using namespace llvm;
using namespace pcomb;

namespace annotation
{

/**
 * Finds a pointer effect summary for a given function name
 * 
 * @param name The name of the function to look up
 * @return A pointer to the summary, or nullptr if not found
 * 
 * This method is used to retrieve pointer effect summaries for external functions
 * that have been specified in a configuration file.
 */
const PointerEffectSummary* ExternalPointerTable::lookup(const StringRef& name) const
{
	auto itr = table.find(name.str());
	if (itr == table.end())
		return nullptr;
	else
		return &itr->second;
}

/**
 * Builds a pointer effect table from a configuration file's content
 * 
 * @param fileContent The content of the configuration file as a string
 * @return A fully populated ExternalPointerTable
 * 
 * This method parses a configuration file containing definitions of pointer behaviors
 * for external functions. It uses a parser combinator approach to interpret the
 * configuration language, which supports various pointer-related effects like:
 * - Memory allocation
 * - Pointer copying between parameters/return values
 * - Memory escape tracking
 * - Ignoring specific functions
 * 
 * The parser defines rules for positions (args/return values), copy sources/destinations,
 * allocation effects, and builds a table mapping function names to their effects.
 */
ExternalPointerTable ExternalPointerTable::buildTable(const StringRef& fileContent)
{
	ExternalPointerTable extTable;

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

	auto pret = rule(
		str("Ret"),
		[] (auto const&)
		{
			return APosition::getReturnPosition();
		}
	);

	auto parg = rule(
		seq(str("Arg"), idx),
		[] (auto const& pair)
		{
			return APosition::getArgPosition(std::get<1>(pair));
		}
	);

	auto ppos = alt(parg, pret);

	auto argsrc = rule(
		seq(parg, token(alt(ch('V'), ch('D'), ch('R')))),
		[] (auto const& pair)
		{
			auto type = std::get<1>(pair);
			switch (type)
			{
				case 'V':
					return CopySource::getValue(std::get<0>(pair));
				case 'D':
					return CopySource::getDirectMemory(std::get<0>(pair));
				case 'R':
					return CopySource::getReachableMemory(std::get<0>(pair));
				default:
					llvm_unreachable("Only VDR could possibly be here");
			}
		}
	);

	auto nullsrc = rule(
		str("NULL"),
		[] (auto const&)
		{
			return CopySource::getNullPointer();
		}
	);

	auto unknowsrc = rule(
		str("UNKNOWN"),
		[] (auto const&)
		{
			return CopySource::getUniversalPointer();
		}
	);

	auto staticsrc = rule(
		str("STATIC"),
		[] (auto const&)
		{
			return CopySource::getStaticPointer();
		}
	);

	auto copysrc = alt(nullsrc, unknowsrc, staticsrc, argsrc);

	auto copydest = rule(
		seq(ppos, token(alt(ch('V'), ch('D'), ch('R')))),
		[] (auto const& pair)
		{
			auto type = std::get<1>(pair);
			switch (type)
			{
				case 'V':
					return CopyDest::getValue(std::get<0>(pair));
				case 'D':
					return CopyDest::getDirectMemory(std::get<0>(pair));
				case 'R':
					return CopyDest::getReachableMemory(std::get<0>(pair));
				default:
					llvm_unreachable("Only VDR could possibly be here");
			}
		}
	);

	auto commentEntry = rule(
		token(regex("#.*\\n")),
		[] (auto const&)
		{
			return false;
		}
	);

	auto ignoreEntry = rule(
		seq(
			token(str("IGNORE")),
			token(id)
		),
		[&extTable] (auto const& pair)
		{
			assert(extTable.lookup(std::get<1>(pair)) == nullptr && "Ignore entry should not co-exist with other entries");
			extTable.table.insert(std::make_pair(std::get<1>(pair).str(), PointerEffectSummary()));
			return false;
		}
	);

	auto allocWithSize = rule(
		seq(
			str("ALLOC"),
			token(parg)
		),
		[] (auto const& pair)
		{
			return PointerEffect::getAllocEffect(std::get<1>(pair));
		}
	);

	auto allocWithoutSize = rule(
		str("ALLOC"),
		[] (auto const&)
		{
			return PointerEffect::getAllocEffect();
		}
	);

	auto allocEntry = rule(
		seq(
			token(id),
			token(alt(allocWithSize, allocWithoutSize))
		),
		[&extTable] (auto&& pair)
		{
			extTable.table[std::get<0>(pair).str()].addEffect(std::move(std::get<1>(pair)));
			return true;
		}
	);

	auto copyEntry = rule(
		seq(
			token(id),
			token(str("COPY")),
			token(copydest),
			token(copysrc)
		),
		[&extTable] (auto const& tuple)
		{
			auto entry = PointerEffect::getCopyEffect(std::get<2>(tuple), std::get<3>(tuple));
			extTable.table[std::get<0>(tuple).str()].addEffect(std::move(entry));
			return true;
		}
	);

	auto exitEntry = rule(
		seq(
			token(id),
			token(str("EXIT"))
		),
		[&extTable] (auto const& tuple)
		{
			auto entry = PointerEffect::getExitEffect();
			extTable.table[std::get<0>(tuple).str()].addEffect(std::move(entry));
			return true;
		}
	);

	auto pentry = alt(commentEntry, ignoreEntry, allocEntry, copyEntry, exitEntry);
	auto ptable = many(pentry);

	auto parseResult = ptable.parse(fileContent);
	if (parseResult.hasError() || !StringRef(parseResult.getInputStream().getRawBuffer()).ltrim().empty())
	{
		auto& stream = parseResult.getInputStream();
		errs() << "Parsing pointer config file failed at line " << stream.getLineNumber() << ", column " << stream.getColumnNumber() << "\n";
		std::exit(-1);
	}

	return extTable;
}

/**
 * Loads an external pointer table from a file
 * 
 * @param fileName The path to the configuration file
 * @return A fully populated ExternalPointerTable
 * 
 * This method reads the configuration file and passes its content to the buildTable 
 * method to create the external pointer table.
 */
ExternalPointerTable ExternalPointerTable::loadFromFile(const char* fileName)
{
	auto memBuf = util::io::readFileIntoBuffer(fileName);
	return buildTable(memBuf->getBuffer());
}

/**
 * Adds a pointer effect to the table for a specific function
 * 
 * @param name The name of the function
 * @param e The pointer effect to add
 * 
 * This method is primarily used for testing purposes
 */
void ExternalPointerTable::addEffect(const llvm::StringRef& name, PointerEffect&& e)
{
	table[name.str()].addEffect(std::move(e));
}

}