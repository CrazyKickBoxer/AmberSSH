// BlastRadiusTests — deterministic risk analysis of a command.
//
// The roadmap's rule is that no language model is ever the authority on
// whether a destructive command is safe. That makes this parser the authority,
// and a parser that is the authority has to be pinned down by tests: what it
// catches, what it deliberately does not claim to understand, and — most
// importantly — that it never quietly rewrites what the user typed.
#include <catch2/catch_test_macros.hpp>

#include <string>

#include "security/BlastRadius.h"

using namespace amber;

namespace
{

RiskLevel Level(const std::string& cmd)
{
    return AnalyseCommand(cmd).level;
}

bool Mentions(const RiskReport& r, const std::string& needle)
{
    for (const RiskFinding& f : r.findings)
        if (f.what.find(needle) != std::string::npos ||
            f.detail.find(needle) != std::string::npos)
            return true;
    return false;
}

} // namespace

TEST_CASE("An ordinary command is not flagged", "[risk]")
{
    for (const char* c : { "ls -la", "cat /etc/hostname", "grep -r foo src",
                           "git status", "df -h", "tail -f app.log",
                           "cd /var/www", "echo hello" })
    {
        const RiskReport r = AnalyseCommand(c);
        INFO(c);
        CHECK(r.level == RiskLevel::None);
        CHECK(r.findings.empty());
        CHECK_FALSE(r.Flagged(RiskPolicy::Everything));
    }
}

TEST_CASE("The command is never rewritten", "[risk]")
{
    // Whatever is shown next to a warning has to be the exact bytes that will
    // be sent. A warning about a command the user did not type is a lie.
    const char* kCmds[] = {
        "rm -rf /", "  rm   -rf   'my dir'  ", "sudo rm -rf /var/log",
        "echo \"$(rm -rf /)\"", "rm -rf \"/home/me",
    };
    for (const char* c : kCmds)
        CHECK(AnalyseCommand(c).command == std::string(c));
}

TEST_CASE("Deleting a root path is critical", "[risk]")
{
    CHECK(Level("rm -rf /") == RiskLevel::Critical);
    CHECK(Level("rm -rf /etc") == RiskLevel::Critical);
    CHECK(Level("rm -rf ~") == RiskLevel::Critical);
    CHECK(Level("rm -fr /usr/") == RiskLevel::Critical);
    CHECK(Level("sudo rm -rf /") == RiskLevel::Critical);
}

TEST_CASE("Recursive delete of an ordinary directory is high, not critical", "[risk]")
{
    // Over-reporting is its own failure: if every rm is critical, the critical
    // confirmation stops meaning anything.
    const RiskReport r = AnalyseCommand("rm -rf /home/josh/project/build");
    CHECK(r.level == RiskLevel::High);
    CHECK(r.program == "rm");
    CHECK_FALSE(r.sudo);
    CHECK(Level("rm build/out.o") == RiskLevel::Medium);
}

TEST_CASE("A wildcard target is reported as undetermined", "[risk]")
{
    const RiskReport r = AnalyseCommand("rm -rf ./build/*");
    CHECK(r.level == RiskLevel::High);
    bool sawUncertain = false;
    for (const RiskFinding& f : r.findings)
        if (f.uncertain)
            sawUncertain = true;
    // What a glob matches is decided by the remote shell. Claiming to know is
    // exactly the kind of confident wrong answer this module exists to avoid.
    CHECK(sawUncertain);
}

TEST_CASE("sudo is resolved through to the real program", "[risk]")
{
    const RiskReport a = AnalyseCommand("sudo -u deploy rm -rf /var/log/app");
    CHECK(a.sudo);
    CHECK(a.program == "rm");
    // High on its own; running as root removes the permission error that
    // would otherwise have stopped it, so one step worse.
    CHECK(a.level == RiskLevel::Critical);

    const RiskReport b = AnalyseCommand("sudo systemctl restart nginx");
    CHECK(b.sudo);
    CHECK(b.program == "systemctl");
    CHECK(b.level == RiskLevel::Critical);

    // Env prefixes and nohup are transparent too.
    CHECK(AnalyseCommand("FOO=bar rm -rf /").program == "rm");
    CHECK(AnalyseCommand("env rm -rf /").program == "rm");
    CHECK(AnalyseCommand("/usr/bin/rm -rf /").program == "rm");
}

TEST_CASE("sudo alone does not create risk", "[risk]")
{
    const RiskReport r = AnalyseCommand("sudo ls -la /root");
    CHECK(r.sudo);
    CHECK(r.level == RiskLevel::None);
}

TEST_CASE("git history rewrites are recognised", "[risk]")
{
    CHECK(Level("git push --force origin main") == RiskLevel::Critical);
    CHECK(Level("git push -f origin main") == RiskLevel::Critical);
    // A lease is genuinely safer, and saying so is what makes the critical
    // warning credible when it does appear.
    CHECK(Level("git push --force-with-lease origin main") == RiskLevel::High);
    CHECK(Level("git push origin main") == RiskLevel::None);
    CHECK(Level("git reset --hard") == RiskLevel::High);
    CHECK(Level("git clean -fd") == RiskLevel::High);
    CHECK(Level("git clean -fdx") == RiskLevel::Critical);
    CHECK(Level("git clean -n") == RiskLevel::None);
    CHECK(Level("git push --delete origin old") == RiskLevel::High);
}

TEST_CASE("Container and cluster commands that destroy data", "[risk]")
{
    CHECK(Level("docker compose down") == RiskLevel::High);
    CHECK(Level("docker compose down -v") == RiskLevel::Critical);
    CHECK(Level("docker compose down --volumes") == RiskLevel::Critical);
    CHECK(Level("docker volume rm data") == RiskLevel::Critical);
    CHECK(Level("docker system prune") == RiskLevel::High);
    CHECK(Level("docker ps -a") == RiskLevel::None);
    CHECK(Level("kubectl delete pod web-1") == RiskLevel::High);
    CHECK(Level("kubectl delete pods --all") == RiskLevel::Critical);
    CHECK(Level("kubectl get pods") == RiskLevel::None);
    CHECK(Level("kubectl drain node-3") == RiskLevel::High);
}

TEST_CASE("Whole-machine and whole-disk commands", "[risk]")
{
    CHECK(Level("shutdown -h now") == RiskLevel::Critical);
    CHECK(Level("reboot") == RiskLevel::Critical);
    CHECK(Level("systemctl poweroff") == RiskLevel::Critical);
    CHECK(Level("mkfs.ext4 /dev/sdb1") == RiskLevel::Critical);
    CHECK(Level("dd if=/dev/zero of=/dev/sda bs=1M") == RiskLevel::Critical);
    // Reading a device is not the dangerous half.
    CHECK(Level("dd if=/dev/sda | gzip > disk.gz") == RiskLevel::None);
}

TEST_CASE("Permission changes on system paths", "[risk]")
{
    CHECK(Level("chmod 777 /etc") == RiskLevel::Critical);
    CHECK(Level("chmod -R 755 /srv/app") == RiskLevel::High);
    CHECK(Level("chown -R www-data /var/www/html") == RiskLevel::High);
    CHECK(Level("chmod +x deploy.sh") == RiskLevel::None);
    CHECK(Mentions(AnalyseCommand("chmod 777 app"), "world-writable"));
}

TEST_CASE("A SQL statement is only matched when it is unmistakable", "[risk]")
{
    CHECK(Level("DROP DATABASE production;") == RiskLevel::Critical);
    CHECK(Level("drop table users;") == RiskLevel::High);
    CHECK(Level("TRUNCATE TABLE sessions;") == RiskLevel::High);
    // No WHERE clause means every row, which is a different decision.
    CHECK(Level("DELETE FROM users;") == RiskLevel::Critical);
    CHECK(Level("DELETE FROM users WHERE id = 3;") == RiskLevel::Medium);
    CHECK(Level("psql -c \"DELETE FROM audit\"") == RiskLevel::Critical);
    // A bare word must never trigger it.
    CHECK(Level("git log --grep 'drop table'") == RiskLevel::None);
    CHECK(Level("echo please do not drop") == RiskLevel::None);
}

TEST_CASE("A pipeline is reported as only partly examined", "[risk]")
{
    const RiskReport r = AnalyseCommand("rm -rf ./tmp | tee removed.log");
    CHECK(r.incomplete);
    CHECK_FALSE(r.incompleteWhy.empty());
    CHECK(r.program == "rm");
    // The tail must NOT be read as arguments to the first command: naming
    // "removed.log" as something being deleted would be a confident lie.
    CHECK_FALSE(Mentions(r, "removed.log"));
    CHECK_FALSE(Mentions(r, "tee"));
}

TEST_CASE("Substitution makes the report explicitly incomplete", "[risk]")
{
    for (const char* c : { "rm -rf $(cat targets.txt)", "rm -rf `pwd`/build",
                           "echo \"$(rm -rf /)\"" })
    {
        const RiskReport r = AnalyseCommand(c);
        INFO(c);
        CHECK(r.incomplete);
        CHECK(r.incompleteWhy.find("substitution") != std::string::npos);
    }
    // And a finding that rests on an expansion is marked uncertain.
    const RiskReport r = AnalyseCommand("rm -rf $TARGET");
    bool uncertain = false;
    for (const RiskFinding& f : r.findings)
        if (f.uncertain)
            uncertain = true;
    CHECK(uncertain);
}

TEST_CASE("An unclosed quote is admitted, not guessed at", "[risk]")
{
    const RiskReport r = AnalyseCommand("rm -rf \"/home/me");
    CHECK(r.incomplete);
    CHECK(r.incompleteWhy.find("quote") != std::string::npos);
}

TEST_CASE("SplitArgs reads a command line the way a shell would", "[risk]")
{
    bool bad = false;
    auto a = SplitArgs("rm -rf \"my dir\" 'other dir'", bad);
    REQUIRE(a.size() == 4);
    CHECK(a[0] == "rm");
    CHECK(a[1] == "-rf");
    CHECK(a[2] == "my dir");
    CHECK(a[3] == "other dir");
    CHECK_FALSE(bad);

    a = SplitArgs("echo a\\ b", bad);
    REQUIRE(a.size() == 2);
    CHECK(a[1] == "a b");

    // An empty quoted argument is a real argument.
    a = SplitArgs("cmd \"\"", bad);
    REQUIRE(a.size() == 2);
    CHECK(a[1].empty());

    SplitArgs("rm 'unclosed", bad);
    CHECK(bad);

    SplitArgs("", bad);
    CHECK_FALSE(bad);
    CHECK(AnalyseCommand("").level == RiskLevel::None);
    CHECK(AnalyseCommand("   ").level == RiskLevel::None);
}

TEST_CASE("IsRootLikePath knows what must never be lost", "[risk]")
{
    for (const char* p : { "/", "/etc", "/etc/", "/usr", "/var", "/home",
                           "/root", "~", "C:", "C:\\", "/*", "." })
    {
        INFO(p);
        CHECK(IsRootLikePath(p));
    }
    for (const char* p : { "/home/josh", "/var/log/app", "build", "./out",
                           "../sibling", "/tmp/scratch", "" })
    {
        INFO(p);
        CHECK_FALSE(IsRootLikePath(p));
    }
}

TEST_CASE("HasUnmodelledSyntax admits what the parser cannot read", "[risk]")
{
    CHECK(HasUnmodelledSyntax("$(pwd)"));
    CHECK(HasUnmodelledSyntax("${HOME}"));
    CHECK(HasUnmodelledSyntax("$HOME/x"));
    CHECK(HasUnmodelledSyntax("`pwd`"));
    CHECK_FALSE(HasUnmodelledSyntax("/var/log"));
    CHECK_FALSE(HasUnmodelledSyntax("build"));
}

TEST_CASE("The policy decides what interrupts, and Off never does", "[risk]")
{
    const RiskReport crit = AnalyseCommand("rm -rf /");
    CHECK(crit.level == RiskLevel::Critical);
    CHECK_FALSE(crit.Flagged(RiskPolicy::Off));
    CHECK(crit.Flagged(RiskPolicy::CriticalOnly));
    CHECK(crit.Flagged(RiskPolicy::Everything));

    const RiskReport high = AnalyseCommand("git reset --hard");
    CHECK_FALSE(high.Flagged(RiskPolicy::CriticalOnly));
    CHECK(high.Flagged(RiskPolicy::HighAndCritical));

    const RiskReport med = AnalyseCommand("rm notes.txt");
    CHECK_FALSE(med.Flagged(RiskPolicy::HighAndCritical));
    CHECK(med.Flagged(RiskPolicy::Standard));
}

TEST_CASE("Friction is reserved for the irreversible", "[risk]")
{
    // If every warning demanded typing, people would learn to type without
    // reading, and the one that mattered would be lost with the rest.
    CHECK(ConfirmFor(RiskLevel::Critical, RiskPolicy::Standard) ==
          ConfirmStyle::TypeHostname);
    CHECK(ConfirmFor(RiskLevel::High, RiskPolicy::Standard) == ConfirmStyle::YesNo);
    CHECK(ConfirmFor(RiskLevel::Medium, RiskPolicy::Standard) == ConfirmStyle::YesNo);
    CHECK(ConfirmFor(RiskLevel::Low, RiskPolicy::Everything) == ConfirmStyle::Notice);
    // Below the policy threshold nothing interrupts at all.
    CHECK(ConfirmFor(RiskLevel::Low, RiskPolicy::Standard) == ConfirmStyle::None);
    CHECK(ConfirmFor(RiskLevel::High, RiskPolicy::CriticalOnly) == ConfirmStyle::None);
    CHECK(ConfirmFor(RiskLevel::None, RiskPolicy::Everything) == ConfirmStyle::None);
    for (int l = 0; l <= static_cast<int>(RiskLevel::Critical); ++l)
        CHECK(ConfirmFor(static_cast<RiskLevel>(l), RiskPolicy::Off) ==
              ConfirmStyle::None);
}

TEST_CASE("The headline names the worst thing, not the first", "[risk]")
{
    const RiskReport r = AnalyseCommand("rm -rf /etc");
    const std::string h = r.Headline();
    CHECK(h.find("root path") != std::string::npos);
    CHECK(h.find("/etc") != std::string::npos);
    CHECK(AnalyseCommand("ls").Headline().find("Nothing") != std::string::npos);

    // When the worst finding is one the parser could not pin down, the
    // headline admits it rather than stating it as fact.
    CHECK(AnalyseCommand("rm -rf $TARGET").Headline().find("unable to determine") !=
          std::string::npos);
}

TEST_CASE("Level names and policy names are all covered", "[risk]")
{
    for (int l = 0; l <= static_cast<int>(RiskLevel::Critical); ++l)
        CHECK(std::string(RiskLevelName(static_cast<RiskLevel>(l))) != "unknown");
    for (int p = 0; p <= static_cast<int>(RiskPolicy::Everything); ++p)
        CHECK(std::string(RiskPolicyName(static_cast<RiskPolicy>(p))) != "unknown");
}
