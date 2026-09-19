/* Copyright (c) 2026, Pedigree Developers. */
#include "pedigree/kernel/Atomic.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/process/PerProcessorScheduler.h"
#include "pedigree/kernel/process/Process.h"
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"

namespace {
class HostedAccountingProcess : public Process {
 public:
  explicit HostedAccountingProcess(Thread* driver, size_t interest = ~size_t(0))
      : Process(DeferredPublication(), driver->getParent()),
        m_Driver(driver),
        calls(0),
        user(0),
        profile(0),
        failures(0) {
    enableTimeAccountingReports(interest);
    description() += "hosted deferred accounting probe";
    publish();
  }

  ~HostedAccountingProcess() override {
    prepareForDestruction();
  }

  void setReportInterest(size_t interest, bool enabled) {
    setTimeAccountingReportInterest(interest, enabled);
  }

  Thread* m_Driver;
  Atomic<size_t> calls;
  Atomic<size_t> user;
  Atomic<size_t> profile;
  Atomic<size_t> failures;

 private:
  void reportTimesUpdated(Time::Timestamp userTotal, Time::Timestamp profileTotal) override {
    Thread* current = Processor::information().getCurrentThread();
    const size_t processCount = Scheduler::instance().getNumProcesses();
    const size_t threadCount = getNumThreads();
    (void)threadCount;
    if (!Processor::getInterrupts() || Processor::inDeviceHardIrq() || !current ||
        current == m_Driver || !processCount || userTotal < user || profileTotal < profile) {
      failures += 1;
    }
    user = static_cast<size_t>(userTotal);
    profile = static_cast<size_t>(profileTotal);
    calls += 1;
  }
};

struct AccountingThreadContext {
  AccountingThreadContext(Process* process, Time::Timestamp kernelBefore)
      : process(process), kernelBefore(kernelBefore), ran(0), firstSliceAccounted(0) {}

  Process* process;
  Time::Timestamp kernelBefore;
  Atomic<size_t> ran;
  Atomic<size_t> firstSliceAccounted;
};

int accountedKernelThread(void* parameter) {
  AccountingThreadContext* context = reinterpret_cast<AccountingThreadContext*>(parameter);
  Scheduler::instance().yield();
  context->firstSliceAccounted = context->process->getKernelTime() > context->kernelBefore;
  context->ran = 1;
  return 0;
}

}  // namespace

bool runHostedAccountingRegressions() {
  Thread* driver = Processor::information().getCurrentThread();
  const bool interruptsWereEnabled = Processor::getInterrupts();
  Processor::setInterrupts(false);
  const Time::Timestamp threadUser = driver->getUserTime();
  const Time::Timestamp threadKernel = driver->getKernelTime();
  const Time::Timestamp processUser = driver->getParent()->getUserTime();
  const Time::Timestamp processKernel = driver->getParent()->getKernelTime();
  driver->publishTimeAccountingForHostedTest(13, 7);
  driver->publishTimeAccountingForHostedTest(0, 0);
  const bool exactThreadPublication = !Processor::getInterrupts() &&
                                      driver->getUserTime() == threadUser + 13 &&
                                      driver->getKernelTime() == threadKernel + 7 &&
                                      driver->getParent()->getUserTime() == processUser + 13 &&
                                      driver->getParent()->getKernelTime() == processKernel + 7;
  Processor::setInterrupts(interruptsWereEnabled);

  const bool loadRequestPassed = Scheduler::instance().runHostedLoadAverageRequestRegression();
  HostedAccountingProcess* dormant = new HostedAccountingProcess(driver, 0);
  dormant->publishTimeAccountingForHostedTest(100, 200);
  bool interestPassed = !dormant->timeAccountingPendingForHostedTest() &&
                        !dormant->timeAccountingInterestForHostedTest() &&
                        dormant->getUserTime() == 100 && dormant->getKernelTime() == 200;
  dormant->setReportInterest(1, true);
  for (size_t attempt = 0; !dormant->calls && attempt < 10000; ++attempt) {
    PerProcessorScheduler::serviceCurrentIrqWorkDoorbellForTest();
    Scheduler::instance().yield();
  }
  interestPassed &=
      dormant->calls == 1 && dormant->user == 100 && dormant->profile == 300 && !dormant->failures;
  dormant->setReportInterest(2, true);
  dormant->setReportInterest(1, false);
  dormant->publishTimeAccountingForHostedTest(17, 11);
  interestPassed &= dormant->timeAccountingInterestForHostedTest() == 2;
  for (size_t attempt = 0; dormant->calls < 2 && attempt < 10000; ++attempt) {
    PerProcessorScheduler::serviceCurrentIrqWorkDoorbellForTest();
    Scheduler::instance().yield();
  }
  interestPassed &=
      dormant->calls == 2 && dormant->user == 117 && dormant->profile == 328 && !dormant->failures;
  dormant->setReportInterest(2, false);
  dormant->closeTimeAccountingForHostedTest();
  const size_t dormantCalls = dormant->calls;
  dormant->publishTimeAccountingForHostedTest(3, 5);
  interestPassed &= !dormant->timeAccountingInterestForHostedTest() &&
                    !dormant->timeAccountingPendingForHostedTest() &&
                    dormant->calls == dormantCalls && dormant->getUserTime() == 120 &&
                    dormant->getKernelTime() == 216;
  delete dormant;

  HostedAccountingProcess* process = new HostedAccountingProcess(driver);

  Processor::setInterrupts(false);
  process->publishTimeAccountingForHostedTest(13, 7);
  Processor::setInterrupts(interruptsWereEnabled);

  constexpr size_t Attempts = 10000;
  for (size_t attempt = 0; !process->calls && attempt < Attempts; ++attempt) {
    PerProcessorScheduler::serviceCurrentIrqWorkDoorbellForTest();
    Scheduler::instance().yield();
  }

  const bool exactWorkerBatch =
      process->calls == 1 && process->user == 13 && process->profile == 20 && !process->failures;
  process->publishTimeAccountingForHostedTest(0, 0);
  const bool zeroBatchDiscarded = !process->timeAccountingPendingForHostedTest() &&
                                  process->getUserTime() == 13 && process->getKernelTime() == 7;

  AccountingThreadContext threadContext(process, process->getKernelTime());
  Thread* accountedThread =
      new Thread(process, accountedKernelThread, &threadContext, nullptr, false, true, true);
  accountedThread->setName("hosted accounting first-slice probe");
  const bool accountedThreadStarted = accountedThread->start();
  const bool accountedThreadJoined = accountedThreadStarted && accountedThread->joinForCompletion();
  if (!accountedThreadStarted) {
    delete accountedThread;
  }

  for (size_t attempt = 0; process->profile == 20 && attempt < Attempts; ++attempt) {
    PerProcessorScheduler::serviceCurrentIrqWorkDoorbellForTest();
    Scheduler::instance().yield();
  }
  const bool firstKernelSliceAccounted = accountedThreadStarted && accountedThreadJoined &&
                                         threadContext.ran && threadContext.firstSliceAccounted &&
                                         process->user == 13 && process->profile > 20 &&
                                         !process->failures;
  if (!firstKernelSliceAccounted) {
    ERROR("HOSTED-ACCOUNTING-FIRST: started="
          << accountedThreadStarted << " joined=" << accountedThreadJoined
          << " ran=" << static_cast<size_t>(threadContext.ran)
          << " slice=" << static_cast<size_t>(threadContext.firstSliceAccounted));
    ERROR("HOSTED-ACCOUNTING-FIRST: user=" << static_cast<size_t>(process->user) << " profile="
                                           << static_cast<size_t>(process->profile) << " failures="
                                           << static_cast<size_t>(process->failures));
  }

  process->closeTimeAccountingForHostedTest();
  const size_t callsBeforeLatePublication = process->calls;
  const Time::Timestamp userBeforeLatePublication = process->getUserTime();
  const Time::Timestamp kernelBeforeLatePublication = process->getKernelTime();
  process->publishTimeAccountingForHostedTest(101, 211);
  for (size_t attempt = 0; attempt < 32; ++attempt) {
    Scheduler::instance().yield();
  }
  const bool latePublicationDiscarded =
      process->calls == callsBeforeLatePublication &&
      !process->timeAccountingPendingForHostedTest() &&
      process->timeAccountingInterestForHostedTest() &&
      process->getUserTime() == userBeforeLatePublication + 101 &&
      process->getKernelTime() == kernelBeforeLatePublication + 211;
  delete process;

  const bool passed = exactThreadPublication && loadRequestPassed && interestPassed &&
                      exactWorkerBatch && zeroBatchDiscarded && firstKernelSliceAccounted &&
                      latePublicationDiscarded;
  if (!passed) {
    ERROR("HOSTED-WAIT-TEST: FAIL deferred-time-accounting-worker: exact="
          << exactThreadPublication << " load=" << loadRequestPassed << " interest="
          << interestPassed << " batch=" << exactWorkerBatch << " zero=" << zeroBatchDiscarded
          << " first=" << firstKernelSliceAccounted << " late=" << latePublicationDiscarded);
  } else {
    NOTICE("HOSTED-WAIT-TEST: PASS deferred-time-accounting-worker");
  }
  return passed;
}
