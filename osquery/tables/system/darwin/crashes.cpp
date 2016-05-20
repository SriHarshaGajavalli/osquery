/*
 *  Copyright (c) 2014-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */

#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/replace.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/algorithm/string/trim.hpp>
#include <boost/format.hpp>
#include <boost/regex.hpp>

#include <osquery/filesystem.h>
#include <osquery/logger.h>
#include <osquery/tables.h>

#include "osquery/core/conversions.h"
#include "osquery/tables/system/system_utils.h"

namespace fs = boost::filesystem;
namespace alg = boost::algorithm;

namespace osquery {
namespace tables {

/// Location of the system application crash logs in OS X
const std::string kDiagnosticReportsPath = "/Library/Logs/DiagnosticReports";
/// Location of the user mobile devices crash logs in OS X
const std::string kMobileDiagnosticReportsPath =
    "/Library/Logs/CrashReporter/MobileDevice";
/// Map of the values we currently parse out of the log file
const std::map<std::string, std::string> kCrashDumpKeys = {
    {"Process", "pid"},
    {"Path", "path"},
    {"Log Location", "crash_path"},
    {"Identifier", "identifier"},
    {"Version", "version"},
    {"Parent Process", "parent"},
    {"Responsible", "responsible"},
    {"User ID", "uid"},
    {"Date/Time", "datetime"},
    {"Crashed Thread", "crashed_thread"},
    {"Exception Type", "exception_type"},
    {"Exception Codes", "exception_codes"},
    {"Exception Note", "exception_notes"},
    // Note: We leave these two in, as they ensure we don't skip over the
    // register values in our check to ensure the token is a value we care
    // about.
    {"rax", "rax"},
    {"rdi", "rdi"},
    // Registers for mobile crashes
    {"Triggered by Thread", "crashed_thread"},
    {"x0", "x0"},
    {"x4", "x4"},
};

void readCrashDump(const std::string& app_log, Row& r) {
  r["crash_path"] = app_log;
  std::string content;

  if (!readFile(app_log, content).ok()) {
    return;
  }

  // Variables for capturing the stack trace
  boost::format crashed_thread_format("Thread %1% Crashed");
  auto crashed_thread_seen = false;

  auto lines = split(content, "\n");
  for (auto it = lines.begin(); it != lines.end(); it++) {
    auto line = *it;
    // Tokenize first by colons
    auto toks = split(line, ":");

    if (toks.size() == 0) {
      continue;
    }

    // Grab the most recent stack trace line of the crashed thread.
    if (crashed_thread_seen && toks[0] == crashed_thread_format.str()) {
      r["stack_trace"] = *(++it);
      crashed_thread_seen = false;
      continue;
    }

    if (kCrashDumpKeys.count(toks[0]) == 0) {
      continue;
    }

    // Process and grab all register values
    if (toks[0] == "rax" || toks[0] == "x0") {
      std::string reg_str = *it + " " + *(++it);

      alg::replace_all(reg_str, ": ", ":");
      alg::replace_all(reg_str, "   ", " ");

      r["registers"] = std::move(reg_str);
    } else if (toks[0] == "Date/Time" && toks.size() >= 3) {
      // Reconstruct split date/time
      r[kCrashDumpKeys.at(toks[0])] = toks[1] + ":" + toks[2] + ":" + toks[3];
    } else if (toks[0] == "Crashed Thread" ||
               toks[0] == "Triggered by Thread") {
      // If the token is the Crashed thread, update the format string so
      // we can grab the stack trace later.
      auto t = split(toks[1], " ");
      if (t.size() == 0) {
        continue;
      }
      r[kCrashDumpKeys.at(toks[0])] = t[0];
      crashed_thread_format % r[kCrashDumpKeys.at(toks[0])];
      crashed_thread_seen = true;
    } else if (toks[0] == "Process" || toks[0] == "Parent Process") {
      // Use a regex to extract out the PID value
      const boost::regex e{"\\[\\d+\\]"};
      boost::smatch results;
      if (boost::regex_search(line, results, e)) {
        auto pid_str = std::string(results[0].first, results[0].second);
        auto pid = pid_str.substr(1, pid_str.size() - 2);
        r[kCrashDumpKeys.at(toks[0])] = pid;
      }
    } else if (toks[0] == "User ID") {
      r[kCrashDumpKeys.at(toks[0])] = toks[1];
    } else {
      // otherwise, process the line normally.
      r[kCrashDumpKeys.at(toks[0])] = toks[1];
    }
  }
}

QueryData genCrashLogs(QueryContext& context) {
  QueryData results;

  auto process_crash_logs = [&results](const fs::path& path,
                                       const std::string type) {
    std::vector<std::string> files;
    if (listFilesInDirectory(path, files)) {
      for (const auto& lf : files) {
        if (alg::ends_with(lf, ".crash") &&
            lf.find("LowBattery") == std::string::npos) {
          Row r;
          r["type"] = type;
          readCrashDump(lf, r);
          results.push_back(r);
        }
      }
    }
  };

  // Process system logs
  if (context.constraints["uid"].notExistsOrMatches("0")) {
    process_crash_logs(kDiagnosticReportsPath, "application");
  }

  // Process user logs
  auto users = usersFromContext(context);
  for (const auto& user : users) {
    auto user_home = fs::path(user.at("directory")) / kDiagnosticReportsPath;
    process_crash_logs(user_home, "application");

    // Process mobile crash logs
    auto user_mobile_root =
        fs::path(user.at("directory")) / kMobileDiagnosticReportsPath;
    std::vector<std::string> mobile_paths;
    if (listDirectoriesInDirectory(user_mobile_root, mobile_paths)) {
      for (const auto& mobile_device : mobile_paths) {
        process_crash_logs(mobile_device, "mobile");
      }
    }
  }

  return results;
}
}
}