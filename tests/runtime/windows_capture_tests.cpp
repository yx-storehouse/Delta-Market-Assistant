#include "application/vision/dxgi_observation_source.h"
#include <QCoreApplication>
#include <functional>
#include <iostream>
#include <limits>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

using namespace relink::vision;
using namespace relink::runtime::observation;
namespace {
int assertions = 0, failures = 0;
void check(bool value, const char* label) {
    ++assertions; if (!value) ++failures;
    std::cout << (value ? "PASS " : "FAIL ") << label << '\n';
}
ObservationDemand demand() {
    ObservationDemand d;
    d.demandId = QStringLiteral("test-demand");
    d.context = {QStringLiteral("test-run"), QStringLiteral("test-session"), captureClockDomain(), QStringLiteral("test-step"), 0, 1};
    d.createdMonoMs = d.notBeforeMonoMs = captureClockMs();
    d.deadlineMonoMs = d.createdMonoMs + 5000;
    d.roiSpecs = {{QStringLiteral("test-roi"), QStringLiteral("client_physical_px"), 0, 0, 4, 4, 1}};
    return d;
}
CaptureRequest request(const ObservationDemand& d) { return {d, QStringLiteral("request-1"), 1, d.notBeforeMonoMs}; }
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    check(qpcTicksToMs(0, 10000000) == 0, "qpc_zero");
    check(qpcTicksToMs(10000001, 10000000) == 1000, "qpc_floor");
    check(qpcTicksToMs(19999999, 10000000) == 1999, "qpc_fraction_floor");
    check(qpcTicksToMs(-1, 100) == -1, "qpc_negative_rejected");
    check(qpcTicksToMs(1, 0) == -1, "qpc_zero_frequency_rejected");
    check(qpcTicksToMs(1, -1) == -1, "qpc_negative_frequency_rejected");
    check(qpcTicksToMs(1, 1000000000001LL) == -1, "qpc_excessive_frequency_rejected");
    check(qpcTicksToMs(std::numeric_limits<qint64>::max(), 1) == -1, "qpc_result_overflow_rejected");
    check(qpcTicksToMs(std::numeric_limits<qint64>::max(), 10000000) == 922337203685477LL, "qpc_no_intermediate_overflow");
    check(qpcTicksToMs(9007199254740991LL, 1000) == 9007199254740991LL, "qpc_safe_integer_boundary");
    check(qpcTicksToMs(9007199254740992LL, 1000) == -1, "qpc_fraction_cannot_exceed_safe_integer");
    check(captureClockMs() >= 0 && captureClockMs() <= captureClockMs(), "native_qpc_clock_monotonic");
    QByteArray black(4 * 4 * 4, '\0');
    auto summary = summarizeBgra(black, 4, 4, 16);
    check(summary.valid && summary.nearBlack && summary.sampledPixels == 16, "black_frame_classified");
    black[3] = char(255);
    check(summarizeBgra(black, 4, 4, 16).nearBlack, "alpha_ignored_for_black_check");
    black[10] = char(240);
    summary = summarizeBgra(black, 4, 4, 16);
    check(summary.valid && !summary.nearBlack && summary.maximum == 240, "color_detected");
    check(!summarizeBgra(black, 0, 4, 16).valid, "zero_width_rejected");
    check(!summarizeBgra(black, 4, -1, 16).valid, "negative_height_rejected");
    check(!summarizeBgra(black, 4, 4, 15).valid, "short_stride_rejected");
    check(!summarizeBgra(black, 8193, 1, 32772).valid, "oversized_width_rejected");
    check(!summarizeBgra(black, 4, 8193, 16).valid, "oversized_height_rejected");
    check(!summarizeBgra(black, 4, 4, 32769).valid, "oversized_stride_rejected");
    check(!summarizeBgra(QByteArray(63, '\0'), 4, 4, 16).valid, "truncated_pixels_rejected");
    check(!summarizeBgra(QByteArray(65, '\0'), 4, 4, 16).valid, "extra_pixels_rejected");
    QByteArray padded(80, '\0'); padded[17] = char(255);
    check(summarizeBgra(padded, 4, 4, 20).nearBlack, "stride_padding_not_pixels");
    padded[20] = char(8);
    check(!summarizeBgra(padded, 4, 4, 20).nearBlack, "black_threshold_boundary");
    const auto d = demand();
    check(sameObservationDemand(d, d), "demand_identity_equal");
    const std::function<void(ObservationDemand&)> mutations[] = {
        [](auto& x){x.demandId += "x";}, [](auto& x){x.context.runId += "x";},
        [](auto& x){x.context.sessionId += "x";}, [](auto& x){x.context.stepId += "x";},
        [](auto& x){x.context.clockDomainId += "x";}, [](auto& x){++x.context.cancelEpoch;},
        [](auto& x){++x.context.viewportGeneration;}, [](auto& x){x.mode = ObservationMode::BoundedWatch;},
        [](auto& x){x.purpose = ObservationPurpose::Receipt;}, [](auto& x){++x.createdMonoMs;},
        [](auto& x){++x.notBeforeMonoMs;}, [](auto& x){++x.deadlineMonoMs;},
        [](auto& x){++x.maxFrameAgeMs;}, [](auto& x){++x.frameBudget;}, [](auto& x){++x.minIntervalMs;},
        [](auto& x){x.persistence = "disk";}, [](auto& x){x.roiSpecs.clear();},
        [](auto& x){x.roiSpecs[0].roiId += "x";}, [](auto& x){x.roiSpecs[0].coordinateSpace += "x";},
        [](auto& x){++x.roiSpecs[0].x;}, [](auto& x){++x.roiSpecs[0].y;},
        [](auto& x){++x.roiSpecs[0].width;}, [](auto& x){++x.roiSpecs[0].height;}, [](auto& x){++x.roiSpecs[0].transformVersion;}
    };
    for (const auto& mutate : mutations) {
        auto changed = d; mutate(changed);
        check(!sameObservationDemand(d, changed), "immutable_demand_field_change_rejected");
        DxgiObservationSource source({}, d);
        check(source.capture(request(changed)).errorCode == "E_CAPTURE_REQUEST_INVALID", "source_rejects_mutated_demand_before_capture");
    }
    DxgiObservationSource cancelled({}, d);
    cancelled.cancel(d.demandId, d.context);
    const auto cancelledReply = cancelled.capture(request(d));
    check(cancelledReply.status == CaptureStatus::Cancelled && cancelledReply.errorCode == "E_CAPTURE_CANCELLED", "cancel_before_first_capture");
    check(cancelledReply.frame.pixels.isEmpty(), "cancelled_has_no_pixels");
    check(cancelledReply.requestId == "request-1" && cancelledReply.requestNumber == 1, "failure_preserves_correlation");
    check(cancelled.capture(request(d)).status == CaptureStatus::Cancelled, "cancel_is_sticky");
    DxgiObservationSource unrelatedCancel({}, d);
    auto otherContext = d.context; ++otherContext.cancelEpoch;
    unrelatedCancel.cancel(d.demandId, otherContext);
    unrelatedCancel.cancel(QStringLiteral("different"), d.context);
    check(unrelatedCancel.capture(request(d)).errorCode == "E_WINDOW_IDENTITY", "unrelated_cancellation_ignored");
    check(unrelatedCancel.capture(request(d)).errorCode == "E_CAPTURE_REQUEST_INVALID", "duplicate_request_rejected");
    for (int n : {0, -1, 2, 601}) {
        DxgiObservationSource source({}, d); auto r = request(d); r.requestNumber = n;
        check(source.capture(r).errorCode == "E_CAPTURE_REQUEST_INVALID", "request_number_budget_enforced");
    }
    DxgiObservationSource badId({}, d); auto invalid = request(d); invalid.requestId.clear();
    check(badId.capture(invalid).errorCode == "E_CAPTURE_REQUEST_INVALID", "missing_request_id");
    invalid.requestId = QString(201, QLatin1Char('x'));
    check(badId.capture(invalid).errorCode == "E_CAPTURE_REQUEST_INVALID", "long_request_id");
    invalid = request(d); invalid.requestedMonoMs = d.notBeforeMonoMs - 1;
    check(badId.capture(invalid).errorCode == "E_CAPTURE_REQUEST_INVALID", "pre_barrier_request");
    invalid.requestedMonoMs = d.deadlineMonoMs + 1;
    check(badId.capture(invalid).errorCode == "E_CAPTURE_REQUEST_INVALID", "post_deadline_request");
    auto wrongClock = d; wrongClock.context.clockDomainId = "replay-clock";
    DxgiObservationSource clockSource({}, wrongClock);
    check(clockSource.capture(request(wrongClock)).errorCode == "E_CAPTURE_CLOCK_DOMAIN", "clock_domains_not_interchangeable");
    auto expired = d; expired.createdMonoMs -= 10000; expired.notBeforeMonoMs -= 10000; expired.deadlineMonoMs -= 10000;
    DxgiObservationSource expiredSource({}, expired);
    check(expiredSource.capture(request(expired)).errorCode == "E_CAPTURE_DEADLINE", "expired_request_no_native_capture");
    QString error;
    check(!bindTargetWindow(0, 0, &error).valid() && error == "E_WINDOW_IDENTITY", "null_window_rejected");
    check(validateTargetWindow({}) == "E_WINDOW_IDENTITY", "invalid_window_validation");
    const auto before = foregroundWindow();
    check(!activateTargetWindow({}, &error) && error == "E_WINDOW_IDENTITY", "invalid_focus_target_rejected");
    { ForegroundReturnGuard guard({}); check(!guard.restore(&error), "invalid_restore_reports_failure"); }
    check(foregroundWindow() == before, "negative_tests_do_not_change_foreground");
#ifdef Q_OS_WIN
    HWND hidden = CreateWindowExW(0, L"STATIC", L"capture-unit-fixture", WS_OVERLAPPEDWINDOW,
        10, 10, 100, 100, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    check(hidden != nullptr, "create_hidden_native_fixture");
    const auto bound = bindTargetWindow(reinterpret_cast<quintptr>(hidden), GetCurrentProcessId(), &error);
    check(bound.valid() && error.isEmpty(), "bind_hidden_window_identity");
    check(!bound.executableName.isEmpty() && !bound.executableName.contains('\\'), "only_executable_basename_retained");
    check(validateTargetWindow(bound) == "E_WINDOW_NOT_VISIBLE", "hidden_window_not_capture_ready");
    check(!bindTargetWindow(bound.hwnd, GetCurrentProcessId() + 1, &error).valid(), "wrong_pid_rejected");
    auto rebound = bound; ++rebound.processCreated;
    check(validateTargetWindow(rebound) == "E_WINDOW_IDENTITY", "process_timestamp_mismatch_rejected");
    rebound = bound; rebound.windowClass += "x";
    check(validateTargetWindow(rebound) == "E_WINDOW_IDENTITY", "window_class_mismatch_rejected");
    DestroyWindow(hidden);
    check(validateTargetWindow(bound) == "E_WINDOW_IDENTITY", "destroyed_window_rejected");
    check(foregroundWindow() == before, "hidden_fixture_never_claims_foreground");
#endif
    std::cout << "WINDOWS_CAPTURE_TESTS=" << (failures ? "FAIL" : "PASS") << "; assertions=" << assertions
              << "; failures=" << failures << "; live_capture=false; image_file_writes=0\n";
    return failures ? 1 : 0;
}
