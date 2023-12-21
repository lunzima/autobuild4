#include "logger.hpp"
#include "abconfig.h"

#include <iostream>

inline const char *level_to_string(const LogLevel level) {
  switch (level) {
  case LogLevel::Debug:
    return "DEBUG";
  case LogLevel::Info:
    return "INFO";
  case LogLevel::Warning:
    return "WARN";
  case LogLevel::Error:
    return "ERROR";
  case LogLevel::Critical:
    return "CRIT";
  }
  return "UNK";
}

void PlainLogger::log(const LogLevel lvl, const std::string message) {
  switch (lvl) {
  case LogLevel::Info:
    std::cout << "[INFO]:  ";
    break;
  case LogLevel::Warning:
    std::cout << "[WARN]:  ";
    break;
  case LogLevel::Error:
    std::cout << "[ERROR]: ";
    break;
  case LogLevel::Critical:
    std::cout << "[CRIT]:  ";
    break;
  case LogLevel::Debug:
    std::cout << "[DEBUG]: ";
    break;
  }

  std::cout << message << std::endl;
  std::cout.flush();
}

void PlainLogger::logDiagnostic(Diagnostic diagnostic) {
  this->error("Build error detected ^o^");
  for (const auto &diag : diagnostic.frames) {
    const auto filename = diag.file.empty() ? "<unknown>" : diag.file;
    const auto function = (diag.function.empty() || diag.function == "source")
                              ? "<unknown>"
                              : diag.function;
    printf("%s(%zu): In function `%s':\n", filename.c_str(), diag.line,
           function.c_str());
  }
  std::cout << diagnostic.message << std::endl;
}

void PlainLogger::logException(std::string message) {
  std::cout << "autobuild encountered an error and couldn't continue."
            << std::endl;
  if (!message.empty()) {
    std::cout << message << std::endl;
  } else {
    std::cout << "Look at the stacktrace to see what happened." << std::endl;
  }
  printf("------------------------------autobuild "
         "%s------------------------------\n",
         ab_version);
  printf("Go to %s for more information on this error.\n", ab_url);
}

using json = nlohmann::json;
using namespace nlohmann::literals;

void JsonLogger::log(LogLevel lvl, std::string message) {
  const json line = {
      {"event", "log"}, {"level", level_to_string(lvl)}, {"message", message}};
  std::cout << line.dump() << std::endl;
}

void JsonLogger::logDiagnostic(Diagnostic diagnostic) {
  json line = {{"event", "diagnostic"},
               {"level", level_to_string(diagnostic.level)},
               {"message", diagnostic.message}};
  line["frames"] = json::array();
  for (const auto &frame : diagnostic.frames) {
    line["frames"].push_back({{"file", frame.file},
                              {"line", frame.line},
                              {"function", frame.function}});
  }
  std::cout << line.dump() << std::endl;
}

void JsonLogger::logException(std::string message) {
  json line = {{"event", "exception"}, {"level", "CRIT"}, {"message", message}};
  std::cout << line.dump() << std::endl;
}

void ColorfulLogger::log(LogLevel lvl, std::string message) {
  switch (lvl) {
  case LogLevel::Info:
    std::cout << "[\x1b[96mINFO\x1b[0m]:  ";
    break;
  case LogLevel::Warning:
    std::cout << "[\x1b[33mWARN\x1b[0m]:  ";
    break;
  case LogLevel::Error:
    std::cout << "[\x1b[31mERROR\x1b[0m]: ";
    break;
  case LogLevel::Critical:
    std::cout << "[\x1b[93mCRIT\x1b[0m]:  ";
    break;
  case LogLevel::Debug:
    std::cout << "[\x1b[32mDEBUG\x1b[0m]: ";
    break;
  }

  std::cout << "\x1b[1m" << message << "\x1b[0m" << std::endl;
  std::cout.flush();
}

void ColorfulLogger::logDiagnostic(Diagnostic diagnostic) {
  // TODO
}

void ColorfulLogger::logException(std::string message) {
  std::cout << "\x1b[1;31m"
            << "autobuild encountered an error and couldn't continue."
            << "\x1b[0m" << std::endl;
  if (!message.empty()) {
    std::cout << message << std::endl;
  } else {
    std::cout << "Look at the stacktrace to see what happened." << std::endl;
  }
  printf("------------------------------autobuild "
         "%s------------------------------\n",
         ab_version);
  std::string colored_url{"‘\e[1m"};
  colored_url += ab_url;
  colored_url += "\x1b[0m’";
  printf("Go to %s for more information on this error.\n", colored_url.c_str());
}
