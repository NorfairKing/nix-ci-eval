#include "iets.hh"

#include <set>
#include <stdexcept>

namespace nixcieval {

namespace {

// Percent-encode a filesystem path for a file:// URL, leaving the characters
// a path is usually made of alone so the URL stays readable in logs.
std::string encodePath(const std::string & path) {
    static const char * hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : path) {
        bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                     (c >= '0' && c <= '9') || c == '/' || c == '-' ||
                     c == '_' || c == '.' || c == '~';
        if (plain) {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 0xF]);
        }
    }
    return out;
}

} // namespace

std::variant<GitRoot, NotForIets> gitRootFor(const RootRef & root) {
    if (root.type != "git") {
        return NotForIets{"the flake is a '" + root.type +
                          "' input, and only a git repository can be checked "
                          "out for iets"};
    }
    // `shallow` is decided here rather than passed on, so it is allowed
    // whatever its value. Everything else changes what Nix would fetch, or
    // checks it, in a way a plain checkout would not reproduce.
    const std::set<std::string> understood = {"type", "url", "ref", "rev",
                                              "shallow"};
    for (const auto & [key, value] : root.attrs) {
        if (understood.count(key) == 0) {
            return NotForIets{"the flake reference sets '" + key +
                              "', which a checkout for iets would not honour"};
        }
    }
    auto url = root.attrs.find("url");
    if (url == root.attrs.end() || url->second.empty()) {
        return NotForIets{"the flake reference names no repository"};
    }
    GitRoot git;
    git.url = url->second;
    if (auto rev = root.attrs.find("rev"); rev != root.attrs.end()) {
        git.rev = rev->second;
    }
    if (auto ref = root.attrs.find("ref"); ref != root.attrs.end()) {
        git.ref = ref->second;
    }
    git.subdir = root.subdir;
    return git;
}

bool fetchShallow(const GitRoot & root, bool shallowAllowed) {
    return shallowAllowed && root.url.rfind("file:", 0) != 0;
}

std::vector<std::string> gitFetchArgs(const GitRoot & root, bool shallow) {
    std::vector<std::string> args = {"fetch", "--quiet", "--no-tags"};
    if (shallow) {
        args.push_back("--depth");
        args.push_back("1");
    }
    args.push_back("--");
    args.push_back(root.url);
    if (root.rev) {
        args.push_back(*root.rev);
    } else if (root.ref) {
        args.push_back(*root.ref);
    } else {
        args.push_back("HEAD");
    }
    return args;
}

std::string ietsFlakeRef(const std::string & checkoutDir,
                         const std::string & rev, const std::string & subdir) {
    std::string ref = "git+file://" + encodePath(checkoutDir) + "?rev=" + rev;
    if (!subdir.empty()) {
        ref += "&dir=" + encodePath(subdir);
    }
    return ref;
}

std::string ietsSelectExpr(const std::string & system) {
    std::string sys = nixStringLiteral(system);
    // [ref:maxAttrPathDepth]
    std::string maxDepth = std::to_string(kMaxAttrPathDepth);
    return "root: let"
           "  o = root.outputs;"
           "  mark = depth: v:"
           "    if !(builtins.isAttrs v) || (v.type or null) == \"derivation\""
           "    then v"
           "    else if depth >= " +
           maxDepth +
           "    then throw \"attribute path nested too deep\""
           "    else builtins.mapAttrs (_: mark (depth + 1)) v"
           "      // { recurseForDerivations = true; };"
           "in mark 0 {"
           "  packages." +
           sys + " = o.packages." + sys +
           " or {};"
           "  checks." +
           sys + " = o.checks." + sys +
           " or {};"
           "  devShells." +
           sys + " = o.devShells." + sys +
           " or {};"
           "  formatter." +
           sys + " = o.formatter." + sys +
           " or {};"
           "  devShell." +
           sys + " = o.devShell." + sys + " or {};" + "}";
}

std::vector<std::string> ietsArgs(const IetsInvocation & invocation) {
    std::vector<std::string> args = {
        "eval-jobs",
        "--flake",
        invocation.flakeRef,
        "--select",
        ietsSelectExpr(invocation.system),
        "--check-cache-status",
        "--eval-store",
        "unix://" + invocation.storeSocket,
        "--max-memory-size",
        std::to_string(invocation.maxMemoryMiB),
        "--quiet",
    };
    if (invocation.workers) {
        args.push_back("--workers");
        args.push_back(std::to_string(*invocation.workers));
    }
    if (invocation.impure) {
        args.push_back("--impure");
    }
    return args;
}

IetsLine parseIetsLine(const std::string & line) {
    auto message = nlohmann::json::parse(line);
    if (!message.is_object() || !message.contains("attrPath")) {
        return IetsIgnored{};
    }
    auto attrPath = message.at("attrPath").get<AttrPath>();
    if (message.contains("error")) {
        return IetsFailed{std::move(attrPath)};
    }
    if (!message.contains("drvPath")) {
        return IetsIgnored{};
    }
    return IetsJob{
        .attrPath = std::move(attrPath),
        .drvPath = message.at("drvPath").get<std::string>(),
        .isCached = message.at("isCached").get<bool>(),
    };
}

} // namespace nixcieval
