#include "iets_run.hh"

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unistd.h>
#include <vector>

#include "nix_eval.hh"
#include "process.hh"

namespace nixcieval {

namespace {

// The programs this tool runs, fixed at build time so that the iets that was
// tested is the one that runs.
const char * const kIetsProgram = NIX_CI_EVAL_IETS;
const char * const kGitProgram = NIX_CI_EVAL_GIT;

// How much of iets's stderr to show when it fails.
constexpr std::size_t kLogTailBytes = 4096;

std::string makeTempDir() {
    const char * base = std::getenv("TMPDIR");
    std::string pattern =
        std::string(base != nullptr && *base != '\0' ? base : "/tmp") +
        "/nix-ci-eval-XXXXXX";
    if (mkdtemp(pattern.data()) == nullptr) {
        throw std::runtime_error(std::string("cannot create a temporary "
                                             "directory: ") +
                                 std::strerror(errno));
    }
    return pattern;
}

std::vector<std::string> inCheckout(const std::string & checkout,
                                    std::vector<std::string> args) {
    args.insert(args.begin(), {"-C", checkout});
    return args;
}

// Check out the commit the root names into 'checkout', and return its rev.
// Throws with a message that names no URL, since a URL can carry a token.
std::string checkOut(const GitRoot & root, bool shallow,
                     const std::string & checkout) {
    std::filesystem::create_directories(checkout);
    auto git = [&](std::vector<std::string> args,
                   std::string * out = nullptr) {
        if (!runProgram(kGitProgram, inCheckout(checkout, std::move(args)),
                        out)) {
            throw std::runtime_error(
                "could not check out the flake's repository");
        }
    };
    git({"-c", "init.defaultBranch=nix-ci-eval", "init", "--quiet"});
    git(gitFetchArgs(root, shallow));
    std::string rev;
    if (root.rev) {
        rev = *root.rev;
    } else {
        git({"rev-parse", "FETCH_HEAD"}, &rev);
        while (!rev.empty() && (rev.back() == '\n' || rev.back() == '\r')) {
            rev.pop_back();
        }
    }
    git({"-c", "advice.detachedHead=false", "checkout", "--quiet", "--detach",
         rev});
    return rev;
}

} // namespace

std::variant<std::unique_ptr<IetsRun>, NotForIets>
IetsRun::start(const Args & args) {
    if (std::string(kIetsProgram).empty()) {
        return NotForIets{"this build of nix-ci-eval has no iets"};
    }

    GitRoot root;
    std::string socket;
    try {
        auto decided = gitRootFor(describeFlakeRef(args.flakeRef));
        if (auto * why = std::get_if<NotForIets>(&decided)) {
            return *why;
        }
        root = std::get<GitRoot>(decided);
        auto daemon = daemonSocketPath();
        if (!daemon) {
            return NotForIets{"the store is not served by a Nix daemon, and "
                              "iets can only write derivations through one"};
        }
        socket = *daemon;
    } catch (const std::exception & e) {
        return NotForIets{e.what()};
    }

    std::string tempDir = makeTempDir();
    auto removeTempDir = [&]() {
        std::error_code ignored;
        std::filesystem::remove_all(tempDir, ignored);
    };
    try {
        std::string checkout = tempDir + "/checkout";
        std::string rev =
            checkOut(root, fetchShallow(root, args.shallow), checkout);

        std::string logPath = tempDir + "/iets.log";
        int logFd = open(logPath.c_str(),
                         O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
        if (logFd < 0) {
            throw std::runtime_error(std::string("cannot open the iets log: ") +
                                     std::strerror(errno));
        }
        IetsInvocation invocation{
            .flakeRef = ietsFlakeRef(checkout, rev, root.subdir),
            .system = args.system,
            .storeSocket = socket,
            .workers = args.workers,
            .maxMemoryMiB = args.maxMemoryMiB,
            .impure = args.impure,
        };
        Spawned spawned;
        try {
            spawned = spawnProgram(kIetsProgram, ietsArgs(invocation), logFd);
        } catch (...) {
            close(logFd);
            throw;
        }
        close(logFd);
        return std::unique_ptr<IetsRun>(
            new IetsRun(std::move(tempDir), spawned.pid, spawned.stdoutFd));
    } catch (const std::exception & e) {
        removeTempDir();
        return NotForIets{e.what()};
    }
}

IetsRun::IetsRun(std::string tempDir, pid_t pid, int stdoutFd)
    : tempDir_(std::move(tempDir)), pid_(pid),
      output_(std::make_unique<LineReader>(stdoutFd)) {}

IetsRun::~IetsRun() {
    abandon();
    std::error_code ignored;
    std::filesystem::remove_all(tempDir_, ignored);
}

bool IetsRun::finish() {
    output_.reset();
    if (pid_ < 0) {
        return false;
    }
    bool succeeded = waitSucceeded(pid_);
    pid_ = -1;
    return succeeded;
}

void IetsRun::abandon() {
    output_.reset();
    if (pid_ > 0) {
        kill(pid_, SIGKILL);
        waitSucceeded(pid_);
        pid_ = -1;
    }
}

std::string IetsRun::logTail() const {
    std::ifstream log(tempDir_ + "/iets.log", std::ios::binary);
    std::stringstream contents;
    contents << log.rdbuf();
    std::string text = contents.str();
    if (text.size() > kLogTailBytes) {
        text = "..." + text.substr(text.size() - kLogTailBytes);
    }
    return text;
}

} // namespace nixcieval
