#ifndef STRUCT_ANALYZER_H
#define STRUCT_ANALYZER_H

#include <cassert>
#include <llvm/IR/Module.h>
#include <llvm/IR/Type.h>
#include <llvm/ADT/iterator_range.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/DebugLoc.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/JSON.h>
#include <llvm/Support/raw_ostream.h>

#include <vector>
#include <map>
#include <set>
#include <unordered_map>

#include "Common.h"
#include "Annotation.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Value.h"

using namespace llvm;
using namespace std;

extern cl::opt<std::string> OutputDir;

// Every struct type T is mapped to the vectors fieldSize and offsetMap.
// If field [i] in the expanded struct T begins an embedded struct, fieldSize[i] is the # of fields in the largest such struct, else S[i] = 1.
// Also, if a field has index (j) in the original struct, it has index offsetMap[j] in the expanded struct.
class StructInfo
{
private:
	// FIXME: vector<bool> is considered to be BAD C++ practice. We have to switch to something else like deque<bool> some time in the future
	std::vector<bool> arrayFlags;
	std::vector<bool> pointerFlags;
	std::vector<bool> unionFlags;
	std::vector<unsigned> fieldSize;
	std::vector<unsigned> offsetMap;
	std::vector<unsigned> fieldOffset;
	std::vector<unsigned> fieldRealSize;


	// field => type(s) map
	std::map<unsigned, std::set<const llvm::Type*> > elementType;
	
	// the corresponding data layout for this struct
	const llvm::DataLayout* dataLayout;
	void setDataLayout(const llvm::DataLayout* layout) { dataLayout = layout; }

	// real type
	const llvm::StructType* stType;
	void setRealType(const llvm::StructType* st) { stType = st; }

	// defining module
	const llvm::Module* module;
	void setModule(const llvm::Module* M) { module = M; }

	// container type(s)
	std::set<std::pair<const llvm::StructType*, unsigned> > containers;
	void addContainer(const llvm::StructType* st, unsigned offset)
	{
		containers.insert(std::make_pair(st, offset));
	}

	static const llvm::StructType* maxStruct;
	static unsigned maxStructSize;
	uint64_t allocSize;

	bool finalized;

	void addOffsetMap(unsigned newOffsetMap) { offsetMap.push_back(newOffsetMap); }
	void addField(unsigned newFieldSize, bool isArray, bool isPointer, bool isUnion)
	{
		fieldSize.push_back(newFieldSize);
		arrayFlags.push_back(isArray);
		pointerFlags.push_back(isPointer);
		unionFlags.push_back(isUnion);
	}
	void addFieldOffset(unsigned newOffset) { fieldOffset.push_back(newOffset); }
	void addRealSize(unsigned size) { fieldRealSize.push_back(size); }
	void appendFields(const StructInfo& other)
	{
		if (!other.isEmpty()) {
			fieldSize.insert(fieldSize.end(), (other.fieldSize).begin(), (other.fieldSize).end());
		}
		arrayFlags.insert(arrayFlags.end(), (other.arrayFlags).begin(), (other.arrayFlags).end());
		pointerFlags.insert(pointerFlags.end(), (other.pointerFlags).begin(), (other.pointerFlags).end());
		unionFlags.insert(unionFlags.end(), (other.unionFlags).begin(), (other.unionFlags).end());
		fieldRealSize.insert(fieldRealSize.end(), (other.fieldRealSize).begin(), (other.fieldRealSize).end());
	}
	void appendFieldOffset(const StructInfo& other)
	{
		unsigned base = fieldOffset.back();
		for (auto i : other.fieldOffset) {
			if (i == 0) continue;
			fieldOffset.push_back(i + base);
		}
	}
	void addElementType(unsigned field, const llvm::Type* type) { elementType[field].insert(type); }
	void appendElementType(const StructInfo& other)
	{
		unsigned base = fieldSize.size();
		for (auto item : other.elementType)
			elementType[item.first + base].insert(item.second.begin(), item.second.end());
	}

	// Must be called after all fields have been analyzed
	void finalize()
	{
		finalized = true;
	}

	static void updateMaxStruct(const llvm::StructType* st, unsigned structSize)
	{
		if (structSize > maxStructSize) {
			maxStruct = st;
			maxStructSize = structSize;
		}
	}
public:
	bool isFinalized() {
		return finalized;
	}

    /****************** Flexible Structural Object Identification **************/
    bool flexibleStructFlag;
    /**************** End Flexible Structural Object Identification ************/

	// external information
	std::string name;
	llvm::SmallPtrSet<llvm::Instruction*, 32>   allocaInst;
	llvm::SmallPtrSet<llvm::Instruction*, 32>   copyInst;
	std::set<unsigned>   fieldAllocGEP;	// GEP of fields

    typedef std::vector<Value*> CmpSrc;
    struct CheckSrc{
        CmpSrc src1;
        CmpSrc src2;
        unsigned branchTaken;
    };

    typedef std::unordered_map<llvm::Instruction*, CheckSrc> CheckInfo;
    typedef std::unordered_map<string, CheckInfo> CheckMap; 
    CheckMap allocCheck, otherCheck;

	struct SiteInfo {
		unsigned TYPE;
		unsigned KEY_OFFSET;
		// location info can be stored in Value
		// see the location by calling DEBUG_Inst()
		// value stores Load or GEP, which indicates load-GEP pair
		llvm::StructType *toSt=nullptr;
		llvm::Value *toValue=nullptr;
		llvm::StructType *fromSt=nullptr;
		llvm::Value *fromValue=nullptr;
		llvm::StructType *lenSt=nullptr;
		llvm::Value *lenValue=nullptr;
        CheckMap copyCheckMap; // usage of all fields before copying

		void dumpLen() {
			if(this->lenValue && this->lenSt){
				KA_LOGS(0, "[#] len Value ");
				DEBUG_Inst(0, dyn_cast<Instruction>(this->lenValue));
				KA_LOGS(0, "  StructType : "<< *this->lenSt << "\n");
			}
		}
		void dumpFrom() {
			if(this->fromValue){
				KA_LOGS(0, "[#] src Value ");
				if(dyn_cast<Instruction>(this->fromValue)){
					DEBUG_Inst(0, dyn_cast<Instruction>(this->fromValue));
				}else{
					KA_LOGS(0, *this->fromValue);
					for(auto *user : this->fromValue->users()){
						if(auto *I = dyn_cast<Instruction>(user)){
							KA_LOGS(0, " in " << I->getModule()->getName());
							break;
						}
					}
				}

			}
			if(this->fromSt){
				KA_LOGS(0, "StructType : "<< this->fromSt->getName() << "\n");
			}
			KA_LOGS(0,"\n");
		}
		void dumpTo() {
			if(this->toValue){
				KA_LOGS(0, "[#] dst Value ");
				if(dyn_cast<Instruction>(this->toValue)){
					DEBUG_Inst(0, dyn_cast<Instruction>(this->toValue));
				}else{
					KA_LOGS(0, *this->toValue);
					for(auto *user : this->toValue->users()){
						if(auto *I = dyn_cast<Instruction>(user)){
							KA_LOGS(0, " in " << I->getModule()->getName());
							break;
						}
					}
				}

			}
			if(this->toSt){
				KA_LOGS(0, "StructType : "<< this->toSt->getName() << "\n");
			}
			KA_LOGS(0,"\n");
		}

		llvm::Value *getSiteInfoValue(unsigned argOffset) {
			switch (argOffset) {
			case 0:	// to
				return this->toValue;
			case 1: // from
				return this->fromValue;
			case 2: // len
				return this->lenValue;
			default:
				assert(argOffset <= 2 && "Wrong argOffset");
				return nullptr;
			}
		}
		llvm::StructType *getSiteInfoSt(unsigned argOffset) {
			switch (argOffset) {
			case 0:	// to
				return this->toSt;
			case 1: // from
				return this->fromSt;
			case 2: // len
				return this->lenSt;
			default:
				assert(argOffset <= 2 && "Wrong argOffset");
				return nullptr;
			}
		}

		void setSiteInfoValue(unsigned argOffset, llvm::Value *V) {
			switch (argOffset) {
			case 0:	// to
				this->toValue = V;
				break;
			case 1: // from
				this->fromValue = V;
				break;
			case 2: // len
				this->lenValue = V;
				break;
			default:
				assert(argOffset <= 2 && "Wrong argOffset");
			}
		}
		void setSiteInfoSt(unsigned argOffset, llvm::StructType *stType) {
			switch (argOffset) {
			case 0:	// to
				this->toSt = stType;
				break;
			case 1: // from
				this->fromSt = stType;
				break;
			case 2: // len
				this->lenSt = stType;
				break;
			default:
				assert(argOffset <= 2 && "Wrong argOffset");
			}
		}
	};
	// differents values represent different copying sites
	// value here equals call copyout(to, from, len);
	typedef std::unordered_map<llvm::Value *, SiteInfo> CopySourceInfo;
	// len offset and copyInfo
	typedef std::unordered_map<unsigned, CopySourceInfo> CopyInfo;

	CopyInfo copyInfo;

	void addCopySourceInfo(unsigned offset, llvm::Value *V, SiteInfo siteInfo){

		if(copyInfo.find(offset) == copyInfo.end()){
			CopySourceInfo LSI;
			LSI.insert(std::make_pair(V, siteInfo));
			copyInfo.insert(std::make_pair(offset, LSI));
			return;
		}

		CopyInfo::iterator it = copyInfo.find(offset);

		if(it->second.find(V) == it->second.end()){
			it->second.insert(std::make_pair(V, siteInfo));
			return;
		}

		WARNING("Are we trying to update siteInfo?");
		assert(false && "BUG?");

		CopySourceInfo::iterator sit = it->second.find(V);
		sit->second = siteInfo;
	}

	SiteInfo *getSiteInfo(unsigned offset, llvm::Value *V){

		if(copyInfo.find(offset) == copyInfo.end()){
			return nullptr;
		}

		CopyInfo::iterator it = copyInfo.find(offset);

		if(it->second.find(V) == it->second.end()){
			return nullptr;
		}

		CopySourceInfo::iterator sit = it->second.find(V);
		return &sit->second;
	}
	// # fields == # arrayFlags == # pointer flags
	// size => # of fields????
	// getExpandedSize => # of unrolled fields???

	typedef std::vector<unsigned>::const_iterator const_iterator;
	unsigned getSize() const { return offsetMap.size(); }
	unsigned getExpandedSize() const { return arrayFlags.size(); }

	bool isEmpty() const { return (fieldSize.empty() || fieldSize[0] == 0);}
	bool isFieldArray(unsigned field) const { return arrayFlags.at(field); }
	bool isFieldPointer(unsigned field) const { return pointerFlags.at(field); }
	bool isFieldUnion(unsigned field) const { return unionFlags.at(field); }
	unsigned getOffset(unsigned off) const { return offsetMap.at(off); }
	const llvm::Module* getModule() const { return module; }
	const llvm::DataLayout* getDataLayout() const { return dataLayout; }
	const llvm::StructType* getRealType() const { return stType; }
	const uint64_t getAllocSize() const { return allocSize; }
	unsigned getFieldRealSize(unsigned field) const { return fieldRealSize.at(field); }
	unsigned getFieldOffset(unsigned field) const { return fieldOffset.at(field); }
	std::set<const llvm::Type*> getPointerElementType(unsigned field) const
	{
		auto itr = elementType.find(field);
		if (itr != elementType.end())
			return itr->second;
		else
			return std::set<const llvm::Type*>();
	}
	const llvm::StructType* getContainer(const llvm::StructType* st, unsigned offset) const
	{
		assert(!st->isOpaque());
		if (containers.count(std::make_pair(st, offset)) == 1)
			return st;
		else
			return nullptr;
	}

	static unsigned getMaxStructSize() { return maxStructSize; }

	friend class StructAnalyzer;
};

// Construct the necessary StructInfo from LLVM IR
// This pass will make GEP instruction handling easier
class StructAnalyzer
{
private:
	// Map llvm type to corresponding StructInfo
	typedef std::map<const llvm::StructType*, StructInfo> StructInfoMap;
	StructInfoMap structInfoMap;

	// Map struct name to llvm type
	typedef std::map<const std::string, const llvm::StructType*> StructMap;
	StructMap structMap;

	// Expand (or flatten) the specified StructType and produce StructInfo
	StructInfo& addStructInfo(const llvm::StructType* st, const llvm::Module* M, const llvm::DataLayout* layout);
	// If st has been calculated before, return its StructInfo; otherwise, calculate StructInfo for st
	StructInfo& computeStructInfo(const llvm::StructType* st, const llvm::Module *M, const llvm::DataLayout* layout);
	// update container information
	void addContainer(const llvm::StructType* container, StructInfo& containee, unsigned offset, const llvm::Module* M);
public:
	StructAnalyzer() {}

	// Return NULL if info not found
	// const StructInfo* getStructInfo(const llvm::StructType* st, llvm::Module* M) const;
	StructInfo* getStructInfo(const llvm::StructType* st, llvm::Module* M);
	size_t getSize() const { return structMap.size(); }

	// Iterate over every analyzed struct (the FULL set of struct types seen in
	// the modules, not just the alloc/copy "key objects"). Used to dump the
	// complete struct relation graph.
	const std::map<const llvm::StructType*, StructInfo>& getAllStructInfo() const
	{ return structInfoMap; }
	bool getContainer(std::string stid, const llvm::Module* M, std::set<std::string> &out) const;
	//bool getContainer(const llvm::StructType* st, std::set<std::string> &out) const;

	void run(llvm::Module* M, const llvm::DataLayout* layout);

	void printStructInfo() const;
};

#endif
