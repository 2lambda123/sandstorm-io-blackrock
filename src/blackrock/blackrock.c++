// Sandstorm Blackrock
// Copyright (c) 2014 Sandstorm Development Group, Inc.
// All Rights Reserved

#include <kj/main.h>
#include <kj/async-io.h>
#include <capnp/rpc.h>
#include <capnp/rpc-twoparty.h>
#include <capnp/serialize.h>
#include <sandstorm/version.h>
#include <sandstorm/util.h>
#include <blackrock/machine.capnp.h>
#include "cluster-rpc.h"
#include "worker.h"
#include "fs-storage.h"
#include "master.h"
#include "logs.h"
#include <netdb.h>
#include <sys/file.h>
#include <sys/sendfile.h>
#include <ifaddrs.h>

namespace blackrock {

class MachineImpl: public Machine::Server {
  // TODO(security): For most become*() methods, we should probably actually spawn a child process.
  //   (But before we do that we probably need to implement Cap'n Proto Level 3.)

public:
  MachineImpl(kj::AsyncIoContext& ioContext): ioContext(ioContext) {}
  ~MachineImpl() {
    KJ_LOG(WARNING, "master disconnected");
  }

  kj::Promise<void> becomeStorage(BecomeStorageContext context) override {
    mkdir("/var", 0777);
    mkdir("/var/blackrock", 0777);
    mkdir("/var/blackrock/storage", 0777);

    StorageRootSet::Client storage = kj::heap<FilesystemStorage>(
        sandstorm::raiiOpen("/var/blackrock/storage", O_RDONLY | O_DIRECTORY | O_CLOEXEC),
        ioContext.unixEventPort, ioContext.lowLevelProvider->getTimer(), nullptr);
    // TODO(someday): restorers, both incoming and outgoing
    auto results = context.getResults();
    results.setStorageFactory(storage.getFactoryRequest().send().getFactory());
    results.setRootSet(kj::mv(storage));

    return kj::READY_NOW;
  }

  kj::Promise<void> becomeWorker(BecomeWorkerContext context) override {
    context.getResults().setWorker(kj::heap<WorkerImpl>(ioContext));
    return kj::READY_NOW;
  }

private:
  kj::AsyncIoContext& ioContext;
};

class Main {
public:
  Main(kj::ProcessContext& context): context(context) {}

  kj::MainFunc getMain() {
    return kj::MainBuilder(context, "Sandstorm Blackrock version " SANDSTORM_VERSION,
                           "Starts Blackrock.")
        .addSubCommand("master", KJ_BIND_METHOD(*this, getMasterMain), "run as master node")
        .addSubCommand("slave", KJ_BIND_METHOD(*this, getSlaveMain), "run as slave node")
        .addSubCommand("grain", KJ_BIND_METHOD(*this, getSupervisorMain),
            "(internal) run a supervised grain")
        .build();
  }

  kj::MainFunc getMasterMain() {
    return kj::MainBuilder(context, "Sandstorm Blackrock version " SANDSTORM_VERSION,
                           "Starts Blackrock master.")
        .expectArg("<master-config>", KJ_BIND_METHOD(*this, runMaster))
        .build();
  }

  kj::MainFunc getSlaveMain() {
    return kj::MainBuilder(context, "Sandstorm Blackrock version " SANDSTORM_VERSION,
                           "Starts Blackrock slave.")
        .addOptionWithArg({'l', "log"}, KJ_BIND_METHOD(*this, setLogSink), "<addr>/<name>",
            "Redirect console logs to the log sink server at <addr>, self-identifying as <name>.")
        .expectArg("<bind-ip>", KJ_BIND_METHOD(*this, setBindIp))
        .callAfterParsing(KJ_BIND_METHOD(*this, runSlave))
        .build();
  }

  kj::MainFunc getSupervisorMain() {
    alternateMain = kj::heap<SupervisorMain>(context);
    return alternateMain->getMain();
  }

private:
  kj::ProcessContext& context;
  kj::Own<sandstorm::AbstractMain> alternateMain;
  SimpleAddress bindAddress = nullptr;

  struct LogSinkInfo {
    SimpleAddress address;
    kj::StringPtr name;
  };
  kj::Maybe<LogSinkInfo> logSinkInfo;

  kj::MainBuilder::Validity setLogSink(kj::StringPtr arg) {

    kj::StringPtr addrStr, name;
    kj::String scratch;

    KJ_IF_MAYBE(slashPos, arg.findFirst('/')) {
      scratch = kj::str(arg.slice(0, *slashPos));
      name = arg.slice(*slashPos + 1);
      addrStr = scratch;
    } else {
      addrStr = arg;
      name = nullptr;
    }

    auto address = SimpleAddress::lookup(addrStr);

    // Verify that we can connect at all.
    {
      int sock_;
      KJ_SYSCALL(sock_ = socket(address.family(), SOCK_STREAM, 0));
      kj::AutoCloseFd sock(sock_);
      KJ_SYSCALL(connect(sock, address.asSockaddr(), address.getSockaddrSize()));
    }

    logSinkInfo = LogSinkInfo { address, name };
    return true;
  }

  kj::MainBuilder::Validity setBindIp(kj::StringPtr bindIp) {
    if (bindIp.startsWith("if4:")) {
      bindAddress = SimpleAddress::getInterfaceAddress(AF_INET, bindIp.slice(strlen("if4:")));
      return true;
    } else if (bindIp.startsWith("if6:")) {
      bindAddress = SimpleAddress::getInterfaceAddress(AF_INET6, bindIp.slice(strlen("if6:")));
      return true;
    } else {
      bindAddress = SimpleAddress::lookup(bindIp);
      return true;
    }
  }

  bool runMaster(kj::StringPtr configFile) {
    capnp::StreamFdMessageReader configReader(
        sandstorm::raiiOpen(configFile, O_RDONLY | O_CLOEXEC));

    auto ioContext = kj::setupAsyncIo();
    sandstorm::SubprocessSet subprocessSet(ioContext.unixEventPort);
    VagrantDriver driver(subprocessSet, *ioContext.lowLevelProvider);
    blackrock::runMaster(ioContext, driver, configReader.getRoot<MasterConfig>());
    KJ_UNREACHABLE;
  }

  bool runSlave() {
    auto pidfile = sandstorm::raiiOpen("/var/run/blackrock-slave",
        O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    int l;
    KJ_NONBLOCKING_SYSCALL(l = flock(pidfile, LOCK_EX | LOCK_NB));

    if (l < 0) {
      // pidfile locked; slave already running
      dumpFile(pidfile, STDOUT_FILENO);
      context.exit();
    }

    // We're the only slave running. Go!
    KJ_SYSCALL(ftruncate(pidfile, 0));

    auto ioContext = kj::setupAsyncIo();
    VatNetwork network(ioContext.provider->getNetwork(), ioContext.provider->getTimer(),
                       bindAddress);

    // Write VatPath to pidfile.
    {
      capnp::MallocMessageBuilder vatPath(16);
      vatPath.setRoot(network.getSelf());
      capnp::writeMessageToFd(pidfile, vatPath);
    }

    sandstorm::Subprocess daemon([&]() -> int {
      // Detach from controlling terminal and make ourselves session leader.
      KJ_SYSCALL(setsid());

      // Write logs to log process.
      kj::Own<sandstorm::Subprocess> logProc;
      KJ_IF_MAYBE(l, logSinkInfo) {
        sendStderrToLogSink(l->name, l->address, "/var/log");
      }

      // Redirect stdout to stderr (i.e. the log sink).
      KJ_SYSCALL(dup2(STDERR_FILENO, STDOUT_FILENO));

      // Make standard input /dev/null.
      dup2(sandstorm::raiiOpen("/dev/null", O_RDONLY | O_CLOEXEC), STDIN_FILENO);

      KJ_DBG("in slave daemon...");

      // Set up RPC.
      // TODO(security): Only let the master bootstrap the MachineImpl.
      auto rpcSystem = capnp::makeRpcServer(network, kj::heap<MachineImpl>(ioContext));

      // Loop forever handling messages.
      kj::NEVER_DONE.wait(ioContext.waitScope);
      KJ_UNREACHABLE;
    });

    // The pidfile contains the VatPath. Write it to stdout, then exit;
    dumpFile(pidfile, STDOUT_FILENO);
    context.exit();
  }

  void dumpFile(int inFd, int outFd) {
    ssize_t n;
    off_t offset = 0;
    do {
      KJ_SYSCALL(n = sendfile(outFd, inFd, &offset, 4096));
    } while (n > 0);
  }
};

}  // namespace blackrock

KJ_MAIN(blackrock::Main)
