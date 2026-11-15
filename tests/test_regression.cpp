// Regression tests for audited correctness defects.

#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <system_error>
#include <variant>
#include <vector>

#include "ensemble_fabric/client.hpp"
#include "ensemble_fabric/coordinator.hpp"
#include "ensemble_fabric/protocol.hpp"
#include "ensemble_fabric/transport.hpp"
#include "ensemble_fabric/version.hpp"
#include "support/test_harness.hpp"
#include "support/test_support.hpp"

using namespace ensemble_fabric;

namespace {

/// Two candidates and one judge that accepts both: the votes tie, which is the
/// judge-disagreement fallback trigger.
EnsembleSpec judged_fallback_spec() {
  EnsembleSpec spec = parallel_spec(1, {ParticipantId(1), ParticipantId(2)}, {ParticipantId(10)});
  spec.participants.push_back(fallback_participant(ParticipantId(5)));
  spec.fallback.enabled = true;
  spec.fallback.triggers.push_back(FallbackTrigger::judge_disagreement);
  spec.fallback.fallback_participants.push_back(ParticipantId(5));
  spec.fallback.max_depth = 1u;
  return spec;
}

void attach_all(EnsembleHarness& harness, const EnsembleSpec& spec) {
  std::uint64_t serial = 7001u;
  for (const ParticipantSpec& participant : spec.participants) {
    const std::string script = participant.roles.contains(ParticipantRole::judge)
                                   ? "evaluate:judgment=accept,score=0.5"
                                   : "ok:value=content-" + participant.id.to_string();
    harness.attach(EnsembleId(1), EnsembleGeneration(1), participant,
                   reference_program(script).value(), serial++);
  }
}

}  // namespace

EF_TEST(a_fallback_activated_after_judging_still_settles) {
  EnsembleFabric fabric;
  EnsembleHarness harness(fabric);
  const EnsembleSpec spec = judged_fallback_spec();
  EF_REQUIRE_OK(harness.define(spec));
  EF_REQUIRE_OK(harness.open(EnsembleId(1), EnsembleGeneration(1)));
  attach_all(harness, spec);
  EF_REQUIRE_OK(harness.register_all());
  EF_REQUIRE_OK(harness.run());
  Outcome<EnsembleSnapshot> snapshot = fabric.inspect(EnsembleId(1));
  EF_REQUIRE_OK(snapshot);
  EF_CHECK(snapshot.value().fallback_activated);
  Outcome<bool> settled = fabric.is_settled(EnsembleId(1), EnsembleGeneration(1));
  EF_REQUIRE_OK(settled);
  EF_CHECK(settled.value());
}

EF_TEST(every_mandatory_criterion_needs_its_own_evidence) {
  EnsembleFabric fabric;
  EnsembleHarness harness(fabric);
  EnsembleSpec spec = parallel_spec(1, {ParticipantId(1)});
  spec.participants.push_back(verifier_participant(ParticipantId(20), false));
  spec.participants.push_back(verifier_participant(ParticipantId(21), false));
  const CriterionRef first_criterion{1u, 1u};
  const CriterionRef second_criterion{2u, 1u};
  spec.evidence.mandatory_criteria = {first_criterion, second_criterion};
  EF_REQUIRE_OK(harness.define(spec));
  EF_REQUIRE_OK(harness.open(EnsembleId(1), EnsembleGeneration(1)));
  harness.attach(EnsembleId(1), EnsembleGeneration(1), spec.participants[0],
                 reference_program("ok:value=answer").value(), 9101u);
  harness.attach(EnsembleId(1), EnsembleGeneration(1), spec.participants[1],
                 reference_program("evaluate:verification=pass,judgment=accept").value(), 9102u);
  harness.attach(EnsembleId(1), EnsembleGeneration(1), spec.participants[2],
                 reference_program("evaluate:verification=pass,judgment=accept").value(), 9103u);
  EF_REQUIRE_OK(harness.register_all());
  EF_REQUIRE_OK(harness.step());  // the candidate is dispatched and produces content

  // Both verifiers are asked for both criteria; each answers only the first.
  Outcome<std::vector<FabricAction>> actions =
      fabric.advance(EnsembleId(1), EnsembleGeneration(1));
  EF_REQUIRE_OK(actions);
  std::uint32_t answered = 0;
  for (const FabricAction& action : actions.value()) {
    const auto* evaluation = std::get_if<EvaluationAction>(&action);
    if (evaluation == nullptr || !(evaluation->criterion == first_criterion)) {
      continue;
    }
    HarnessParticipant* verifier = harness.find(evaluation->judge);
    EF_REQUIRE(verifier != nullptr);
    EvaluationSubmission submission;
    submission.authority.evaluator = harness.authority_of(*verifier);
    submission.authority.execution = evaluation->execution.execution;
    submission.authority.attempt_generation = evaluation->execution.attempt_generation;
    submission.authority.evaluation = evaluation->evaluation;
    submission.authority.evaluation_generation = evaluation->evaluation_generation;
    submission.authority.candidate = evaluation->candidate;
    submission.authority.candidate_generation = evaluation->candidate_generation;
    submission.criterion = evaluation->criterion;
    submission.kind = evaluation->criterion_kind;
    submission.verification = VerificationState::pass;
    submission.judgment = CategoricalJudgment::accept;
    submission.rationale = "first criterion holds";
    EF_REQUIRE_OK(fabric.submit_evaluation(submission));
    answered += 1u;
  }
  EF_REQUIRE(answered == 2u);
  // Neither verifier ever answers the second criterion.
  for (const ParticipantId id : {ParticipantId(20), ParticipantId(21)}) {
    HarnessParticipant* verifier = harness.find(id);
    EF_REQUIRE(verifier != nullptr);
    EF_REQUIRE_OK(fabric.report_participant_failure(harness.authority_of(*verifier),
                                                    ParticipantFailureKind::unavailable, "gone"));
  }
  EF_REQUIRE_OK(harness.run());
  Outcome<EnsembleResult> result = fabric.finalize(EnsembleId(1), EnsembleGeneration(1));
  EF_CHECK(!result.ok() || !result.value().has_selected);
}

EF_TEST(the_commit_gate_refuses_a_payload_the_selected_candidate_never_produced) {
  EnsembleFabric fabric;
  EnsembleHarness harness(fabric);
  const EnsembleSpec spec = parallel_spec(1, {ParticipantId(1), ParticipantId(2)});
  EF_REQUIRE_OK(harness.define(spec));
  EF_REQUIRE_OK(harness.open(EnsembleId(1), EnsembleGeneration(1)));
  attach_all(harness, spec);
  EF_REQUIRE_OK(harness.register_all());
  EF_REQUIRE_OK(harness.run());
  Outcome<EnsembleResult> prepared = fabric.prepare_result(EnsembleId(1), EnsembleGeneration(1));
  EF_REQUIRE_OK(prepared);
  EnsembleResult forged = prepared.value();
  const std::string text = "forged";
  forged.payload.assign(reinterpret_cast<const std::byte*>(text.data()),
                        reinterpret_cast<const std::byte*>(text.data()) + text.size());
  EF_CHECK(!fabric.commit_result(forged).ok());
  EF_CHECK(!fabric.current_result(EnsembleId(1)).ok());
}

EF_TEST(the_commit_gate_refuses_a_result_prepared_before_its_authority_changed) {
  EnsembleFabric fabric;
  EnsembleHarness harness(fabric);
  const EnsembleSpec spec = parallel_spec(1, {ParticipantId(1), ParticipantId(2)});
  EF_REQUIRE_OK(harness.define(spec));
  EF_REQUIRE_OK(harness.open(EnsembleId(1), EnsembleGeneration(1)));
  attach_all(harness, spec);
  EF_REQUIRE_OK(harness.register_all());
  EF_REQUIRE_OK(harness.run());
  Outcome<EnsembleResult> prepared = fabric.prepare_result(EnsembleId(1), EnsembleGeneration(1));
  EF_REQUIRE_OK(prepared);
  EF_REQUIRE(prepared.value().has_selected);
  // The selected candidate's participant loses its authority after the result
  // was prepared: the prepared result is no longer one the evidence supports.
  HarnessParticipant* selected = harness.find(prepared.value().selected_participant);
  EF_REQUIRE(selected != nullptr);
  EF_REQUIRE_OK(fabric.report_participant_failure(harness.authority_of(*selected),
                                                  ParticipantFailureKind::internal_error, "lost"));
  EF_CHECK(!fabric.commit_result(prepared.value()).ok());
}

namespace {

/// A loopback peer speaking the wire protocol directly, so that a test decides
/// exactly what a worker sends and observes exactly what it receives.
class RawPeer {
 public:
  RawPeer(std::uint16_t port, WorkerId worker, WorkerBootId boot) : worker_(worker), boot_(boot) {
    Endpoint endpoint;
    endpoint.host = "127.0.0.1";
    endpoint.port = port;
    Outcome<Socket> socket = connect_to(endpoint);
    if (!socket.ok()) {
      return;
    }
    channel_ = std::make_unique<FrameChannel>(std::move(socket).value());
    HelloMessage hello;
    hello.worker = worker_;
    hello.worker_boot = boot_;
    hello.coordinator_epoch = CoordinatorEpoch(0);
    hello.profile.protocol_version = protocol_version;
    hello.label = "raw-peer";
    if (!send(MessageType::hello, hello)) {
      return;
    }
    Outcome<Frame> reply = channel_->receive();
    if (!reply.ok() || reply.value().type != MessageType::hello_ack) {
      return;
    }
    Decoder decoder(std::span<const std::byte>(reply.value().payload.data(),
                                               reply.value().payload.size()));
    Outcome<CoordinatorAck> ack = decode_coordinator_ack(decoder);
    if (ack.ok() && ack.value().accepted) {
      epoch_ = ack.value().coordinator_epoch;
      ready_ = true;
    }
  }

  [[nodiscard]] bool ready() const noexcept { return ready_; }
  [[nodiscard]] WorkerId worker() const noexcept { return worker_; }
  [[nodiscard]] WorkerBootId boot() const noexcept { return boot_; }

  template <class Message>
  [[nodiscard]] bool send(MessageType type, const Message& message) {
    Encoder encoded;
    encode(message, encoded);
    Frame frame;
    frame.type = type;
    frame.correlation = RequestId(++sequence_);
    frame.epoch = epoch_;
    frame.worker_boot = boot_;
    frame.sequence = Sequence(sequence_);
    frame.payload = encoded.take();
    return channel_->send(frame).ok();
  }

  [[nodiscard]] bool register_for(EnsembleId ensemble, ParticipantId participant) {
    RegisterParticipantMessage message;
    message.registration.ensemble = ensemble;
    message.registration.ensemble_generation = EnsembleGeneration(1);
    message.registration.participant = participant;
    message.registration.worker = worker_;
    message.registration.worker_boot = boot_;
    message.registration.coordinator_epoch = epoch_;
    message.registration.profile.protocol_version = protocol_version;
    message.registration.claimed_roles.insert(ParticipantRole::candidate);
    if (!send(MessageType::register_participant, message)) {
      return false;
    }
    Outcome<Frame> reply = channel_->receive();
    if (!reply.ok() || reply.value().type != MessageType::register_ack) {
      return false;
    }
    Decoder decoder(std::span<const std::byte>(reply.value().payload.data(),
                                               reply.value().payload.size()));
    Outcome<RegistrationAck> ack = decode_registration_ack(decoder);
    return ack.ok() && ack.value().accepted;
  }

  /// Receives frames up to the reply of an inspection request sent now, so the
  /// caller sees everything the coordinator sent this peer before it.
  [[nodiscard]] std::vector<Frame> drain(EnsembleId ensemble) {
    std::vector<Frame> frames;
    InspectRequestMessage probe;
    probe.ensemble = ensemble;
    if (!send(MessageType::inspect_request, probe)) {
      return frames;
    }
    for (;;) {
      Outcome<Frame> frame = channel_->receive();
      if (!frame.ok() || frame.value().type == MessageType::inspect_response) {
        return frames;
      }
      frames.push_back(std::move(frame).value());
    }
  }

  [[nodiscard]] Outcome<DispatchCandidateMessage> next_dispatch() {
    for (;;) {
      Outcome<Frame> frame = channel_->receive();
      if (!frame.ok()) {
        return fail<DispatchCandidateMessage>(frame.code(), frame.error().message);
      }
      if (frame.value().type == MessageType::dispatch_candidate) {
        Decoder decoder(std::span<const std::byte>(frame.value().payload.data(),
                                                   frame.value().payload.size()));
        return decode_dispatch_candidate(decoder);
      }
    }
  }

  void close() {
    if (channel_ != nullptr) {
      channel_->close();
    }
  }

 private:
  WorkerId worker_;
  WorkerBootId boot_;
  CoordinatorEpoch epoch_;
  std::unique_ptr<FrameChannel> channel_;
  std::uint64_t sequence_{0};
  bool ready_{false};
};

[[nodiscard]] CoordinatorConfig loopback_config(std::string state_path = std::string()) {
  CoordinatorConfig config;
  config.bind.host = "127.0.0.1";
  config.bind.port = 0;
  config.state_path = std::move(state_path);
  config.persist_on_change = !config.state_path.empty();
  return config;
}

[[nodiscard]] Endpoint coordinator_endpoint(const EnsembleCoordinator& coordinator) {
  Endpoint endpoint;
  endpoint.host = "127.0.0.1";
  endpoint.port = coordinator.port();
  return endpoint;
}

[[nodiscard]] std::filesystem::path scratch_state(const char* name) {
  std::error_code error;
  const std::filesystem::path directory =
      std::filesystem::temp_directory_path(error) / "ensemble_fabric_regression";
  std::filesystem::create_directories(directory, error);
  const std::filesystem::path path = directory / name;
  std::filesystem::remove_all(path, error);
  return path;
}

[[nodiscard]] CandidateResultMessage result_for(const DispatchCandidateMessage& dispatch,
                                                WorkerId worker, WorkerBootId boot,
                                                const std::string& text) {
  CandidateResultMessage message;
  message.authority.participant.ensemble = dispatch.ensemble;
  message.authority.participant.ensemble_generation = dispatch.ensemble_generation;
  message.authority.participant.participant = dispatch.participant;
  message.authority.participant.participant_generation = dispatch.participant_generation;
  message.authority.participant.worker = worker;
  message.authority.participant.worker_boot = boot;
  message.authority.participant.coordinator_epoch = dispatch.coordinator_epoch;
  message.authority.execution = dispatch.execution;
  message.authority.attempt_generation = dispatch.attempt_generation;
  message.authority.candidate = dispatch.candidate;
  message.authority.candidate_generation = dispatch.candidate_generation;
  message.payload.assign(reinterpret_cast<const std::byte*>(text.data()),
                         reinterpret_cast<const std::byte*>(text.data()) + text.size());
  return message;
}

[[nodiscard]] std::vector<std::uint64_t> dispatched_ensembles(const std::vector<Frame>& frames) {
  std::vector<std::uint64_t> ensembles;
  for (const Frame& frame : frames) {
    if (frame.type != MessageType::dispatch_candidate) {
      continue;
    }
    Decoder decoder(std::span<const std::byte>(frame.payload.data(), frame.payload.size()));
    Outcome<DispatchCandidateMessage> dispatch = decode_dispatch_candidate(decoder);
    if (dispatch.ok()) {
      ensembles.push_back(dispatch.value().ensemble.value());
    }
  }
  return ensembles;
}

}  // namespace

EF_TEST(a_connection_cannot_submit_under_another_workers_incarnation) {
  EnsembleCoordinator coordinator(loopback_config());
  EF_REQUIRE_OK(coordinator.start());
  Outcome<EnsembleClient> client = EnsembleClient::connect(coordinator_endpoint(coordinator));
  EF_REQUIRE_OK(client);
  Outcome<DefineEnsembleAck> defined =
      client.value().define_ensemble(parallel_spec(1, {ParticipantId(1)}));
  EF_REQUIRE_OK(defined);
  EF_REQUIRE(defined.value().accepted);
  RawPeer owner(coordinator.port(), WorkerId(501), WorkerBootId(9001));
  EF_REQUIRE(owner.ready());
  EF_REQUIRE(owner.register_for(EnsembleId(1), ParticipantId(1)));
  EF_REQUIRE_OK(client.value().open_execution(EnsembleId(1), EnsembleGeneration(1)));
  Outcome<DispatchCandidateMessage> dispatch = owner.next_dispatch();
  EF_REQUIRE_OK(dispatch);

  // Another connection names the owner's worker and boot.
  RawPeer impostor(coordinator.port(), WorkerId(777), WorkerBootId(4242));
  EF_REQUIRE(impostor.ready());
  EF_REQUIRE(impostor.send(MessageType::candidate_result,
                           result_for(dispatch.value(), owner.worker(), owner.boot(), "forged")));
  (void)impostor.drain(EnsembleId(1));

  Outcome<EnsembleSnapshot> snapshot = coordinator.fabric().inspect(EnsembleId(1));
  EF_REQUIRE_OK(snapshot);
  EF_REQUIRE(snapshot.value().candidates.size() == 1u);
  EF_CHECK(snapshot.value().candidates.front().state == CandidateState::dispatched);
  impostor.close();
  owner.close();
  client.value().close();
  EF_REQUIRE_OK(coordinator.stop());
}

EF_TEST(a_dispatch_reaches_the_worker_registered_for_its_ensemble) {
  EnsembleCoordinator coordinator(loopback_config());
  EF_REQUIRE_OK(coordinator.start());
  Outcome<EnsembleClient> client = EnsembleClient::connect(coordinator_endpoint(coordinator));
  EF_REQUIRE_OK(client);
  // Participant identities are scoped to their ensemble: both ensembles use 1.
  EF_REQUIRE_OK(client.value().define_ensemble(parallel_spec(1, {ParticipantId(1)})));
  EF_REQUIRE_OK(client.value().define_ensemble(parallel_spec(2, {ParticipantId(1)})));
  RawPeer second(coordinator.port(), WorkerId(602), WorkerBootId(6002));
  RawPeer first(coordinator.port(), WorkerId(601), WorkerBootId(6001));
  EF_REQUIRE(second.ready() && first.ready());
  EF_REQUIRE(second.register_for(EnsembleId(2), ParticipantId(1)));
  EF_REQUIRE(first.register_for(EnsembleId(1), ParticipantId(1)));
  EF_REQUIRE_OK(client.value().open_execution(EnsembleId(1), EnsembleGeneration(1)));
  EF_REQUIRE_OK(client.value().open_execution(EnsembleId(2), EnsembleGeneration(1)));

  const std::vector<std::uint64_t> to_first = dispatched_ensembles(first.drain(EnsembleId(1)));
  const std::vector<std::uint64_t> to_second = dispatched_ensembles(second.drain(EnsembleId(2)));
  EF_CHECK(to_first == std::vector<std::uint64_t>{1u});
  EF_CHECK(to_second == std::vector<std::uint64_t>{2u});
  first.close();
  second.close();
  client.value().close();
  EF_REQUIRE_OK(coordinator.stop());
}

EF_TEST(a_restart_before_any_change_still_advances_the_epoch) {
  const std::filesystem::path path = scratch_state("epoch.state");
  CoordinatorEpoch served;
  {
    EnsembleCoordinator coordinator(loopback_config(path.string()));
    EF_REQUIRE_OK(coordinator.start());
    served = coordinator.epoch();
    // The process ends here without a single state change.
    EF_REQUIRE_OK(coordinator.stop());
  }
  EnsembleCoordinator restarted(loopback_config(path.string()));
  EF_REQUIRE_OK(restarted.start());
  EF_CHECK(restarted.epoch().value() > served.value());
  EF_REQUIRE_OK(restarted.stop());
  std::error_code error;
  std::filesystem::remove_all(path, error);
}

EF_TEST(a_commit_is_acknowledged_only_once_it_is_durable) {
  const std::filesystem::path path = scratch_state("commit.state");
  EnsembleCoordinator coordinator(loopback_config(path.string()));
  EF_REQUIRE_OK(coordinator.start());
  Outcome<EnsembleClient> client = EnsembleClient::connect(coordinator_endpoint(coordinator));
  EF_REQUIRE_OK(client);
  EF_REQUIRE_OK(client.value().define_ensemble(parallel_spec(1, {ParticipantId(1)})));
  RawPeer worker(coordinator.port(), WorkerId(801), WorkerBootId(8001));
  EF_REQUIRE(worker.ready());
  EF_REQUIRE(worker.register_for(EnsembleId(1), ParticipantId(1)));
  EF_REQUIRE_OK(client.value().open_execution(EnsembleId(1), EnsembleGeneration(1)));
  Outcome<DispatchCandidateMessage> dispatch = worker.next_dispatch();
  EF_REQUIRE_OK(dispatch);
  EF_REQUIRE(worker.send(MessageType::candidate_result,
                         result_for(dispatch.value(), worker.worker(), worker.boot(), "answer")));
  (void)worker.drain(EnsembleId(1));

  // The state file can no longer be replaced: a directory now holds its name.
  std::error_code error;
  std::filesystem::remove_all(path, error);
  std::filesystem::create_directories(path, error);
  EF_REQUIRE(!error);
  Outcome<FinalizeResponseMessage> finalized =
      client.value().finalize(EnsembleId(1), EnsembleGeneration(1));
  EF_REQUIRE_OK(finalized);
  EF_CHECK(!finalized.value().accepted);
  worker.close();
  client.value().close();
  EF_REQUIRE_OK(coordinator.stop());
  std::filesystem::remove_all(path, error);
}

EF_TEST_MAIN
