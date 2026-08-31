#include "luke/build.hpp"

#include <iostream>

#if defined(_WIN32) && !defined(__CYGWIN__)

namespace luke {

int runDapStdio(const BuildOptions & /*baseOpts*/) {
  std::cerr
      << "Error: luke DAP needs a POSIX host (gdb + fork).\n"
      << "  On Windows use WSL, or luke DEBUG on Linux/macOS.\n";
  return 1;
}

}  // namespace luke

#else

#include "luke/build.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace luke {
namespace {

std::string jsonEscape(const std::string &s) {
  std::ostringstream o;
  for (char c : s) {
    if (c == '"' || c == '\\') o << '\\' << c;
    else if (c == '\n') o << "\\n";
    else if (c == '\r') o << "\\r";
    else if (c == '\t') o << "\\t";
    else o << c;
  }
  return o.str();
}

void writeMessage(const std::string &body) {
  std::cout << "Content-Length: " << body.size() << "\r\n\r\n" << body << std::flush;
}

std::string readMessage() {
  std::string line;
  size_t contentLength = 0;
  while (std::getline(std::cin, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) break;
    auto colon = line.find(':');
    if (colon == std::string::npos) continue;
    auto key = line.substr(0, colon);
    auto val = line.substr(colon + 1);
    while (!val.empty() && (val[0] == ' ' || val[0] == '\t')) val.erase(val.begin());
    if (key == "Content-Length") contentLength = (size_t)std::stoul(val);
  }
  if (contentLength == 0) return {};
  std::string body(contentLength, '\0');
  std::cin.read(&body[0], (std::streamsize)contentLength);
  if ((size_t)std::cin.gcount() != contentLength) return {};
  return body;
}

std::string extractMethod(const std::string &msg) {
  auto keyPos = msg.find("\"command\"");
  if (keyPos == std::string::npos) keyPos = msg.find("\"method\"");
  if (keyPos == std::string::npos) return {};
  auto colon = msg.find(':', keyPos);
  if (colon == std::string::npos) return {};
  auto pos = msg.find('"', colon);
  if (pos == std::string::npos) return {};
  auto end = msg.find('"', pos + 1);
  if (end == std::string::npos) return {};
  return msg.substr(pos + 1, end - pos - 1);
}

std::string extractSeq(const std::string &msg) {
  auto pos = msg.find("\"seq\"");
  if (pos == std::string::npos) return "0";
  pos = msg.find_first_of("0123456789", pos + 5);
  if (pos == std::string::npos) return "0";
  size_t end = pos;
  while (end < msg.size() && isdigit((unsigned char)msg[end])) ++end;
  return msg.substr(pos, end - pos);
}

std::string extractStr(const std::string &msg, const char *key) {
  std::string pat = std::string("\"") + key + "\"";
  auto pos = msg.find(pat);
  if (pos == std::string::npos) return {};
  pos = msg.find('"', pos + pat.size());
  if (pos == std::string::npos) return {};
  auto end = pos + 1;
  std::string out;
  while (end < msg.size() && msg[end] != '"') {
    if (msg[end] == '\\' && end + 1 < msg.size()) {
      out.push_back(msg[end + 1]);
      end += 2;
      continue;
    }
    out.push_back(msg[end++]);
  }
  return out;
}

int extractInt(const std::string &msg, const char *key, int def = 0) {
  std::string pat = std::string("\"") + key + "\"";
  auto pos = msg.find(pat);
  if (pos == std::string::npos) return def;
  pos = msg.find_first_of("0123456789-", pos + pat.size());
  if (pos == std::string::npos) return def;
  return std::atoi(msg.c_str() + pos);
}

std::string dirnameOf(std::string path) {
  auto slash = path.find_last_of("/\\");
  if (slash == std::string::npos) return ".";
  return path.substr(0, slash);
}

std::string basenameNoExt(std::string path) {
  auto slash = path.find_last_of("/\\");
  if (slash != std::string::npos) path = path.substr(slash + 1);
  auto dot = path.rfind('.');
  if (dot != std::string::npos) path = path.substr(0, dot);
  return path;
}

std::string findGdb() {
  if (access("/usr/bin/gdb", X_OK) == 0) return "/usr/bin/gdb";
  if (access("/usr/local/bin/gdb", X_OK) == 0) return "/usr/local/bin/gdb";
  return "gdb";
}

std::string readFile(const std::string &path) {
  std::ifstream in(path);
  if (!in) return {};
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

bool writeFile(const std::string &path, const std::string &body) {
  std::ofstream out(path);
  if (!out) return false;
  out << body;
  return (bool)out;
}

std::string findRuntimeInclude() {
  const char *cands[] = {"runtime", "../runtime", "vm/runtime", "/workspace/vm/runtime", nullptr};
  for (int i = 0; cands[i]; ++i)
    if (std::ifstream(std::string(cands[i]) + "/luke_rt.h")) return cands[i];
  return "runtime";
}

std::string findStdlib() {
  if (std::ifstream("stdlib/files.luke")) return "stdlib";
  if (std::ifstream("vm/stdlib/files.luke")) return "vm/stdlib";
  if (std::ifstream("../stdlib/files.luke")) return "../stdlib";
  return "stdlib";
}

struct DapSession {
  int outSeq = 1;
  std::string lukePath;
  std::string binary;
  std::string cPath;
  int gdbIn = -1;  /* write commands to gdb */
  int gdbOut = -1; /* read gdb output */
  pid_t gdbPid = -1;
  bool running = false;
  bool stopped = false;
  std::vector<std::pair<std::string, int>> breaks; /* path, line */
  std::string lastRxJson;
  int varRefLocals = 1000;
  int varRefReactive = 2000;
  int varRefEdges = 3000;
  std::vector<std::string> edgeVars;
};

void sendResponse(DapSession &s, const std::string &reqSeq, const std::string &cmd, bool ok,
                  const std::string &body) {
  std::ostringstream o;
  o << "{\"seq\":" << s.outSeq++ << ",\"type\":\"response\",\"request_seq\":" << reqSeq
    << ",\"success\":" << (ok ? "true" : "false") << ",\"command\":\"" << cmd << "\"";
  if (!body.empty()) o << ",\"body\":" << body;
  o << "}";
  writeMessage(o.str());
}

void sendEvent(DapSession &s, const std::string &event, const std::string &body) {
  std::ostringstream o;
  o << "{\"seq\":" << s.outSeq++ << ",\"type\":\"event\",\"event\":\"" << event << "\"";
  if (!body.empty()) o << ",\"body\":" << body;
  o << "}";
  writeMessage(o.str());
}

void closeGdb(DapSession &s) {
  if (s.gdbIn >= 0) {
    close(s.gdbIn);
    s.gdbIn = -1;
  }
  if (s.gdbOut >= 0) {
    close(s.gdbOut);
    s.gdbOut = -1;
  }
  if (s.gdbPid > 0) {
    int st = 0;
    waitpid(s.gdbPid, &st, WNOHANG);
    s.gdbPid = -1;
  }
}

std::string gdbSend(DapSession &s, const std::string &cmd) {
  if (s.gdbIn < 0 || s.gdbOut < 0) return {};
  std::string line = cmd + "\n";
  if (write(s.gdbIn, line.data(), line.size()) < 0) return {};
  std::string out;
  char buf[4096];
  /* Read until (gdb) prompt — CLI mode. */
  while (true) {
    ssize_t n = read(s.gdbOut, buf, sizeof(buf) - 1);
    if (n <= 0) break;
    buf[n] = 0;
    out.append(buf, (size_t)n);
    if (out.find("(gdb)") != std::string::npos) break;
    if (out.size() > 1 << 20) break;
  }
  return out;
}

bool buildDebugBinary(DapSession &s) {
  auto src = readFile(s.lukePath);
  if (src.empty() && !std::ifstream(s.lukePath)) return false;
  BuildOptions opt;
  opt.sourcePath = s.lukePath;
  opt.stdlibPath = findStdlib();
  auto built = compileLukeToC(src, opt);
  if (!built.ok) return false;
  s.binary = "/tmp/luke_dap_" + basenameNoExt(s.lukePath);
  s.cPath = s.binary + ".luke.c";
  if (!writeFile(s.cPath, built.cSource)) return false;
  std::string rt = findRuntimeInclude();
  std::ostringstream cmd;
  cmd << "cc -O0 -g -fno-inline -std=gnu11 -I\"" << rt << "\" -o \"" << s.binary << "\" \""
      << s.cPath << "\"";
  for (auto &lib : built.linkLibs) {
    if (!lib.empty() && lib[0] == '/')
      cmd << " \"" << lib << "\"";
    else
      cmd << " -l" << lib;
  }
  return std::system(cmd.str().c_str()) == 0;
}

bool startGdb(DapSession &s) {
  closeGdb(s);
  int toGdb[2];
  int fromGdb[2];
  if (pipe(toGdb) != 0 || pipe(fromGdb) != 0) return false;
  std::string gdb = findGdb();
  pid_t pid = fork();
  if (pid < 0) {
    close(toGdb[0]);
    close(toGdb[1]);
    close(fromGdb[0]);
    close(fromGdb[1]);
    return false;
  }
  if (pid == 0) {
    dup2(toGdb[0], STDIN_FILENO);
    dup2(fromGdb[1], STDOUT_FILENO);
    dup2(fromGdb[1], STDERR_FILENO);
    close(toGdb[0]);
    close(toGdb[1]);
    close(fromGdb[0]);
    close(fromGdb[1]);
    execlp(gdb.c_str(), "gdb", "-q", "-nx", s.binary.c_str(), (char *)nullptr);
    _exit(127);
  }
  close(toGdb[0]);
  close(fromGdb[1]);
  s.gdbIn = toGdb[1];
  s.gdbOut = fromGdb[0];
  s.gdbPid = pid;
  /* Drain startup banner until first (gdb) prompt. */
  gdbSend(s, "set pagination off");
  gdbSend(s, "set confirm off");
  gdbSend(s, "directory .");
  gdbSend(s, "directory " + dirnameOf(s.lukePath));
  gdbSend(s, "skip -gfi */luke_rt.h");
  gdbSend(s, "skip -gfi */luke_std.h");
  gdbSend(s, "skip -gfi */argus.h");
  gdbSend(s, "skip -gfi */hanka.h");
  return true;
}

void refreshReactiveSnapshot(DapSession &s) {
  s.lastRxJson.clear();
  s.edgeVars.clear();
  if (s.gdbIn < 0 || !s.stopped) return;
  auto out = gdbSend(s, "printf \"%s\\n\", luke_debug_rx_inspect()");
  auto pos = out.find("{\"cells\":");
  if (pos == std::string::npos) {
    out = gdbSend(s, "printf \"%s\\n\", luke_rx_inspect_cstr(_luke_rx)");
    pos = out.find("{\"cells\":");
  }
  if (pos == std::string::npos) {
    out = gdbSend(s, "call (void)luke_rx_inspect_print(_luke_rx)");
    pos = out.find("{\"cells\":");
  }
  if (pos == std::string::npos) return;
  auto end = out.find('\n', pos);
  s.lastRxJson = out.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
  auto cellsPos = s.lastRxJson.find("\"cells\":[");
  if (cellsPos == std::string::npos) return;
  size_t i = cellsPos;
  while ((i = s.lastRxJson.find("\"name\":\"", i)) != std::string::npos) {
    i += 8;
    auto ne = s.lastRxJson.find('"', i);
    if (ne == std::string::npos) break;
    std::string name = s.lastRxJson.substr(i, ne - i);
    auto depsPos = s.lastRxJson.find("\"deps\":[", ne);
    auto nextCell = s.lastRxJson.find("\"name\":\"", ne + 1);
    if (depsPos == std::string::npos || (nextCell != std::string::npos && depsPos > nextCell)) {
      i = ne + 1;
      continue;
    }
    auto depsEnd = s.lastRxJson.find(']', depsPos);
    std::string deps = s.lastRxJson.substr(depsPos, depsEnd - depsPos);
    std::string depNames;
    size_t d = 0;
    while ((d = deps.find("\"name\":\"", d)) != std::string::npos) {
      d += 8;
      auto de = deps.find('"', d);
      if (de == std::string::npos) break;
      if (!depNames.empty()) depNames += ", ";
      depNames += deps.substr(d, de - d);
      d = de + 1;
    }
    if (!name.empty()) {
      std::string line = name + " = ";
      auto vpos = s.lastRxJson.find("\"value\":\"", ne);
      if (vpos != std::string::npos && (nextCell == std::string::npos || vpos < nextCell)) {
        vpos += 9;
        auto ve = s.lastRxJson.find('"', vpos);
        line += s.lastRxJson.substr(vpos, ve - vpos);
      }
      if (!depNames.empty()) line += "  deps→[" + depNames + "]";
      s.edgeVars.push_back(line);
    }
    i = ne + 1;
  }
}

void handleInitialize(DapSession &s, const std::string &seq) {
  sendResponse(s, seq, "initialize", true,
               "{\"supportsConfigurationDoneRequest\":true,"
               "\"supportsStepInTargetsRequest\":false,"
               "\"supportsCompletionsRequest\":false,"
               "\"exceptionBreakpointFilters\":[]}");
  sendEvent(s, "initialized", "{}");
}

void handleLaunch(DapSession &s, const std::string &seq, const std::string &msg) {
  s.lukePath = extractStr(msg, "program");
  if (s.lukePath.empty()) s.lukePath = extractStr(msg, "path");
  if (s.lukePath.empty()) {
    sendResponse(s, seq, "launch", false,
                 "{\"error\":{\"id\":1,\"format\":\"program (.luke) required\"}}");
    return;
  }
  if (!buildDebugBinary(s)) {
    sendResponse(s, seq, "launch", false, "{\"error\":{\"id\":2,\"format\":\"Build failed\"}}");
    return;
  }
  if (!startGdb(s)) {
    sendResponse(s, seq, "launch", false, "{\"error\":{\"id\":3,\"format\":\"gdb failed\"}}");
    return;
  }
  for (auto &bp : s.breaks) {
    std::ostringstream b;
    b << "break " << bp.first << ":" << bp.second;
    gdbSend(s, b.str());
  }
  if (s.breaks.empty()) gdbSend(s, "break main");
  sendResponse(s, seq, "launch", true, "{}");
  auto runOut = gdbSend(s, "run");
  s.running = true;
  s.stopped = runOut.find("Breakpoint") != std::string::npos ||
              runOut.find("at ") != std::string::npos;
  if (s.stopped) {
    refreshReactiveSnapshot(s);
    sendEvent(s, "stopped",
              "{\"reason\":\"breakpoint\",\"threadId\":1,\"allThreadsStopped\":true}");
  }
}

void handleSetBreakpoints(DapSession &s, const std::string &seq, const std::string &msg) {
  std::string src = extractStr(msg, "path");
  if (src.empty()) src = s.lukePath;
  s.breaks.clear();
  size_t pos = 0;
  std::ostringstream body;
  body << "{\"breakpoints\":[";
  int n = 0;
  while ((pos = msg.find("\"line\"", pos)) != std::string::npos) {
    int line = extractInt(msg.substr(pos), "line", 0);
    pos += 6;
    if (line <= 0) continue;
    s.breaks.push_back({src, line});
    if (n) body << ",";
    body << "{\"id\":" << (n + 1) << ",\"verified\":true,\"line\":" << line << "}";
    ++n;
    if (s.gdbIn >= 0) {
      std::ostringstream b;
      b << "break " << src << ":" << line;
      gdbSend(s, b.str());
    }
  }
  body << "]}";
  sendResponse(s, seq, "setBreakpoints", true, body.str());
}

void handleThreads(DapSession &s, const std::string &seq) {
  sendResponse(s, seq, "threads", true, "{\"threads\":[{\"id\":1,\"name\":\"main\"}]}");
}

void handleStackTrace(DapSession &s, const std::string &seq) {
  std::string frames = "[{\"id\":0,\"name\":\"main\",\"line\":1,\"column\":1,\"source\":{\"path\":\"" +
                       jsonEscape(s.lukePath) + "\"}}]";
  if (s.gdbIn >= 0 && s.stopped) {
    auto out = gdbSend(s, "bt 8");
    auto at = out.find(".luke:");
    if (at != std::string::npos) {
      int line = std::atoi(out.c_str() + at + 6);
      if (line > 0) {
        std::ostringstream f;
        f << "[{\"id\":0,\"name\":\"frame\",\"line\":" << line
          << ",\"column\":1,\"source\":{\"path\":\"" << jsonEscape(s.lukePath) << "\"}}]";
        frames = f.str();
      }
    }
  }
  sendResponse(s, seq, "stackTrace", true, "{\"stackFrames\":" + frames + ",\"totalFrames\":1}");
}

void handleScopes(DapSession &s, const std::string &seq) {
  refreshReactiveSnapshot(s);
  std::ostringstream body;
  body << "{\"scopes\":["
       << "{\"name\":\"Locals\",\"variablesReference\":" << s.varRefLocals
       << ",\"expensive\":false},"
       << "{\"name\":\"Reactive\",\"variablesReference\":" << s.varRefReactive
       << ",\"expensive\":false}"
       << "]}";
  sendResponse(s, seq, "scopes", true, body.str());
}

void handleVariables(DapSession &s, const std::string &seq, const std::string &msg) {
  int ref = extractInt(msg, "variablesReference", 0);
  refreshReactiveSnapshot(s);
  std::ostringstream body;
  body << "{\"variables\":[";
  if (ref == s.varRefReactive || ref == s.varRefEdges) {
    for (size_t i = 0; i < s.edgeVars.size(); ++i) {
      if (i) body << ",";
      auto eq = s.edgeVars[i].find(" = ");
      std::string name = eq == std::string::npos ? s.edgeVars[i] : s.edgeVars[i].substr(0, eq);
      std::string val = eq == std::string::npos ? "" : s.edgeVars[i].substr(eq + 3);
      body << "{\"name\":\"" << jsonEscape(name) << "\",\"value\":\"" << jsonEscape(val)
           << "\",\"variablesReference\":0}";
    }
    if (s.edgeVars.empty() && !s.lastRxJson.empty()) {
      body << "{\"name\":\"graph\",\"value\":\"" << jsonEscape(s.lastRxJson)
           << "\",\"variablesReference\":0}";
    }
  } else if (s.gdbIn >= 0 && s.stopped) {
    auto out = gdbSend(s, "info locals");
    std::istringstream in(out);
    std::string line;
    int n = 0;
    while (std::getline(in, line)) {
      if (line.find("(gdb)") != std::string::npos) continue;
      auto eq = line.find(" = ");
      if (eq == std::string::npos) continue;
      if (n++) body << ",";
      body << "{\"name\":\"" << jsonEscape(line.substr(0, eq)) << "\",\"value\":\""
           << jsonEscape(line.substr(eq + 3)) << "\",\"variablesReference\":0}";
    }
  }
  body << "]}";
  sendResponse(s, seq, "variables", true, body.str());
}

void handleStep(DapSession &s, const std::string &seq, const std::string &cmd, const char *gdbCmd,
                const char *reason) {
  sendResponse(s, seq, cmd, true, "{}");
  if (s.gdbIn < 0) return;
  auto out = gdbSend(s, gdbCmd);
  s.stopped = out.find("at ") != std::string::npos || out.find("Breakpoint") != std::string::npos;
  if (out.find("exited") != std::string::npos) {
    s.running = false;
    s.stopped = false;
    sendEvent(s, "terminated", "{}");
    return;
  }
  if (s.stopped) {
    refreshReactiveSnapshot(s);
    std::ostringstream b;
    b << "{\"reason\":\"" << reason << "\",\"threadId\":1,\"allThreadsStopped\":true}";
    sendEvent(s, "stopped", b.str());
  }
}

void handleDisconnect(DapSession &s, const std::string &seq) {
  if (s.gdbIn >= 0) {
    gdbSend(s, "kill");
    gdbSend(s, "quit");
    closeGdb(s);
  }
  sendResponse(s, seq, "disconnect", true, "{}");
}

}  // namespace

int runDapStdio(const BuildOptions & /*baseOpts*/) {
  DapSession s;
  sendEvent(s, "output",
            "{\"category\":\"console\",\"output\":\"Luke DAP — gdb backend; Reactive scope shows "
            "cell values + dependency edges\\n\"}");
  for (;;) {
    auto msg = readMessage();
    if (msg.empty()) break;
    auto cmd = extractMethod(msg);
    auto seq = extractSeq(msg);
    if (cmd == "initialize")
      handleInitialize(s, seq);
    else if (cmd == "launch")
      handleLaunch(s, seq, msg);
    else if (cmd == "setBreakpoints")
      handleSetBreakpoints(s, seq, msg);
    else if (cmd == "configurationDone")
      sendResponse(s, seq, cmd, true, "{}");
    else if (cmd == "threads")
      handleThreads(s, seq);
    else if (cmd == "stackTrace")
      handleStackTrace(s, seq);
    else if (cmd == "scopes")
      handleScopes(s, seq);
    else if (cmd == "variables")
      handleVariables(s, seq, msg);
    else if (cmd == "next")
      handleStep(s, seq, cmd, "next", "step");
    else if (cmd == "stepIn")
      handleStep(s, seq, cmd, "step", "step");
    else if (cmd == "stepOut")
      handleStep(s, seq, cmd, "finish", "step");
    else if (cmd == "continue")
      handleStep(s, seq, cmd, "continue", "breakpoint");
    else if (cmd == "disconnect" || cmd == "terminate") {
      handleDisconnect(s, seq);
      break;
    } else {
      sendResponse(s, seq, cmd.empty() ? "unknown" : cmd, true, "{}");
    }
  }
  closeGdb(s);
  return 0;
}

}  // namespace luke

#endif  // !_WIN32
