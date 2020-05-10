#include "args.hh"
#include "hash.hh"

#include <glob.h>

namespace nix {

void Args::addFlag(Flag && flag_)
{
    auto flag = std::make_shared<Flag>(std::move(flag_));
    if (flag->handler.arity != ArityAny)
        assert(flag->handler.arity == flag->labels.size());
    assert(flag->longName != "");
    longFlags[flag->longName] = flag;
    if (flag->shortName) shortFlags[flag->shortName] = flag;
}

bool pathCompletions = false;
std::shared_ptr<std::set<std::string>> completions;

std::string completionMarker = "___COMPLETE___";

std::optional<std::string> needsCompletion(std::string_view s)
{
    if (!completions) return {};
    auto i = s.find(completionMarker);
    if (i != std::string::npos)
        return std::string(s.begin(), i);
    return {};
}

void Args::parseCmdline(const Strings & _cmdline)
{
    Strings pendingArgs;
    bool dashDash = false;

    Strings cmdline(_cmdline);

    if (auto s = getEnv("NIX_GET_COMPLETIONS")) {
        size_t n = std::stoi(*s);
        assert(n > 0 && n <= cmdline.size());
        *std::next(cmdline.begin(), n - 1) += completionMarker;
        completions = std::make_shared<decltype(completions)::element_type>();
    }

    for (auto pos = cmdline.begin(); pos != cmdline.end(); ) {

        auto arg = *pos;

        /* Expand compound dash options (i.e., `-qlf' -> `-q -l -f',
           `-j3` -> `-j 3`). */
        if (!dashDash && arg.length() > 2 && arg[0] == '-' && arg[1] != '-' && isalpha(arg[1])) {
            *pos = (string) "-" + arg[1];
            auto next = pos; ++next;
            for (unsigned int j = 2; j < arg.length(); j++)
                if (isalpha(arg[j]))
                    cmdline.insert(next, (string) "-" + arg[j]);
                else {
                    cmdline.insert(next, string(arg, j));
                    break;
                }
            arg = *pos;
        }

        if (!dashDash && arg == "--") {
            dashDash = true;
            ++pos;
        }
        else if (!dashDash && std::string(arg, 0, 1) == "-") {
            if (!processFlag(pos, cmdline.end()))
                throw UsageError(format("unrecognised flag '%1%'") % arg);
        }
        else {
            pendingArgs.push_back(*pos++);
            if (processArgs(pendingArgs, false))
                pendingArgs.clear();
        }
    }

    processArgs(pendingArgs, true);
}

void Args::printHelp(const string & programName, std::ostream & out)
{
    std::cout << fmt(ANSI_BOLD "Usage:" ANSI_NORMAL " %s " ANSI_ITALIC "FLAGS..." ANSI_NORMAL, programName);
    for (auto & exp : expectedArgs) {
        std::cout << renderLabels({exp.label});
        // FIXME: handle arity > 1
        if (exp.arity == 0) std::cout << "...";
        if (exp.optional) std::cout << "?";
    }
    std::cout << "\n";

    auto s = description();
    if (s != "")
        std::cout << "\n" ANSI_BOLD "Summary:" ANSI_NORMAL " " << s << ".\n";

    if (longFlags.size()) {
        std::cout << "\n";
        std::cout << ANSI_BOLD "Flags:" ANSI_NORMAL "\n";
        printFlags(out);
    }
}

void Args::printFlags(std::ostream & out)
{
    Table2 table;
    for (auto & flag : longFlags) {
        if (hiddenCategories.count(flag.second->category)) continue;
        table.push_back(std::make_pair(
                (flag.second->shortName ? std::string("-") + flag.second->shortName + ", " : "    ")
                + "--" + flag.first + renderLabels(flag.second->labels),
                flag.second->description));
    }
    printTable(out, table);
}

bool Args::processFlag(Strings::iterator & pos, Strings::iterator end)
{
    assert(pos != end);

    auto process = [&](const std::string & name, const Flag & flag) -> bool {
        ++pos;
        std::vector<std::string> args;
        for (size_t n = 0 ; n < flag.handler.arity; ++n) {
            if (pos == end) {
                if (flag.handler.arity == ArityAny) break;
                throw UsageError("flag '%s' requires %d argument(s)", name, flag.handler.arity);
            }
            if (auto prefix = needsCompletion(*pos))
                if (flag.completer)
                    flag.completer(n, *prefix);
            args.push_back(*pos++);
        }
        flag.handler.fun(std::move(args));
        return true;
    };

    if (string(*pos, 0, 2) == "--") {
        if (auto prefix = needsCompletion(*pos)) {
            for (auto & [name, flag] : longFlags) {
                if (!hiddenCategories.count(flag->category)
                    && hasPrefix(name, std::string(*prefix, 2)))
                    completions->insert("--" + name);
            }
        }
        auto i = longFlags.find(string(*pos, 2));
        if (i == longFlags.end()) return false;
        return process("--" + i->first, *i->second);
    }

    if (string(*pos, 0, 1) == "-" && pos->size() == 2) {
        auto c = (*pos)[1];
        auto i = shortFlags.find(c);
        if (i == shortFlags.end()) return false;
        return process(std::string("-") + c, *i->second);
    }

    if (auto prefix = needsCompletion(*pos)) {
        if (prefix == "-") {
            completions->insert("--");
            for (auto & [flag, _] : shortFlags)
                completions->insert(std::string("-") + flag);
        }
    }

    return false;
}

bool Args::processArgs(const Strings & args, bool finish)
{
    if (expectedArgs.empty()) {
        if (!args.empty())
            throw UsageError(format("unexpected argument '%1%'") % args.front());
        return true;
    }

    auto & exp = expectedArgs.front();

    bool res = false;

    if ((exp.arity == 0 && finish) ||
        (exp.arity > 0 && args.size() == exp.arity))
    {
        std::vector<std::string> ss;
        for (auto & s : args) ss.push_back(s);
        exp.handler(std::move(ss));
        expectedArgs.pop_front();
        res = true;
    }

    if (finish && !expectedArgs.empty() && !expectedArgs.front().optional)
        throw UsageError("more arguments are required");

    return res;
}

Args::Flag Args::Flag::mkHashTypeFlag(std::string && longName, HashType * ht)
{
    return Flag {
        .longName = std::move(longName),
        .description = "hash algorithm ('md5', 'sha1', 'sha256', or 'sha512')",
        .labels = {"hash-algo"},
        .handler = {[ht](std::string s) {
            *ht = parseHashType(s);
            if (*ht == htUnknown)
                throw UsageError("unknown hash type '%1%'", s);
        }},
        .completer = [](size_t index, std::string_view prefix) {
            for (auto & type : hashTypes)
                if (hasPrefix(type, prefix))
                    completions->insert(type);
        }
    };
}

void completePath(size_t, std::string_view s)
{
    if (auto prefix = needsCompletion(s)) {
        pathCompletions = true;
        glob_t globbuf;
        if (glob((*prefix + "*").c_str(), GLOB_NOESCAPE | GLOB_TILDE, nullptr, &globbuf) == 0) {
            for (size_t i = 0; i < globbuf.gl_pathc; ++i)
                completions->insert(globbuf.gl_pathv[i]);
            globfree(&globbuf);
        }
    }
}

void Args::expectPathArg(const std::string & label, string * dest, bool optional)
{
    expectedArgs.push_back({
        .label = label,
        .arity = 1,
        .optional = optional,
        .handler = {[=](std::vector<std::string> ss) {
            completePath(0, ss[0]);
            *dest = ss[0];
        }}
    });
}

void Args::expectPathArgs(const std::string & label, std::vector<std::string> * dest)
{
    expectedArgs.push_back({
        .label = label,
        .arity = 0,
        .optional = false,
        .handler = {[=](std::vector<std::string> ss) {
            for (auto & s : ss)
                completePath(0, s);
            *dest = std::move(ss);
        }}
    });
}

Strings argvToStrings(int argc, char * * argv)
{
    Strings args;
    argc--; argv++;
    while (argc--) args.push_back(*argv++);
    return args;
}

std::string renderLabels(const Strings & labels)
{
    std::string res;
    for (auto label : labels) {
        for (auto & c : label) c = std::toupper(c);
        res += " " ANSI_ITALIC + label + ANSI_NORMAL;
    }
    return res;
}

void printTable(std::ostream & out, const Table2 & table)
{
    size_t max = 0;
    for (auto & row : table)
        max = std::max(max, filterANSIEscapes(row.first, true).size());
    for (auto & row : table) {
        out << "  " << row.first
            << std::string(max - filterANSIEscapes(row.first, true).size() + 2, ' ')
            << row.second << "\n";
    }
}

void Command::printHelp(const string & programName, std::ostream & out)
{
    Args::printHelp(programName, out);

    auto exs = examples();
    if (!exs.empty()) {
        out << "\n" ANSI_BOLD "Examples:" ANSI_NORMAL "\n";
        for (auto & ex : exs)
            out << "\n"
                << "  " << ex.description << "\n" // FIXME: wrap
                << "  $ " << ex.command << "\n";
    }
}

MultiCommand::MultiCommand(const Commands & commands)
    : commands(commands)
{
    expectedArgs.push_back(ExpectedArg{"command", 1, true, [=](std::vector<std::string> ss) {
        assert(!command);
        if (auto prefix = needsCompletion(ss[0])) {
            for (auto & [name, command] : commands)
                if (hasPrefix(name, *prefix))
                    completions->insert(name);
        }
        auto i = commands.find(ss[0]);
        if (i == commands.end())
            throw UsageError("'%s' is not a recognised command", ss[0]);
        command = {ss[0], i->second()};
    }});

    categories[Command::catDefault] = "Available commands";
}

void MultiCommand::printHelp(const string & programName, std::ostream & out)
{
    if (command) {
        command->second->printHelp(programName + " " + command->first, out);
        return;
    }

    out << fmt(ANSI_BOLD "Usage:" ANSI_NORMAL " %s " ANSI_ITALIC "COMMAND FLAGS... ARGS..." ANSI_NORMAL "\n", programName);

    out << "\n" ANSI_BOLD "Common flags:" ANSI_NORMAL "\n";
    printFlags(out);

    std::map<Command::Category, std::map<std::string, ref<Command>>> commandsByCategory;

    for (auto & [name, commandFun] : commands) {
        auto command = commandFun();
        commandsByCategory[command->category()].insert_or_assign(name, command);
    }

    for (auto & [category, commands] : commandsByCategory) {
        out << fmt("\n" ANSI_BOLD "%s:" ANSI_NORMAL "\n", categories[category]);

        Table2 table;
        for (auto & [name, command] : commands) {
            auto descr = command->description();
            if (!descr.empty())
                table.push_back(std::make_pair(name, descr));
        }
        printTable(out, table);
    }
}

bool MultiCommand::processFlag(Strings::iterator & pos, Strings::iterator end)
{
    if (Args::processFlag(pos, end)) return true;
    if (command && command->second->processFlag(pos, end)) return true;
    return false;
}

bool MultiCommand::processArgs(const Strings & args, bool finish)
{
    if (command)
        return command->second->processArgs(args, finish);
    else
        return Args::processArgs(args, finish);
}

}
