#include "lsu/LSU.hpp"
#include "OlympiaSim.hpp"

#include "sparta/app/CommandLineSimulator.hpp"
#include "sparta/kernel/Scheduler.hpp"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace olympia
{
    class LSUTester
    {
      public:
        struct Snapshot
        {
            uint64_t cycle = 0;
            uint64_t issued = 0;
            uint64_t replayed = 0;
            uint64_t completed = 0;
            bool stopped = false;
            bool forwarding_seen = false;
            std::optional<uint64_t> appended_uid;
            std::map<uint64_t, LoadStoreInstInfo::IssueState> states;
            std::string text;
        };

        static Snapshot capture(const LSU & lsu)
        {
            Snapshot snapshot;
            snapshot.cycle = lsu.getClock()->currentCycle();
            snapshot.issued = lsu.lsu_insts_issued_.get();
            snapshot.replayed = lsu.replay_insts_.get();
            snapshot.completed = lsu.lsu_insts_completed_.get();
            snapshot.stopped = lsu.rob_stopped_simulation_;

            std::ostringstream out;
            out << "  IQ      " << formatQueue_(lsu.ldst_inst_queue_, true, &snapshot.states)
                << '\n';
            out << "  READY   " << formatQueue_(lsu.ready_queue_, true, nullptr) << '\n';
            out << "  PIPE    ";
            for (uint32_t stage = 0; stage < lsu.ldst_pipeline_.capacity(); ++stage)
            {
                if (stage != 0)
                {
                    out << " | ";
                }
                out << stageName_(lsu, stage) << '=';
                if (!lsu.ldst_pipeline_.isValid(stage))
                {
                    out << '-';
                    continue;
                }

                const auto & info = lsu.ldst_pipeline_[stage];
                out << shortInst_(info);
                const auto & inst = info->getInstPtr();
                const auto & mem = info->getMemoryAccessInfoPtr();
                if (!inst->isStoreInst() && mem->isCacheHit() && mem->isDataReady()
                    && lsu.tryStoreToLoadForwarding(inst))
                {
                    snapshot.forwarding_seen = true;
                    out << "(FWD)";
                }
            }
            if (lsu.ldst_pipeline_.isAppended())
            {
                snapshot.appended_uid = lsu.ldst_pipeline_.readAppendedData()->getInstUniqueID();
                out << " | NEXT=#" << *snapshot.appended_uid;
            }
            out << '\n';
            out << "  REPLAY  " << formatQueue_(lsu.replay_buffer_, false, nullptr) << '\n';
            out << "  STORE   " << formatQueue_(lsu.store_buffer_, false, nullptr) << '\n';
            out << "  COUNT   issued=" << snapshot.issued << " replay=" << snapshot.replayed
                << " completed=" << snapshot.completed;
            snapshot.text = out.str();
            return snapshot;
        }

        static bool speculative(const LSU & lsu) { return lsu.allow_speculative_load_exec_; }

      private:
        static const char* stateName_(const LoadStoreInstInfo::IssueState state)
        {
            switch (state)
            {
            case LoadStoreInstInfo::IssueState::READY:
                return "R";
            case LoadStoreInstInfo::IssueState::ISSUED:
                return "I";
            case LoadStoreInstInfo::IssueState::NOT_READY:
                return "N";
            case LoadStoreInstInfo::IssueState::NUM_STATES:
                return "?";
            }
            return "?";
        }

        static const char* priorityName_(const LoadStoreInstInfo::IssuePriority priority)
        {
            switch (priority)
            {
            case LoadStoreInstInfo::IssuePriority::HIGHEST:
                return "HIGH";
            case LoadStoreInstInfo::IssuePriority::CACHE_RELOAD:
                return "C-RELOAD";
            case LoadStoreInstInfo::IssuePriority::CACHE_PENDING:
                return "C-PEND";
            case LoadStoreInstInfo::IssuePriority::MMU_RELOAD:
                return "M-RELOAD";
            case LoadStoreInstInfo::IssuePriority::MMU_PENDING:
                return "M-PEND";
            case LoadStoreInstInfo::IssuePriority::NEW_DISP:
                return "NEW";
            case LoadStoreInstInfo::IssuePriority::LOWEST:
                return "LOW";
            case LoadStoreInstInfo::IssuePriority::NUM_OF_PRIORITIES:
                return "?";
            }
            return "?";
        }

        static std::string shortInst_(const LSU::LoadStoreInstInfoPtr & info,
                                      const bool full = false)
        {
            std::ostringstream out;
            out << '#' << info->getInstUniqueID() << ':' << info->getMnemonic();
            if (full)
            {
                out << ':' << stateName_(info->getState()) << '/'
                    << priorityName_(info->getPriority());
            }
            return out.str();
        }

        template <class QueueT>
        static std::string formatQueue_(const QueueT & queue, const bool full,
                                        std::map<uint64_t, LoadStoreInstInfo::IssueState>* states)
        {
            std::ostringstream out;
            out << '[';
            bool first = true;
            for (const auto & info : queue)
            {
                if (!first)
                {
                    out << ", ";
                }
                first = false;
                out << shortInst_(info, full);
                if (states != nullptr)
                {
                    states->emplace(info->getInstUniqueID(), info->getState());
                }
            }
            out << ']';
            return out.str();
        }

        static const char* stageName_(const LSU & lsu, const uint32_t stage)
        {
            if (stage == static_cast<uint32_t>(lsu.address_calculation_stage_))
            {
                return "AGEN";
            }
            if (stage == static_cast<uint32_t>(lsu.mmu_lookup_stage_))
            {
                return "MMU";
            }
            if (stage == static_cast<uint32_t>(lsu.cache_lookup_stage_))
            {
                return "DL1";
            }
            if (stage == static_cast<uint32_t>(lsu.cache_read_stage_))
            {
                return "READ";
            }
            if (stage == static_cast<uint32_t>(lsu.complete_stage_))
            {
                return "WB";
            }
            return "WAIT";
        }
    };
} // namespace olympia

namespace
{
    constexpr char USAGE[] =
        "Usage:\n"
        "    lsu_issue_queue_demo --input-file TRACE -c CONFIG [model options]\n";

    std::optional<uint64_t> findIssuedUID(const olympia::LSUTester::Snapshot & previous,
                                          const olympia::LSUTester::Snapshot & current)
    {
        if (current.issued == previous.issued)
        {
            return std::nullopt;
        }
        if (current.appended_uid.has_value())
        {
            return current.appended_uid;
        }

        for (const auto & [uid, state] : current.states)
        {
            const auto old = previous.states.find(uid);
            if (state == olympia::LoadStoreInstInfo::IssueState::ISSUED
                && (old == previous.states.end()
                    || old->second != olympia::LoadStoreInstInfo::IssueState::ISSUED))
            {
                return uid;
            }
        }
        return std::nullopt;
    }

    void printSnapshot(const olympia::LSUTester::Snapshot & snapshot,
                       const std::optional<uint64_t> issued_uid = std::nullopt,
                       const bool speculative_abort = false)
    {
        std::cout << "\nCYCLE " << snapshot.cycle;
        if (issued_uid.has_value())
        {
            std::cout << "  ISSUE -> #" << *issued_uid;
        }
        if (snapshot.forwarding_seen)
        {
            std::cout << "  STORE-TO-LOAD FORWARDING";
        }
        if (speculative_abort)
        {
            std::cout << "  SPECULATIVE ABORT -> #6 READY AGAIN";
        }
        std::cout << '\n' << snapshot.text << '\n';
    }

    size_t firstPosition(const std::vector<uint64_t> & history, const uint64_t uid)
    {
        const auto iter = std::find(history.begin(), history.end(), uid);
        return iter == history.end() ? std::numeric_limits<size_t>::max()
                                     : static_cast<size_t>(std::distance(history.begin(), iter));
    }

    bool printChecks(const bool speculative, const std::vector<uint64_t> & issue_history,
                     const bool forwarding_seen, const bool load_replay_seen)
    {
        const bool bypassed_blocked_load =
            firstPosition(issue_history, 3) < firstPosition(issue_history, 2)
            && firstPosition(issue_history, 4) < firstPosition(issue_history, 2);
        const bool replay_check = !speculative || load_replay_seen;

        std::cout << "\nCHECKS\n";
        std::cout << "  [" << (bypassed_blocked_load ? "PASS" : "FAIL")
                  << "] ready loads #3/#4 issued before blocked older load #2\n";
        std::cout << "  [" << (replay_check ? "PASS" : "FAIL") << "] "
                  << (speculative ? "load #6 was aborted and made ready to replay"
                                  : "speculative load replay is not required in conservative mode")
                  << '\n';
        std::cout << "  [" << (forwarding_seen ? "PASS" : "FAIL")
                  << "] same-address load #6 used store-to-load forwarding\n";

        std::cout << "  issue history:";
        for (const uint64_t uid : issue_history)
        {
            std::cout << " #" << uid;
        }
        std::cout << '\n';
        return bypassed_blocked_load && replay_check && forwarding_seen;
    }
} // namespace

int main(int argc, char** argv)
{
    sparta::app::DefaultValues defaults;
    defaults.auto_summary_default = "off";

    std::string input_file;
    uint64_t max_cycles = 200;
    sparta::app::CommandLineSimulator cls(USAGE, defaults);
    auto & options = cls.getApplicationOptions();
    options.add_options()("input-file",
                          sparta::app::named_value<std::string>("TRACE", &input_file)->required(),
                          "JSON instruction trace")(
        "max-cycles", sparta::app::named_value<uint64_t>("CYCLES", &max_cycles)->default_value(200),
        "Safety limit for cycle stepping");

    int error_code = 0;
    if (!cls.parse(argc, argv, error_code))
    {
        return error_code;
    }

    sparta::Scheduler scheduler;
    OlympiaSim sim(scheduler, 1, input_file, 7, false);
    cls.populateSimulation(&sim);

    auto* lsu = sim.getRoot()->getChild("cpu.core0.lsu")->getResourceAs<olympia::LSU*>();
    const bool speculative = olympia::LSUTester::speculative(*lsu);
    std::cout << "\nLSU ISSUE QUEUE DEMO\n"
              << "  mode=" << (speculative ? "speculative" : "conservative")
              << " queue-size=4 replay-size=8\n"
              << "  entry=#uid:mnemonic:state/priority; state R=ready I=issued N=not-ready\n";

    auto previous = olympia::LSUTester::capture(*lsu);
    printSnapshot(previous);

    std::vector<uint64_t> issue_history;
    bool forwarding_seen = false;
    bool load_replay_seen = false;
    bool finished = false;
    const auto cycle_ticks = lsu->getClock()->getPeriod();
    for (uint64_t cycle = 0; cycle < max_cycles; ++cycle)
    {
        scheduler.run(cycle_ticks, true, false);
        auto current = olympia::LSUTester::capture(*lsu);
        const auto issued_uid = findIssuedUID(previous, current);
        const auto previous_load = previous.states.find(6);
        const auto current_load = current.states.find(6);
        const bool load_replayed_this_cycle =
            previous_load != previous.states.end() && current_load != current.states.end()
            && previous_load->second == olympia::LoadStoreInstInfo::IssueState::ISSUED
            && current_load->second == olympia::LoadStoreInstInfo::IssueState::READY;
        if (issued_uid.has_value())
        {
            issue_history.emplace_back(*issued_uid);
        }
        forwarding_seen = forwarding_seen || current.forwarding_seen;
        load_replay_seen = load_replay_seen || load_replayed_this_cycle;

        if (current.text != previous.text || issued_uid.has_value() || load_replayed_this_cycle
            || current.stopped)
        {
            printSnapshot(current, issued_uid, load_replayed_this_cycle);
        }
        previous = std::move(current);

        if (previous.stopped)
        {
            finished = true;
            break;
        }
    }

    if (!finished)
    {
        std::cerr << "Demo exceeded the max cycle limit before the ROB stopped the simulation\n";
        return 2;
    }

    return printChecks(speculative, issue_history, forwarding_seen, load_replay_seen) ? 0 : 3;
}
