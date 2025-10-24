#ifndef CHECKER_REPORT_BUGREPORTMGR_H
#define CHECKER_REPORT_BUGREPORTMGR_H

#include "Checker/Report/BugReport.h"
#include "Checker/Report/BugTypes.h"
#include <llvm/ADT/StringMap.h>
#include <llvm/Support/raw_ostream.h>
#include <map>
#include <unordered_map>
#include <vector>
#include <string>

/**
 * BugReportMgr - Centralized manager for all bug reports.
 * Follows Clearblue's architecture pattern.
 * 
 * Responsibilities:
 * - Register bug types with IDs
 * - Store and organize reports by type
 * - Export reports to JSON format
 * - Generate statistics and summaries
 */
class BugReportMgr {
public:
    /**
     * Describes a type of bug (e.g., NPD, Data Race, Taint)
     */
    struct BugType {
        int id;
        llvm::StringRef bug_name;
        BugDescription::BugImportance importance;
        BugDescription::BugClassification classification;
        llvm::StringRef desc;
        
        BugType() 
            : id(-1), bug_name(""), importance(BugDescription::BI_NA),
              classification(BugDescription::BC_NA), desc("") {}
        
        BugType(int id, const llvm::StringRef& name,
                BugDescription::BugImportance imp,
                BugDescription::BugClassification cls,
                const llvm::StringRef& description)
            : id(id), bug_name(name), importance(imp), 
              classification(cls), desc(description) {}
    };
    
public:
    BugReportMgr();
    ~BugReportMgr();
    
    /**
     * Register a new bug type and return its ID.
     * If already registered, returns existing ID.
     */
    int register_bug_type(
        llvm::StringRef ty_name,
        BugDescription::BugImportance importance = BugDescription::BI_NA,
        BugDescription::BugClassification classification = BugDescription::BC_NA,
        llvm::StringRef desc = "");
    
    /**
     * Find bug type ID by name. Returns -1 if not found.
     */
    int find_bug_type(llvm::StringRef ty_name);
    
    /**
     * Get bug type information by ID
     */
    const BugType& get_bug_type_info(int ty_id) const;
    
    /**
     * Insert a bug report
     */
    void insert_report(int ty_id, BugReport* report);
    
    /**
     * Get all reports for a specific bug type
     */
    const std::vector<BugReport*>* get_reports_for_type(int ty_id) const;
    
    /**
     * Generate JSON report file
     */
    void generate_json_report(llvm::raw_ostream& OS, int min_score = 0) const;
    
    /**
     * Print summary statistics to console
     */
    void print_summary(llvm::raw_ostream& OS) const;
    
    /**
     * Get total number of reports across all types
     */
    int get_total_reports() const;
    
    /**
     * Get singleton instance
     */
    static BugReportMgr& get_instance();
    
private:
    // Map bug type names to IDs
    llvm::StringMap<int> bug_type_names;
    
    // Bug type registry
    std::vector<BugType> bug_types;
    
    // Reports organized by bug type ID
    std::unordered_map<int, std::vector<BugReport*>> reports;
    
    // Source file ID management (for compact JSON representation)
    llvm::StringMap<int> src_file_ids;
    std::vector<std::string> src_files;
    
    int get_src_file_id(llvm::StringRef src_file);
};

#endif // CHECKER_REPORT_BUGREPORTMGR_H

