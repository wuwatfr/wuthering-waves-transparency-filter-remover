// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors

#include "trace_submission_identity.hpp"

#include "test_check.hpp"
#include <array>
#include <iostream>
#include <string>
#include <unordered_map>

namespace {

struct Identity {
  std::uint64_t fingerprint = 0;
  friend bool operator==(const Identity&, const Identity&) = default;
};

wuwa_tfr::TraceGeometryKey IndexedGeometry(
    std::uint64_t vertex_resource,
    std::uint64_t index_resource) {
  wuwa_tfr::TraceGeometryKey key;
  key.kind = wuwa_tfr::TraceDrawKind::Indexed;
  key.arguments = {42, 1, 3, static_cast<std::uint64_t>(-2), 0};
  key.topology = 4;
  key.vertex_buffers.push_back({0, vertex_resource, 16, 32});
  key.index_buffer = wuwa_tfr::TraceIndexBinding{index_resource, 8, 4};
  key.observations = wuwa_tfr::TraceObservedPso |
      wuwa_tfr::TraceObservedVertexBuffers |
      wuwa_tfr::TraceObservedIndexBuffer |
      wuwa_tfr::TraceObservedTopology |
      wuwa_tfr::TraceObservedPass;
  return key;
}

void CheckSameSubmissionWindows(
    const std::array<wuwa_tfr::TraceSubmissionWindowMetrics, 3>& actual,
    const std::array<wuwa_tfr::TraceSubmissionWindowMetrics, 3>& expected) {
  for (std::size_t index = 0; index != actual.size(); ++index) {
    CHECK(actual[index].queue_submitted_commands ==
        expected[index].queue_submitted_commands);
    CHECK(actual[index].command_list_submissions ==
        expected[index].command_list_submissions);
    CHECK(actual[index].active_frames == expected[index].active_frames);
    CHECK(actual[index].last_frame == expected[index].last_frame);
  }
}

} // namespace

int main() {
  using wuwa_tfr::TraceIncarnationIndex;
  using wuwa_tfr::TraceLiveHandleKey;

  TraceIncarnationIndex<Identity> psos;
  const TraceLiveHandleKey d1h7{1, 7};
  const auto first = psos.Activate(d1h7, {100});
  CHECK(first.id != 0 && !first.duplicate);
  const auto duplicate = psos.Activate(d1h7, {100});
  CHECK(duplicate.id == first.id && duplicate.duplicate);
  const auto rotated = psos.Activate(d1h7, {200});
  CHECK(rotated.id != first.id && rotated.rotated_without_destroy);
  CHECK(rotated.previous_identity &&
      rotated.previous_identity->fingerprint == 100);
  CHECK(psos.Find(first.id) == nullptr);

  CHECK(psos.Destroy(d1h7) == rotated.id);
  CHECK(psos.Find(rotated.id) == nullptr);
  const auto reused = psos.Activate(d1h7, {200});
  CHECK(reused.id != rotated.id);
  const auto other_device = psos.Activate({2, 7}, {200});
  CHECK(other_device.id != reused.id);

  const auto geometry = IndexedGeometry(11, 12);
  CHECK(wuwa_tfr::TraceGeometryIsConcrete(geometry));
  auto changed_argument = geometry;
  changed_argument.arguments[0]++;
  CHECK(changed_argument != geometry);
  auto changed_resource = geometry;
  changed_resource.index_buffer->resource_incarnation++;
  CHECK(changed_resource != geometry);
  auto dynamic_geometry = geometry;
  dynamic_geometry.vertex_buffers[0].dynamic_contents = true;
  CHECK(dynamic_geometry != geometry);
  CHECK(wuwa_tfr::TraceGeometryUsesDynamicContents(dynamic_geometry));
  CHECK(!wuwa_tfr::TraceGeometryUsesDynamicContents(geometry));

  wuwa_tfr::TraceConcreteDrawKey pso_a{reused.id, geometry, 77};
  wuwa_tfr::TraceConcreteDrawKey pso_b{other_device.id, geometry, 77};
  CHECK(pso_a != pso_b);
  CHECK(pso_a.geometry == pso_b.geometry);
  CHECK(wuwa_tfr::MakeTraceDrawRoute(pso_a) ==
      wuwa_tfr::MakeTraceDrawRoute(pso_b));
  auto other_pass = pso_b;
  other_pass.pass_fingerprint++;
  CHECK(wuwa_tfr::MakeTraceDrawRoute(pso_a) !=
      wuwa_tfr::MakeTraceDrawRoute(other_pass));

  using wuwa_tfr::TraceRouteConclusion;
  CHECK(wuwa_tfr::ClassifyTraceRoute(
             true, true, true, false, false) ==
      TraceRouteConclusion::SubmittedInFullWindow);
  CHECK(wuwa_tfr::ClassifyTraceRoute(
             true, false, true, false, false) ==
      TraceRouteConclusion::NotObservedInFullWindow);
  CHECK(wuwa_tfr::ClassifyTraceRoute(
             false, true, true, false, false) ==
      TraceRouteConclusion::Unknown);
  CHECK(wuwa_tfr::ClassifyTraceRoute(
             true, true, false, false, false) ==
      TraceRouteConclusion::Unknown);
  CHECK(wuwa_tfr::ClassifyTraceRoute(
             true, true, true, true, false) ==
      TraceRouteConclusion::Unknown);
  CHECK(wuwa_tfr::ClassifyTraceRoute(
             true, true, true, false, true) ==
      TraceRouteConclusion::Unknown);

  // Exact pass routes are classified independently. Multiple observed passes
  // for the same geometry and PSO changes do not make either route ambiguous.
  CHECK(wuwa_tfr::ClassifyTraceRoute(
             true, true, true, false, false) ==
      TraceRouteConclusion::SubmittedInFullWindow);
  CHECK(wuwa_tfr::ClassifyTraceRoute(
             true, false, true, false, false) ==
      TraceRouteConclusion::NotObservedInFullWindow);
  auto packed_geometry = geometry;
  packed_geometry.vertex_buffers[0].offset += 4096;
  CHECK(packed_geometry != geometry);
  CHECK(wuwa_tfr::ClassifyTraceRoute(
             true, true, true, false, false) ==
      TraceRouteConclusion::SubmittedInFullWindow);

  std::unordered_map<wuwa_tfr::TraceConcreteDrawKey, int,
      wuwa_tfr::TraceConcreteDrawKeyHash> collision_safe;
  collision_safe[pso_a] = 1;
  collision_safe[pso_b] = 2;
  CHECK(collision_safe.size() == 2);

  auto indirect = geometry;
  indirect.kind = wuwa_tfr::TraceDrawKind::IndirectIndexed;
  indirect.observations |= wuwa_tfr::TraceIndirectArgumentsUnknown;
  CHECK(!wuwa_tfr::TraceGeometryIsConcrete(indirect));
  auto missing_pass = geometry;
  missing_pass.observations &= ~wuwa_tfr::TraceObservedPass;
  CHECK(!wuwa_tfr::TraceGeometryIsConcrete(missing_pass));
  auto unknown_vertex_resource = geometry;
  unknown_vertex_resource.vertex_buffers[0].resource_incarnation = 0;
  CHECK(!wuwa_tfr::TraceGeometryIsConcrete(unknown_vertex_resource));
  wuwa_tfr::TraceConcreteDrawKey secondary{reused.id, missing_pass, 0};
  const auto inherited = wuwa_tfr::WithInheritedTracePass(secondary, 99, true);
  CHECK(inherited.pass_fingerprint == 99);
  CHECK((inherited.geometry.observations &
      wuwa_tfr::TraceObservedPass) != 0);
  CHECK(wuwa_tfr::TraceGeometryIsConcrete(inherited.geometry));
  CHECK(!wuwa_tfr::TraceGeometryIsSkipEligible(inherited.geometry));
  CHECK(wuwa_tfr::TraceGeometryIsSkipEligible(pso_a.geometry));
  const auto unchanged = wuwa_tfr::WithInheritedTracePass(pso_a, 99, true);
  CHECK(unchanged == pso_a);

  wuwa_tfr::TraceSubmissionRecord<3> submitted;
  wuwa_tfr::AccumulateTraceSubmission(
      submitted, std::size_t{0}, 4, true, 10);
  wuwa_tfr::AccumulateTraceSubmission(
      submitted, std::size_t{0}, 2, false, 10);
  CHECK(submitted.windows[0].queue_submitted_commands == 6);
  CHECK(submitted.windows[0].command_list_submissions == 1);
  CHECK(submitted.windows[0].active_frames == 1);
  wuwa_tfr::AccumulateTraceSubmission(
      submitted, std::size_t{2}, 1, true, 20);
  CHECK(submitted.windows[2].queue_submitted_commands == 1);
  CHECK(submitted.windows[2].command_list_submissions == 1);
  wuwa_tfr::AccumulateTraceSubmission(
      submitted, std::size_t{1}, 1, true, 15);
  using wuwa_tfr::TraceInvestigationView;
  CHECK(wuwa_tfr::TraceInvestigationVisible(
      pso_a, submitted.windows, TraceInvestigationView::NormalPartial,
      false, false));
  CHECK(wuwa_tfr::TraceInvestigationVisible(
      pso_a, submitted.windows,
      TraceInvestigationView::NormalPartialNoDiscard, false, false));
  CHECK(!wuwa_tfr::TraceInvestigationVisible(
      pso_a, submitted.windows,
      TraceInvestigationView::NormalPartialNoDiscard, true, false));
  CHECK(wuwa_tfr::TraceInvestigationVisible(
      pso_a, submitted.windows,
      TraceInvestigationView::NormalPartialDiscard, true, false));
  CHECK(!wuwa_tfr::TraceInvestigationVisible(
      pso_a, submitted.windows,
      TraceInvestigationView::NormalPartialDiscard, false, true));
  CHECK(wuwa_tfr::TraceInvestigationVisible(
      pso_a, submitted.windows,
      TraceInvestigationView::NormalPartialStrictSpatialDither, true, true));
  CHECK(!wuwa_tfr::TraceInvestigationVisible(
      pso_a, submitted.windows,
      TraceInvestigationView::NormalPartialStrictSpatialDither, true, false));
  auto normal_partial_without_full = submitted.windows;
  normal_partial_without_full[2] = {};
  CHECK(wuwa_tfr::TraceInvestigationVisible(
      pso_a, normal_partial_without_full,
      TraceInvestigationView::NormalPartialStrictSpatialDither, true, true));
  auto direct_key = pso_a;
  direct_key.geometry.kind = wuwa_tfr::TraceDrawKind::Direct;
  direct_key.geometry.index_buffer.reset();
  direct_key.geometry.observations &= ~wuwa_tfr::TraceObservedIndexBuffer;
  CHECK(wuwa_tfr::TraceInvestigationVisible(
      direct_key, submitted.windows, TraceInvestigationView::NormalPartial,
      false, false));
  auto mesh_key = pso_a;
  mesh_key.geometry.kind = wuwa_tfr::TraceDrawKind::Mesh;
  CHECK(!wuwa_tfr::TraceInvestigationVisible(
      mesh_key, submitted.windows, TraceInvestigationView::NormalPartial,
      false, false));
  auto incomplete_key = pso_a;
  incomplete_key.geometry.observations &= ~wuwa_tfr::TraceObservedPass;
  CHECK(!wuwa_tfr::TraceInvestigationVisible(
      incomplete_key, submitted.windows,
      TraceInvestigationView::NormalPartial, false, false));
  CHECK(wuwa_tfr::TraceInvestigationVisible(
      incomplete_key, submitted.windows, TraceInvestigationView::ShowAll,
      false, false));
  std::array<wuwa_tfr::TraceSubmissionWindowMetrics, 3> unobserved{};
  CHECK(!wuwa_tfr::TraceInvestigationVisible(pso_a, unobserved,
      TraceInvestigationView::NormalPartial, false, false));
  CHECK(wuwa_tfr::TraceInvestigationVisible(pso_a, unobserved,
      TraceInvestigationView::ShowAll, false, false));
  auto normal_only = submitted.windows;
  normal_only[1] = {};
  CHECK(!wuwa_tfr::TraceInvestigationVisible(pso_a, normal_only,
      TraceInvestigationView::NormalPartial, false, false));

  const auto full_range = wuwa_tfr::FullTraceCandidateRange(5);
  CHECK((full_range == wuwa_tfr::TraceCandidateRange{0, 5}));
  const auto first_half = wuwa_tfr::FirstTraceCandidateHalf(full_range);
  const auto second_half = wuwa_tfr::SecondTraceCandidateHalf(full_range);
  CHECK((first_half == wuwa_tfr::TraceCandidateRange{0, 3}));
  CHECK((second_half == wuwa_tfr::TraceCandidateRange{3, 5}));
  CHECK((wuwa_tfr::FirstTraceCandidateHalf(first_half) ==
      wuwa_tfr::TraceCandidateRange{0, 2}));
  CHECK((wuwa_tfr::SecondTraceCandidateHalf(first_half) ==
      wuwa_tfr::TraceCandidateRange{2, 3}));
  CHECK((wuwa_tfr::FirstTraceCandidateHalf({2, 3}) ==
      wuwa_tfr::TraceCandidateRange{2, 3}));
  CHECK((wuwa_tfr::SecondTraceCandidateHalf({2, 3}) ==
      wuwa_tfr::TraceCandidateRange{3, 3}));

  CHECK(std::string(wuwa_tfr::TraceInvestigationViewName(
             TraceInvestigationView::NormalPartialNoDiscard)) ==
      "normal-partial-no-discard");
  CHECK(std::string(wuwa_tfr::TraceInvestigationViewName(
             TraceInvestigationView::ShowAll)) == "show-all");
  CHECK(std::string(wuwa_tfr::TraceInvestigationViewName(
             TraceInvestigationView::NormalOnlyRimShortlist)) ==
      "normal-only-rim-shortlist");
  std::array<wuwa_tfr::TraceSubmissionWindowMetrics, 3> transition{};
  transition[1].command_list_submissions = 3;
  transition[2].command_list_submissions = 3;
  CHECK(wuwa_tfr::TraceFadeTransitionCandidate(transition));

  const std::array<wuwa_tfr::TraceSubmissionWindowMetrics, 3>
      normal_only_candidate{{
      {.command_list_submissions = 1},
      {.command_list_submissions = 0},
      {.command_list_submissions = 0},
  }};
  CHECK(wuwa_tfr::TraceNormalOnlySubmissionCandidate(normal_only_candidate));
  CHECK(!wuwa_tfr::TraceNormalOnlySubmissionCandidate(transition));
  CHECK(wuwa_tfr::TracePartialFullEqual(transition));
  transition[2].command_list_submissions = 4;
  CHECK(wuwa_tfr::TraceFadeTransitionCandidate(transition));
  CHECK(!wuwa_tfr::TracePartialFullEqual(transition));
  transition[0].command_list_submissions = 1;
  CHECK(!wuwa_tfr::TraceFadeTransitionCandidate(transition));

  // A selected SKIP is intentionally not passed to the submission ledger.
  const auto before_skip = submitted;
  const bool selected_for_skip = true;
  if (!selected_for_skip)
    wuwa_tfr::AccumulateTraceSubmission(
        submitted, std::size_t{2}, 1, true, 21);
  CheckSameSubmissionWindows(submitted.windows, before_skip.windows);

  psos.DestroyWhere([](const auto& key) { return key.owner == 1; });
  CHECK(psos.Find(reused.id) == nullptr);
  CHECK(psos.Find(other_device.id) && psos.Find(other_device.id)->live);

  TraceIncarnationIndex<Identity> bounded;
  const auto oldest = bounded.Activate({3, 1}, {1});
  const auto middle = bounded.Activate({3, 2}, {2});
  const auto newest = bounded.Activate({3, 3}, {3});
  CHECK(bounded.size() == 3);
  CHECK(bounded.PruneTo(2) == 1);
  CHECK(bounded.size() == 2);
  CHECK(bounded.Find(oldest.id) == nullptr);
  CHECK(bounded.FindActive({3, 1}) == nullptr);
  CHECK(bounded.Find(middle.id) && bounded.Find(newest.id));

  wuwa_tfr::TraceIncarnationIndex<std::uint64_t> view_slots;
  const TraceLiveHandleKey view_key{4, 99};
  const auto view_created =
      wuwa_tfr::UpdateTraceVersionedSlot(
          view_slots, view_key, std::uint64_t{1000});
  CHECK(view_created.version != 0);
  CHECK(!view_created.duplicate && !view_created.replaced);
  const auto view_duplicate =
      wuwa_tfr::UpdateTraceVersionedSlot(
          view_slots, view_key, std::uint64_t{1000});
  CHECK(view_duplicate.version == view_created.version);
  CHECK(view_duplicate.duplicate && !view_duplicate.replaced);

  const auto view_replaced =
      wuwa_tfr::UpdateTraceVersionedSlot(
          view_slots, view_key, std::uint64_t{2000});
  CHECK(view_replaced.version != view_created.version);
  CHECK(!view_replaced.duplicate && view_replaced.replaced);
  CHECK(view_slots.FindActive(view_key));
  CHECK(view_slots.FindActive(view_key)->identity == 2000);
  CHECK(view_slots.size() == 1);

  std::uint64_t latest_view_version = view_replaced.version;
  for (std::uint64_t identity = 3000; identity != 3100; ++identity) {
    const auto replacement =
        wuwa_tfr::UpdateTraceVersionedSlot(view_slots, view_key, identity);
    CHECK(replacement.replaced && !replacement.duplicate);
    CHECK(replacement.version > latest_view_version);
    latest_view_version = replacement.version;
    CHECK(view_slots.size() == 1);
  }

  CHECK(view_slots.Destroy(view_key) == latest_view_version);
  const auto view_reused =
      wuwa_tfr::UpdateTraceVersionedSlot(
          view_slots, view_key, std::uint64_t{4000});
  CHECK(view_reused.version > latest_view_version);
  CHECK(!view_reused.duplicate && !view_reused.replaced);
  CHECK(view_slots.FindActive(view_key)->identity == 4000);

  wuwa_tfr::TraceLifecycleAmbiguityDiagnostics<Identity, 2> ambiguities;
  ambiguities.Record(d1h7, {100}, {200}, 10, 1);
  ambiguities.Record(d1h7, {200}, {300}, 11, 2);
  ambiguities.Record({2, 7}, {400}, {500}, 12, 3);
  ambiguities.Record({3, 8}, {600}, {700}, 13, 4);
  const auto ambiguity_snapshot = ambiguities.GetSnapshot();
  CHECK(ambiguity_snapshot.total_events == 4);
  CHECK(ambiguity_snapshot.unique_handles == 3);
  CHECK(ambiguity_snapshot.max_events_for_one_handle == 2);
  CHECK(ambiguity_snapshot.samples.size() == 2);
  CHECK(ambiguity_snapshot.samples[0].key == d1h7);
  CHECK(ambiguity_snapshot.samples[0].previous_identity.fingerprint == 100);
  CHECK(ambiguity_snapshot.samples[0].new_identity.fingerprint == 200);
  CHECK(ambiguity_snapshot.samples[0].frame == 10);
  CHECK(ambiguity_snapshot.samples[0].event_serial == 1);
  CHECK(ambiguity_snapshot.samples[0].handle_event_count == 2);
  ambiguities.Reset();
  const auto reset_snapshot = ambiguities.GetSnapshot();
  CHECK(reset_snapshot.total_events == 0);
  CHECK(reset_snapshot.unique_handles == 0);
  CHECK(reset_snapshot.max_events_for_one_handle == 0);
  CHECK(reset_snapshot.samples.empty());

  std::cout << "trace submission identity tests passed\n";
}
