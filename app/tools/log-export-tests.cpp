// Executable spec for Export logs (App/LogExport.h): the app's log copied to where the person
// asked, with every urnetwork: link in it reduced, and never the log itself emptied, changed or
// made unreadable by the copy. Run against the same header the app compiles, on any host with a
// C++20 compiler. It writes files, so it is given a scratch directory (and removes what it made):
//
//   c++ -std=c++20 -I ../src/App log-export-tests.cpp -o /tmp/log-export-tests && /tmp/log-export-tests /tmp/scratch
//
// Every value is made up and obviously so (SECRETSTATE, SECRETKEY); none is a real state, key or code.
//
// SPDX-License-Identifier: MPL-2.0

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <system_error>

#include "LogExport.h"

namespace fs = std::filesystem;
using urnw::logexport::CopyWithoutLinks;

namespace {

int gFailures = 0;
int gCases = 0;

void Check(bool condition, const std::string& what) {
  ++gCases;
  if (!condition) {
    ++gFailures;
    std::cout << "  FAIL " << what << std::endl;  // flushed: a spec that dies a moment later still says what failed
  }
}

fs::path gDir;

void Write(const fs::path& path, const std::string& bytes) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

std::string Read(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

const std::string kLink = "urnetwork://auth/?state=SECRETSTATE&epk=SECRETKEY&n=SECRETNONCE&d=SECRETBOX";
const std::string kTag = "urnetwork://auth [redacted " + std::to_string(kLink.size()) + " chars]";
const std::string kExe = R"("C:\Program Files\URnetwork\URnetwork.exe")";

// The copy of `log` made through the function, or nothing at all when it said no.
bool Copy(const std::string& log, std::string& copied) {
  const fs::path source = gDir / "urnetwork-app.log";
  const fs::path destination = gDir / "exported.log";
  Write(source, log);
  fs::remove(destination);
  const bool ok = CopyWithoutLinks(source, destination);
  copied = ok ? Read(destination) : std::string();
  return ok;
}

void TestTheCopyIsTheLogWithoutItsLinks() {
  std::string copied;
  const std::string stamp = "2026-10-07 23:36:01 INFO  startup:   command line     : ";
  const std::string log = stamp + kExe + " " + kLink + "\r\nnext line\r\n" + stamp + kExe + " \"" + kLink + "\"\r\nlast\r\n";
  const std::string want = stamp + kExe + " " + kTag + "\r\nnext line\r\n" + stamp + kExe + " \"" + kTag + "\"\r\nlast\r\n";
  Check(Copy(log, copied) && copied == want, "copy: every link of every line is its host and a length, and every other byte is kept");
  Check(copied.find("SECRET") == std::string::npos, "copy: no value survives");
  Check(Copy(log + "no final line break", copied) && copied == want + "no final line break",
        "copy: a last line that ends without a line break is not given one");
  Check(Copy(log, copied) && copied.size() > 2 && copied.compare(copied.size() - 2, 2, "\r\n") == 0,
        "copy: a last line that ends with a line break keeps it, carriage return included");
  Check(Copy("", copied) && copied.empty(), "copy: an empty log is an empty file");
  Check(Copy("\n", copied) && copied == "\n" && Copy("\r\n\r\n", copied) && copied == "\r\n\r\n", "copy: blank lines are kept");
  Check(Copy("one\rtwo\rthree", copied) && copied == "one\rtwo\rthree", "copy: a lone carriage return is not a line break, and is kept");
  const std::string withNul = std::string("a\0b ", 4) + kLink + std::string(" \0c\n", 4);
  Check(Copy(withNul, copied) && copied == std::string("a\0b ", 4) + kTag + std::string(" \0c\n", 4),
        "copy: a NUL in a line is kept, before a link and after it");
  Check(Copy("caf\xC3\xA9 \xE2\x82\xAC\r\n", copied) && copied == "caf\xC3\xA9 \xE2\x82\xAC\r\n", "copy: UTF-8 outside a link is kept");
  // what the app redacted at the source is not redacted a second time
  const std::string already = stamp + kExe + " " + kTag + "\r\n";
  Check(Copy(already, copied) && copied == already, "copy: a line redacted when it was written is copied as it is");
}

void TestALongLogIsCopiedInOnePass() {
  std::string log, want;
  for (int i = 0; i < 4000; ++i) {
    const std::string number = "line " + std::to_string(i);
    if (i % 5 == 0) {
      log += number + " " + kLink + "\r\n";
      want += number + " " + kTag + "\r\n";
    } else {
      log += number + " nothing to see here\r\n";
      want += number + " nothing to see here\r\n";
    }
  }
  std::string copied;
  Check(Copy(log, copied) && copied == want, "long: a log of 4000 lines, a link on one in five, is copied exactly");
  Check(copied == urnw::authlink::RedactLogText(log), "long: and it is exactly what redacting the whole text gives");
}

void TestTheLogItselfIsNeverEmptied() {
  const fs::path source = gDir / "urnetwork-app.log";
  const std::string log = "header\r\n" + kLink + "\r\ntrailer\r\n";
  Write(source, log);
  // saving over the log itself, spelled four ways: the picker suggests the log's own name
  Check(!CopyWithoutLinks(source, source), "same file: saving over the log is refused");
  Check(Read(source) == log, "same file: and the log is as it was, not emptied");
  Check(!CopyWithoutLinks(source, gDir / "." / "urnetwork-app.log"), "same file: the same file by a path with a dot in it is refused");
  Check(Read(source) == log, "same file: and untouched");
  fs::create_directory(gDir / "sub");
  Check(!CopyWithoutLinks(source, gDir / "sub" / ".." / "urnetwork-app.log"), "same file: by a path that goes down and up is refused");
  Check(Read(source) == log, "same file: and untouched again");
  std::error_code linked;
  fs::create_hard_link(source, gDir / "hardlink.log", linked);
  if (!linked) {
    Check(!CopyWithoutLinks(source, gDir / "hardlink.log"), "same file: a hard link to the log is the log, and is refused");
    Check(Read(source) == log, "same file: and untouched once more");
    fs::remove(gDir / "hardlink.log");
  }
  // and in the other direction: the log is the destination of nothing it was read from
  const fs::path elsewhere = gDir / "elsewhere.log";
  Write(elsewhere, "an earlier export that is longer than what will replace it, and is replaced whole");
  Check(CopyWithoutLinks(source, elsewhere) && Read(elsewhere) == "header\r\n" + kTag + "\r\ntrailer\r\n",
        "overwrite: an earlier export is replaced whole, not mixed with what it held");
  Check(Read(source) == log, "overwrite: and the log is still as it was");
}

void TestAnExistingExportSurvivesWhatCannotBeCopied() {
  const fs::path destination = gDir / "earlier.log";
  Write(destination, "the earlier export");
  Check(!CopyWithoutLinks(gDir / "no-such-log.log", destination), "missing: a log that is not there cannot be copied");
  Check(Read(destination) == "the earlier export", "missing: and an export that was already there is as it was, not emptied");
  Check(!CopyWithoutLinks(gDir / "urnetwork-app.log", gDir / "no-such-directory" / "out.log"),
        "missing: a destination in a directory that is not there cannot be written");
  Check(!fs::exists(gDir / "no-such-directory"), "missing: and nothing is made of it");
  // a directory is no log and no destination: refused, and nothing is made of the copy
  Check(!CopyWithoutLinks(gDir / "sub", gDir / "out2.log") && !fs::exists(gDir / "out2.log"),
        "directory: a directory is not copied as a log, and nothing is made of the copy");
  const std::string log = Read(gDir / "urnetwork-app.log");
  Check(!log.empty() && !CopyWithoutLinks(gDir / "urnetwork-app.log", gDir / "sub") && fs::is_directory(gDir / "sub") &&
            Read(gDir / "urnetwork-app.log") == log,
        "directory: nor is a directory a place to save an export, and the log is as it was");
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::cout << "usage: log-export-tests <scratch directory>\n";
    return 2;
  }
  gDir = fs::path(argv[1]);
  std::error_code ignored;
  fs::create_directories(gDir, ignored);
  TestTheCopyIsTheLogWithoutItsLinks();
  TestALongLogIsCopiedInOnePass();
  TestTheLogItselfIsNeverEmptied();
  TestAnExistingExportSurvivesWhatCannotBeCopied();
  fs::remove_all(gDir / "sub", ignored);
  for (const char* name : {"urnetwork-app.log", "exported.log", "elsewhere.log", "earlier.log", "out2.log"}) fs::remove(gDir / name, ignored);
  std::cout << (gCases - gFailures) << "/" << gCases << " log export checks passed\n";
  return gFailures == 0 ? 0 : 1;
}
