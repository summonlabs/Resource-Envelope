// Crash-recovery and cross-process writer-exclusion validation.
//
// The commit protocol has four stages and the manifest replacement is the only atomic step.
// This suite proves the two properties that make that true:
//
//  1. A process killed at any stage boundary leaves a store that either recovers to the last
//     published generation or refuses to open. It never reports a state that no publication
//     authorised, and it never starts empty over a store that holds something.
//  2. Writer exclusion is enforced by the operating system across independent processes, and a
//     process that dies holding the lock does not leave the store permanently locked.
//
// Interrupted commits are produced by re-executing this same binary, asked to terminate at a
// named stage boundary. The interruption is therefore a real process death at a real boundary.
//
// This suite is Windows-specific because it uses process creation and termination directly.

#include <resource_envelope/service.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include <windows.h>

#include <tlhelp32.h>

#include "support/testing.hpp"

using namespace resource_envelope;

namespace {

// The exit status the store uses for a fault-injected commit. It is not a status the library
// ever returns, so it distinguishes an injected abort from a refused operation.
constexpr int kAbortExitCode = 3;
constexpr DWORD kChildWaitMs = 60000U;

std::string executable_path() {
  char buffer[MAX_PATH] = {};
  const DWORD length = ::GetModuleFileNameA(nullptr, buffer, MAX_PATH);
  return std::string(buffer, static_cast<std::size_t>(length));
}

std::string quote(const std::string& text) { return std::string("\"") + text + "\""; }

// A child process is started from this same binary with a role argument. The directive that
// asks it to die arrives through the environment, so the child performs no argument parsing of
// its own beyond its role.
struct ChildProcess {
  PROCESS_INFORMATION information{};
  bool started = false;

  ChildProcess() = default;
  ChildProcess(const ChildProcess&) = delete;
  ChildProcess& operator=(const ChildProcess&) = delete;
  ChildProcess(ChildProcess&& other) noexcept : information(other.information), started(other.started) {
    other.started = false;
    other.information = PROCESS_INFORMATION{};
  }
  ~ChildProcess() {
    if (!started) return;
    ::CloseHandle(information.hThread);
    ::CloseHandle(information.hProcess);
  }

  [[nodiscard]] bool wait() const {
    if (!started) return false;
    return ::WaitForSingleObject(information.hProcess, kChildWaitMs) == WAIT_OBJECT_0;
  }
  [[nodiscard]] unsigned long exit_code() const {
    DWORD code = 0U;
    if (!started) return 0U;
    ::GetExitCodeProcess(information.hProcess, &code);
    return static_cast<unsigned long>(code);
  }
  // The child is launched through the command interpreter, so the process that actually holds
  // the store is a grandchild of the process created here. Terminating only the interpreter
  // would leave the holder running and the store locked, which is precisely the distinction this
  // case exists to observe, so the whole descendant tree is terminated.
  void terminate_tree() {
    if (!started) return;
    const DWORD root_pid = information.dwProcessId;
    std::vector<DWORD> descendants;
    for (int pass = 0; pass < 4; ++pass) {
      const HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0U);
      if (snapshot == INVALID_HANDLE_VALUE) break;
      PROCESSENTRY32W entry{};
      entry.dwSize = sizeof(entry);
      bool grew = false;
      if (::Process32FirstW(snapshot, &entry) != 0) {
        do {
          const DWORD parent = entry.th32ParentProcessID;
          const DWORD child = entry.th32ProcessID;
          const bool parent_is_root = parent == root_pid;
          const bool parent_is_descendant =
              std::find(descendants.begin(), descendants.end(), parent) != descendants.end();
          if ((parent_is_root || parent_is_descendant) &&
              std::find(descendants.begin(), descendants.end(), child) == descendants.end()) {
            descendants.push_back(child);
            grew = true;
          }
        } while (::Process32NextW(snapshot, &entry) != 0);
      }
      ::CloseHandle(snapshot);
      if (!grew) break;
    }
    for (const DWORD pid : descendants) {
      const HANDLE process = ::OpenProcess(PROCESS_TERMINATE, FALSE, pid);
      if (process == nullptr) continue;
      ::TerminateProcess(process, 1U);
      ::CloseHandle(process);
    }
    ::TerminateProcess(information.hProcess, 1U);
    (void)wait();
  }
};

// The child is launched through the command interpreter, because the directive that asks it to
// terminate reaches it through the environment block. "cmd.exe" is resolved from its canonical
// system location rather than from the search path, so the launch does not depend on ambient
// configuration.
std::string system_command_interpreter() {
  char buffer[MAX_PATH] = {};
  const UINT length = ::GetSystemDirectoryA(buffer, MAX_PATH);
  std::string path(buffer, static_cast<std::size_t>(length));
  path += "\\cmd.exe";
  return path;
}

ChildProcess start_child(const std::string& arguments, const std::string& abort_stage) {
  ChildProcess child;
  const std::string interpreter = system_command_interpreter();
  const std::string command_line = quote(interpreter) + " /d /s /c " + quote(arguments);
  std::string environment;
  if (!abort_stage.empty()) {
    // The block must end with a second terminator, so the variable is terminated twice.
    environment = std::string("RESOURCE_ENVELOPE_ABORT_AT=") + abort_stage + std::string("\0\0", 2U);
  }
  STARTUPINFOA startup{};
  startup.cb = sizeof(startup);
  std::vector<char> mutable_line(command_line.begin(), command_line.end());
  mutable_line.push_back('\0');
  const BOOL created = ::CreateProcessA(
      interpreter.c_str(), mutable_line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
      environment.empty() ? nullptr : const_cast<char*>(environment.c_str()), nullptr, &startup,
      &child.information);
  child.started = created != 0;
  return child;
}

StoreOptions create_options() {
  StoreOptions options;
  options.mode = OpenMode::OpenOrCreate;
  return options;
}

StoreOptions existing_options() {
  StoreOptions options;
  options.mode = OpenMode::OpenExisting;
  return options;
}

StoreOptions locked_options(OpenMode mode) {
  StoreOptions options;
  options.mode = mode;
  options.lock.acquire_timeout_ms = 250U;
  options.lock.retry_interval_ms = 25U;
  return options;
}

Envelope envelope_for(const std::string& id, std::uint64_t watts) {
  Envelope envelope;
  envelope.id = id;
  envelope.scope.kind = EnvelopeScopeKind::Tenant;
  envelope.scope.identity.id = id + "-tenant";
  envelope.scope.identity.generation = 1U;
  envelope.site_id = "site-one";
  envelope.require_binding_confirmation = false;
  envelope.provenance.authority = "facility-authority";
  envelope.provenance.actor = "operator";
  envelope.provenance.declared_at = 1767225600000000000LL;
  DimensionSpec power;
  power.kind = DimensionKind::PowerDrawWatts;
  power.hard_limit = watts * kNanounitsPerUnit;
  envelope.dimensions.push_back(power);
  return envelope;
}

DeclareInput declaration_for(const std::string& id, std::uint64_t watts, const std::string& key,
                            Timestamp at) {
  DeclareInput input;
  input.envelope = envelope_for(id, watts);
  input.idempotency_key = key;
  input.requested_at = at;
  return input;
}

ReviseInput revision_for(const std::string& id, std::uint64_t watts, const std::string& key,
                         Timestamp at) {
  ReviseInput input;
  input.envelope = envelope_for(id, watts);
  input.idempotency_key = key;
  input.requested_at = at;
  return input;
}

class ScratchStore {
 public:
  explicit ScratchStore(const std::string& name) {
    path_ = std::filesystem::current_path() / (name + "-" + std::to_string(::GetCurrentProcessId()));
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }
  ~ScratchStore() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }
  ScratchStore(const ScratchStore&) = delete;
  ScratchStore& operator=(const ScratchStore&) = delete;
  [[nodiscard]] const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};

}  // namespace

// The child role. A child is a normal process driving the store, asked to die at one stage
// boundary; it is never a special build of the library.
static int run_child_role(const std::string& store, const std::string& envelope_id, bool declare_only) {
  const Result<std::unique_ptr<EnvelopeService>> service =
      EnvelopeService::open(std::filesystem::path(store), create_options());
  if (!service.ok()) return 4;
  if (!service.value()->declare(declaration_for(envelope_id, 100ULL, "declare-1",
                                                     1767225600000000000LL))
           .ok()) {
    return 5;
  }
  if (declare_only) {
    // Holding the write handle open is the point of this role: the caller uses it to prove that
    // the writer lock is held across processes until the holder exits.
    ::Sleep(kChildWaitMs);
    return 0;
  }
  if (!service.value()->revise(revision_for(envelope_id, 40ULL, "revise-1",
                                            1767225600000000000LL + 1000LL))
           .ok()) {
    return 6;
  }
  return 0;
}
// ---------------------------------------------------------------------------
// Writer exclusion across independent processes
// ---------------------------------------------------------------------------

RE_TEST(a_second_writer_is_refused_while_another_process_holds_the_store) {
  const ScratchStore scratch("re-crash-lock");
  // A first process opens the store for writing and holds its handle open.
  ChildProcess holder =
      start_child(quote(executable_path()) + " --child --store " + quote(scratch.path().string()) +
                      " --envelope lock-envelope --hold", "");
  RE_REQUIRE(holder.started);

  // The holder declares before sleeping, so the store exists and is non-empty before the
  // contender runs. Waiting for the manifest to appear keeps the test free of a fixed delay.
  bool declared = false;
  for (int attempt = 0; attempt < 400 && !declared; ++attempt) {
    std::error_code error;
    declared = std::filesystem::exists(scratch.path() / "manifest.renv", error) && !error;
    if (!declared) ::Sleep(25U);
  }
  RE_CHECK(declared);

  // A contender that can write is refused, and refused for the reason that says so.
  const Result<std::unique_ptr<EnvelopeService>> contender =
      EnvelopeService::open(scratch.path(), locked_options(OpenMode::OpenExisting));
  // The holder must still be running, or the contention this case exists to create is absent.
  RE_CHECK_EQ(static_cast<long>(::WaitForSingleObject(holder.information.hProcess, 0U)),
              static_cast<long>(WAIT_TIMEOUT));
  RE_REQUIRE(!contender.ok());
  RE_CHECK_EQ(contender.status().code(), StatusCode::StoreWriterBusy);

  // Readers share the store with each other, and a writer excludes them. The exact pairing is
  // the platform's to decide: on Windows a shared range lock cannot coexist with an exclusive
  // one, so a reader is refused while a writer holds the store. That is the stricter of the two
  // behaviours and it is asserted here, because the property that matters is that a reader is
  // never admitted alongside a writer that could change what it is reading.
  {
    const Result<std::unique_ptr<EnvelopeService>> reader =
        EnvelopeService::open(scratch.path(), locked_options(OpenMode::ReadOnly));
    RE_CHECK(!reader.ok());
    RE_CHECK_EQ(reader.status().code(), StatusCode::StoreWriterBusy);
  }

  // When the holder is killed, the operating system releases its lock. A crash must not leave
  // a store permanently locked, so the next writer is admitted.
  holder.terminate_tree();
  // The kernel releases a dead process's locks, and that release becomes observable to another
  // process shortly afterwards. The successor is therefore given a bounded window to be admitted;
  // failure to be admitted inside the window is exactly the defect this case reports.
  Result<std::unique_ptr<EnvelopeService>> successor =
      EnvelopeService::open(scratch.path(), locked_options(OpenMode::OpenExisting));
  for (int attempt = 0; attempt < 40 && !successor.ok(); ++attempt) {
    ::Sleep(50U);
    successor = EnvelopeService::open(scratch.path(), locked_options(OpenMode::OpenExisting));
  }
  RE_REQUIRE(successor.ok());
  const Result<EnvelopeView> reread = successor.value()->get_envelope("lock-envelope");
  RE_REQUIRE(reread.ok());
  RE_CHECK_EQ(reread.value().envelope.revision, 1ULL);
}

// ---------------------------------------------------------------------------
// Crash recovery at every commit-stage boundary
// ---------------------------------------------------------------------------

RE_TEST(a_process_killed_at_any_commit_stage_leaves_a_recoverable_store) {
  const char* const stages[] = {"fence", "frames", "verify", "publish"};
  for (const char* const stage : stages) {
    const ScratchStore scratch(std::string("re-crash-") + stage);
    // A first commit is allowed to complete, so the store has a published generation that a
    // later recovery can fall back to.
    {
      const Result<std::unique_ptr<EnvelopeService>> seed =
          EnvelopeService::open(scratch.path(), create_options());
      RE_REQUIRE(seed.ok());
      RE_REQUIRE(seed.value()
                     ->declare(declaration_for("crash-envelope", 100ULL, "declare-1",
                                                 1767225600000000000LL))
                     .ok());
    }
    const Digest published_digest = [&scratch] {
      const Result<std::unique_ptr<EnvelopeService>> probe =
          EnvelopeService::open(scratch.path(), existing_options());
      if (!probe.ok()) return Digest{};
      const Result<EnvelopeView> view = probe.value()->get_envelope("crash-envelope");
      return view.ok() ? view.value().record_digest : Digest{};
    }();
    RE_CHECK(published_digest.known());

    // A second commit is killed at the named boundary. The child is expected to die with the
    // fault-injection status, which is how the test knows the interruption actually happened
    // at the requested stage rather than somewhere else.
    const ChildProcess child =
        start_child(quote(executable_path()) + " --child --store " + quote(scratch.path().string()) +
                        " --envelope crash-envelope", stage);
    RE_REQUIRE(child.started);
    RE_CHECK(child.wait());
    RE_CHECK_EQ(static_cast<long>(child.exit_code()), static_cast<long>(kAbortExitCode));

    // Recovery either reproduces the last published generation exactly, or refuses to open.
    // It must never report a state that no publication authorised, and it must never start
    // empty over a store that holds something.
    const Result<std::unique_ptr<EnvelopeService>> recovered =
        EnvelopeService::open(scratch.path(), existing_options());
    if (!recovered.ok()) {
      // A refusal is an acceptable outcome for an interrupted commit: the store failed closed
      // rather than serving a state that no publication authorised.
      continue;
    }
    const Result<EnvelopeView> view = recovered.value()->get_envelope("crash-envelope");
    RE_REQUIRE(view.ok());
    // The killed commit either never became visible or became visible whole. Both outcomes are
    // correct; what is not correct is a revision that exists without the revision it supersedes.
    RE_CHECK(view.value().envelope.revision == 1ULL || view.value().envelope.revision == 2ULL);
    if (view.value().envelope.revision == 1ULL) {
      RE_CHECK_EQ(view.value().record_digest, published_digest);
    } else {
      RE_CHECK_EQ(view.value().envelope.revision, 2ULL);
      // The recovered revision is a revision of the same envelope, identified by its revision
      // number and lineage. It carries no supersedes pointer, because a pointer naming the
      // envelope that makes it is a self-reference and is refused at evaluation.
      RE_CHECK(!view.value().envelope.supersedes.has_value());
    }
    // The recovered state is committed to by the manifest, so the store is usable: a further
    // commit succeeds and is visible after another reopen.
    const Result<EnvelopeRevision> after = recovered.value()->revise(
        revision_for("crash-envelope", 30ULL, "revise-after-crash",
                     1767225600000000000LL + 2000LL));
    RE_CHECK(after.ok());
  }
}

RE_TEST(an_interrupted_commit_never_publishes_a_partial_generation) {
  const ScratchStore scratch("re-crash-partial");
  // The first commit publishes, the second is killed before the manifest is replaced. The
  // manifest is the commit point, so the second revision must not be visible afterwards.
  {
    const Result<std::unique_ptr<EnvelopeService>> seed =
        EnvelopeService::open(scratch.path(), create_options());
    RE_REQUIRE(seed.ok());
    RE_REQUIRE(
        seed.value()
            ->declare(declaration_for("partial-envelope", 100ULL, "declare-1",
                                        1767225600000000000LL))
            .ok());
  }
  const ChildProcess child =
      start_child(quote(executable_path()) + " --child --store " + quote(scratch.path().string()) +
                      " --envelope partial-envelope", "publish");
  RE_REQUIRE(child.started);
  RE_CHECK(child.wait());
  RE_CHECK_EQ(static_cast<long>(child.exit_code()), static_cast<long>(kAbortExitCode));

  const Result<std::unique_ptr<EnvelopeService>> recovered =
      EnvelopeService::open(scratch.path(), existing_options());
  RE_REQUIRE(recovered.ok());
  const Result<EnvelopeView> view = recovered.value()->get_envelope("partial-envelope");
  RE_REQUIRE(view.ok());
  RE_CHECK_EQ(view.value().envelope.revision, 1ULL);
  const Result<std::vector<EnvelopeView>> all = recovered.value()->list_envelopes();
  RE_REQUIRE(all.ok());
  RE_CHECK_EQ(all.value().size(), std::size_t{1});
}

RE_TEST(a_crash_during_the_first_commit_leaves_a_store_the_next_commit_can_use) {
  // The first commit of a new store allocates its segment. A process killed after the frames are
  // written but before the manifest names them leaves that segment behind with nothing referencing
  // it. It must never be appended to afterwards: the frames the manifest counts would then start at
  // the wrong offset, and the store would refuse itself for ever after.
  const ScratchStore scratch("re-crash-first-commit");
  // The store directory is created by the child itself, so the interruption happens inside the very
  // first commit the store ever performs.
  const ChildProcess child =
      start_child(quote(executable_path()) + " --child --store " + quote(scratch.path().string()) +
                      " --envelope first-envelope", "publish");
  RE_REQUIRE(child.started);
  RE_CHECK(child.wait());
  RE_CHECK_EQ(static_cast<long>(child.exit_code()), static_cast<long>(kAbortExitCode));

  {
    const Result<std::unique_ptr<EnvelopeService>> successor =
        EnvelopeService::open(scratch.path(), existing_options());
    RE_REQUIRE(successor.ok());
    const DeclareInput input =
        declaration_for("successor-envelope", 100ULL, "declare-1", 1767225600000000000LL);
    RE_REQUIRE(successor.value()->declare(input).ok());
    // The unpublished declaration is not authority: the store holds the successor and nothing else.
    const Result<std::vector<EnvelopeView>> listed = successor.value()->list_envelopes();
    RE_REQUIRE(listed.ok());
    RE_CHECK_EQ(listed.value().size(), std::size_t{1});
    RE_CHECK_EQ(listed.value().front().envelope.id, std::string("successor-envelope"));
    const Result<EnvelopeView> unpublished = successor.value()->get_envelope("first-envelope");
    RE_CHECK(!unpublished.ok());
  }
  // The store the successor produced is a store: it reopens, with the state it committed to.
  const Result<std::unique_ptr<EnvelopeService>> reopened =
      EnvelopeService::open(scratch.path(), existing_options());
  RE_REQUIRE(reopened.ok());
  const Result<std::vector<EnvelopeView>> listed = reopened.value()->list_envelopes();
  RE_REQUIRE(listed.ok());
  RE_CHECK_EQ(listed.value().size(), std::size_t{1});
  RE_CHECK_EQ(listed.value().front().envelope.id, std::string("successor-envelope"));
}

int main(int argc, char** argv) {
  // Child role: --child --store <path> --envelope <id> [--hold]
  if (argc > 1 && std::string(argv[1]) == "--child") {
    std::string store;
    std::string envelope_id;
    bool hold = false;
    for (int index = 2; index < argc; ++index) {
      const std::string flag = argv[index];
      if (flag == "--hold") {
        hold = true;
      } else if (flag == "--store" && index + 1 < argc) {
        store = argv[++index];
      } else if (flag == "--envelope" && index + 1 < argc) {
        envelope_id = argv[++index];
      }
    }
    if (store.empty() || envelope_id.empty()) return 2;
    return run_child_role(store, envelope_id, hold);
  }
  return testing::run_all();
}