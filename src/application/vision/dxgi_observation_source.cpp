#include "dxgi_observation_source.h"

#include <QUuid>
#include <algorithm>
#include <cstring>
#include <utility>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#endif

namespace relink::vision {
using namespace runtime::observation;
namespace {
bool sameContext(const runtime::RuntimeContext& a, const runtime::RuntimeContext& b) {
    return a.runId == b.runId && a.sessionId == b.sessionId && a.stepId == b.stepId
        && a.clockDomainId == b.clockDomainId && a.cancelEpoch == b.cancelEpoch
        && a.viewportGeneration == b.viewportGeneration;
}
#ifdef Q_OS_WIN
template<class T> class Com {
public:
    Com() = default;
    ~Com() { if (p) p->Release(); }
    Com(const Com&) = delete;
    Com& operator=(const Com&) = delete;
    T* get() const { return p; }
    T** put() { if (p) p->Release(); p = nullptr; return &p; }
    T* operator->() const { return p; }
private:
    T* p = nullptr;
};
struct HeldFrame {
    IDXGIOutputDuplication* duplication;
    bool held = false;
    ~HeldFrame() { if (held) duplication->ReleaseFrame(); }
};
struct MappedTexture {
    ID3D11DeviceContext* context;
    ID3D11Texture2D* texture;
    bool mapped = false;
    ~MappedTexture() { if (mapped) context->Unmap(texture, 0); }
};
#endif
}

struct DxgiCaptureResources::State {
    TargetWindow target;
    bool ready = false;
#ifdef Q_OS_WIN
    DWORD thread = 0;
    Com<IDXGIFactory1> factory;
    Com<IDXGIAdapter1> adapter;
    Com<IDXGIOutput> output;
    Com<ID3D11Device> device;
    Com<ID3D11DeviceContext> context;
    Com<IDXGIOutput1> output1;
    Com<IDXGIOutputDuplication> duplication;
    Com<ID3D11Texture2D> staging;
    DXGI_OUTPUT_DESC outputDesc{};
#endif
};

DxgiCaptureResources::DxgiCaptureResources() = default;
DxgiCaptureResources::~DxgiCaptureResources() = default;
bool DxgiCaptureResources::discard() {
    if (m_busy.exchange(true)) return false;
    if (m_state) { m_state.reset(); ++m_invalidations; }
    m_busy.store(false);
    return true;
}
QJsonObject DxgiCaptureResources::metrics() const {
    return {{"schema", "dxgi-resource-cache-v1"}, {"initializations", m_initializations},
        {"reuses", m_reuses}, {"invalidations", m_invalidations}, {"staging_creates", m_stagingCreates},
        {"ready", m_state && m_state->ready}, {"pixel_cache", false}, {"background_capture", false}};
}
bool sameCaptureResourceTarget(const TargetWindow& a, const TargetWindow& b) {
    return a.valid() && b.valid() && a.hwnd == b.hwnd && a.pid == b.pid
        && a.processCreated == b.processCreated && a.windowClass == b.windowClass
        && a.executableName == b.executableName && a.clientRect == b.clientRect
        && a.monitorRect == b.monitorRect && a.monitor == b.monitor && a.dpi == b.dpi;
}

bool sameObservationDemand(const ObservationDemand& a, const ObservationDemand& b) {
    if (a.demandId != b.demandId || !sameContext(a.context, b.context) || a.mode != b.mode
        || a.purpose != b.purpose || a.createdMonoMs != b.createdMonoMs || a.notBeforeMonoMs != b.notBeforeMonoMs
        || a.deadlineMonoMs != b.deadlineMonoMs || a.maxFrameAgeMs != b.maxFrameAgeMs || a.frameBudget != b.frameBudget
        || a.minIntervalMs != b.minIntervalMs || a.persistence != b.persistence || a.roiSpecs.size() != b.roiSpecs.size()) return false;
    for (qsizetype i = 0; i < a.roiSpecs.size(); ++i) {
        const auto& x = a.roiSpecs[i]; const auto& y = b.roiSpecs[i];
        if (x.roiId != y.roiId || x.coordinateSpace != y.coordinateSpace || x.x != y.x || x.y != y.y
            || x.width != y.width || x.height != y.height || x.transformVersion != y.transformVersion) return false;
    }
    return true;
}

PixelSummary summarizeBgra(const QByteArray& pixels, int width, int height, int stride) {
    PixelSummary s;
    if (width < 1 || height < 1 || width > 8192 || height > 8192 || stride < qint64(width) * 4
        || stride > 32768 || qint64(stride) * height > 134217728 || pixels.size() != qint64(stride) * height) return s;
    const qint64 count = qint64(width) * height;
    const qint64 step = std::max<qint64>(1, count / 4096);
    for (qint64 i = 0; i < count; i += step) {
        const auto* p = reinterpret_cast<const unsigned char*>(pixels.constData()) + (i / width) * stride + (i % width) * 4;
        const int v = std::max({int(p[0]), int(p[1]), int(p[2])});
        s.minimum = std::min(s.minimum, v); s.maximum = std::max(s.maximum, v); ++s.sampledPixels;
    }
    s.valid = true; s.nearBlack = s.maximum < 8;
    return s;
}

DxgiObservationSource::DxgiObservationSource(TargetWindow target, ObservationDemand demand,
        std::shared_ptr<DxgiCaptureResources> resources)
    : m_target(std::move(target)), m_demand(std::move(demand)),
      m_resources(resources ? std::move(resources) : std::make_shared<DxgiCaptureResources>()) {}

void DxgiObservationSource::cancel(const QString& demandId, const runtime::RuntimeContext& context) {
    if (demandId == m_demand.demandId && sameContext(context, m_demand.context)) m_cancelled.store(true);
}

CaptureReply DxgiObservationSource::capture(const CaptureRequest& request) {
    CaptureReply reply;
    reply.requestId = request.requestId; reply.requestNumber = request.requestNumber;
    const auto fail = [&](const QString& code, CaptureStatus status = CaptureStatus::Failed) {
        reply.status = status; reply.errorCode = code; reply.frame = {}; return reply;
    };
    if (m_busy.exchange(true)) return fail(QStringLiteral("E_CAPTURE_BUSY"));
    struct BusyReset { std::atomic<bool>& b; ~BusyReset() { b.store(false); } } reset{m_busy};
    if (m_resources->m_busy.exchange(true)) return fail(QStringLiteral("E_CAPTURE_RESOURCE_BUSY"));
    BusyReset resourceBusy{m_resources->m_busy};
    // Declared before any mapped texture/held desktop frame. Those transient
    // objects unwind first, then the failed cache is released here.
    bool captureSucceeded = false;
    struct DropFailedCache {
        DxgiCaptureResources& owner; bool& succeeded;
        ~DropFailedCache() {
            if (!succeeded && owner.m_state) { owner.m_state.reset(); ++owner.m_invalidations; }
        }
    } cacheFailure{*m_resources, captureSucceeded};
    ObservationAdapter validator(this);
    if (!validator.validateDemand(request.demand).valid || !sameObservationDemand(request.demand, m_demand)
        || request.requestId.isEmpty() || request.requestId.size() > 200 || request.requestNumber <= m_lastRequestNumber
        || request.requestNumber > m_demand.frameBudget || request.requestedMonoMs < m_demand.notBeforeMonoMs
        || request.requestedMonoMs > m_demand.deadlineMonoMs)
        return fail(QStringLiteral("E_CAPTURE_REQUEST_INVALID"));
    if (m_demand.context.clockDomainId != captureClockDomain()) return fail(QStringLiteral("E_CAPTURE_CLOCK_DOMAIN"));
    if (m_cancelled.load()) return fail(QStringLiteral("E_CAPTURE_CANCELLED"), CaptureStatus::Cancelled);
    const qint64 start = captureClockMs();
    if (start < request.requestedMonoMs || start > m_demand.deadlineMonoMs) return fail(QStringLiteral("E_CAPTURE_DEADLINE"));
    m_lastRequestNumber = request.requestNumber;
    const QString windowError = validateTargetWindow(m_target);
    if (!windowError.isEmpty()) return fail(windowError);
    for (const auto& roi : m_demand.roiSpecs) {
        if (qint64(roi.x) + roi.width > m_target.clientRect.width()
            || qint64(roi.y) + roi.height > m_target.clientRect.height()) return fail(QStringLiteral("E_ROI_OUTSIDE_FRAME"));
    }
#ifdef Q_OS_WIN
    const qint64 stop = std::min(m_demand.deadlineMonoMs, start + 1500);
    auto interrupted = [&]() -> QString {
        if (m_cancelled.load()) return QStringLiteral("E_CAPTURE_CANCELLED");
        if (captureClockMs() >= stop) return QStringLiteral("E_CAPTURE_TIMEOUT");
        return {};
    };
    auto abort = [&](const QString& code) { return fail(code, m_cancelled.load() ? CaptureStatus::Cancelled : CaptureStatus::Failed); };
    auto& owner = *m_resources;
    if (owner.m_state && owner.m_state->ready) {
        auto& old = *owner.m_state;
        DXGI_OUTPUT_DESC current{};
        const bool valid = old.thread == GetCurrentThreadId()
            && sameCaptureResourceTarget(old.target, m_target) && old.factory->IsCurrent()
            && SUCCEEDED(old.device->GetDeviceRemovedReason()) && SUCCEEDED(old.output->GetDesc(&current))
            && current.AttachedToDesktop && current.Monitor == old.outputDesc.Monitor
            && current.Rotation == old.outputDesc.Rotation
            && EqualRect(&current.DesktopCoordinates, &old.outputDesc.DesktopCoordinates);
        if (!valid) { owner.m_state.reset(); ++owner.m_invalidations; }
    }
    if (!owner.m_state) owner.m_state = std::make_unique<DxgiCaptureResources::State>();
    auto& gpu = *owner.m_state;
    auto& factory=gpu.factory; auto& adapter=gpu.adapter; auto& output=gpu.output;
    auto& outputDesc=gpu.outputDesc; auto& device=gpu.device; auto& context=gpu.context;
    auto& output1=gpu.output1; auto& duplication=gpu.duplication; auto& staging=gpu.staging;
    if (!gpu.ready) {
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(factory.put()))))
        return fail(QStringLiteral("E_DXGI_FACTORY"));
    bool found = false;
    for (UINT i = 0; i < 32 && !found; ++i) {
        if (factory->EnumAdapters1(i, adapter.put()) == DXGI_ERROR_NOT_FOUND) break;
        if (!adapter.get()) break;
        for (UINT j = 0; j < 32; ++j) {
            if (adapter->EnumOutputs(j, output.put()) == DXGI_ERROR_NOT_FOUND) break;
            if (!output.get()) break;
            if (SUCCEEDED(output->GetDesc(&outputDesc)) && outputDesc.AttachedToDesktop
                && reinterpret_cast<quintptr>(outputDesc.Monitor) == m_target.monitor) { found = true; break; }
        }
    }
    if (!found) return fail(QStringLiteral("E_DXGI_OUTPUT"));
    if (outputDesc.Rotation != DXGI_MODE_ROTATION_IDENTITY) return fail(QStringLiteral("E_DXGI_ROTATION_UNSUPPORTED"));
    if (const auto e = interrupted(); !e.isEmpty()) return abort(e);
    if (FAILED(D3D11CreateDevice(adapter.get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            nullptr, 0, D3D11_SDK_VERSION, device.put(), nullptr, context.put()))) return fail(QStringLiteral("E_D3D_DEVICE"));
    if (FAILED(output->QueryInterface(__uuidof(IDXGIOutput1), reinterpret_cast<void**>(output1.put()))))
        return fail(QStringLiteral("E_DXGI_OUTPUT_INTERFACE"));
    if (FAILED(output1->DuplicateOutput(device.get(), duplication.put()))) return fail(QStringLiteral("E_DXGI_DUPLICATION"));
    gpu.target = m_target; gpu.thread = GetCurrentThreadId(); gpu.ready = true;
    ++owner.m_initializations;
    } else { ++owner.m_reuses; }
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    const int width = m_target.clientRect.width(), height = m_target.clientRect.height();
    // DXGI copies only the bound client rectangle into CPU-readable memory.
    const int left = m_target.clientRect.x() - outputDesc.DesktopCoordinates.left;
    const int top = m_target.clientRect.y() - outputDesc.DesktopCoordinates.top;
    for (int attempt = 0; attempt < 600; ++attempt) {
        if (const auto e = interrupted(); !e.isEmpty()) return abort(e);
        if (const auto e = validateTargetWindow(m_target); !e.isEmpty()) return fail(e);
        Com<IDXGIResource> resource;
        DXGI_OUTDUPL_FRAME_INFO info{};
        const auto wait = static_cast<UINT>(std::clamp<qint64>(stop - captureClockMs(), 0, 20));
        const HRESULT acquired = duplication->AcquireNextFrame(wait, &info, resource.put());
        if (acquired == DXGI_ERROR_WAIT_TIMEOUT) continue;
        if (FAILED(acquired)) return fail(QStringLiteral("E_DXGI_ACQUIRE"));
        HeldFrame held{duplication.get(), true};
        if (info.ProtectedContentMaskedOut) return fail(QStringLiteral("E_DXGI_MASKED_CONTENT"));
        const qint64 presented = qpcTicksToMs(info.LastPresentTime.QuadPart, frequency.QuadPart);
        // Ignore cached pre-barrier images and pointer-only updates. Floor QPC
        // conversion introduces <1 ms uncertainty; keep a conservative 1 ms.
        if (info.LastPresentTime.QuadPart <= 0 || presented - 1 < m_demand.notBeforeMonoMs
            || presented - 1 < request.requestedMonoMs) continue;
        Com<ID3D11Texture2D> texture;
        if (FAILED(resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(texture.put()))))
            return fail(QStringLiteral("E_DXGI_TEXTURE"));
        D3D11_TEXTURE2D_DESC desc{};
        texture->GetDesc(&desc);
        if (desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM || desc.SampleDesc.Count != 1
            || left < 0 || top < 0 || qint64(left) + width > desc.Width || qint64(top) + height > desc.Height)
            return fail(QStringLiteral("E_DXGI_TEXTURE_SHAPE"));
        desc.Width = width; desc.Height = height; desc.MipLevels = 1; desc.ArraySize = 1;
        desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = 0; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ; desc.MiscFlags = 0;
        if (!staging.get()) {
            if (FAILED(device->CreateTexture2D(&desc, nullptr, staging.put()))) return fail(QStringLiteral("E_D3D_STAGING"));
            ++owner.m_stagingCreates;
        }
        const D3D11_BOX box{UINT(left), UINT(top), 0, UINT(left + width), UINT(top + height), 1};
        context->CopySubresourceRegion(staging.get(), 0, 0, 0, 0, texture.get(), 0, &box);
        context->Flush();
        MappedTexture mapped{context.get(), staging.get(), false};
        D3D11_MAPPED_SUBRESOURCE data{};
        if (const auto e = interrupted(); !e.isEmpty()) return abort(e);
        // A blocking Map waits only for this flushed GPU copy (~1-2 ms). The
        // former DO_NOT_WAIT + Sleep(1) loop slept a whole default timer
        // quantum (~15.6 ms) on almost every frame before reading pixels.
        if (FAILED(context->Map(staging.get(), 0, D3D11_MAP_READ, 0, &data))) return fail(QStringLiteral("E_D3D_MAP"));
        mapped.mapped = true;
        if (!data.pData || data.RowPitch < UINT(width * 4)) return fail(QStringLiteral("E_D3D_ROW_PITCH"));
        FrameEnvelope frame;
        frame.width = width; frame.height = height; frame.strideBytes = width * 4;
        frame.validBytes = qint64(frame.strideBytes) * height;
        frame.pixels.resize(frame.validBytes);
        for (int y = 0; y < height; ++y)
            std::memcpy(frame.pixels.data() + qsizetype(y) * frame.strideBytes,
                static_cast<const char*>(data.pData) + qsizetype(y) * data.RowPitch, frame.strideBytes);
        if (const auto e = interrupted(); !e.isEmpty()) return abort(e);
        if (const auto e = validateTargetWindow(m_target); !e.isEmpty()) return fail(e);
        const auto summary = summarizeBgra(frame.pixels, width, height, frame.strideBytes);
        if (!summary.valid || summary.nearBlack) return fail(QStringLiteral("E_FRAME_BLACK_OR_INVALID"));
        frame.frameId = QStringLiteral("dxgi:%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
        frame.leaseId = frame.frameId + QStringLiteral(":lease");
        frame.demandId = m_demand.demandId; frame.captureSessionId = m_demand.context.sessionId;
        frame.context = m_demand.context; frame.sourceKind = sourceKind();
        frame.windowRef = QStringLiteral("window:%1:%2:%3").arg(m_target.pid).arg(m_target.hwnd).arg(m_target.processCreated);
        frame.dpiX = frame.dpiY = m_target.dpi;
        frame.slotIndex = (request.requestNumber - 1) % 2; frame.slotGeneration = request.requestNumber;
        frame.captureStartMonoMs = start; frame.captureEndMonoMs = captureClockMs();
        frame.freshnessBasis = FreshnessBasis::SourceTimestamp;
        frame.sourceMonoMs = presented; frame.sourceUncertaintyMs = 1;
        if (frame.captureEndMonoMs - (presented - 1) > m_demand.maxFrameAgeMs)
            return fail(QStringLiteral("E_FRAME_TOO_OLD"));
        reply.status = CaptureStatus::Captured; reply.frame = std::move(frame);
        captureSucceeded = true;
        return reply;
    }
    return fail(QStringLiteral("E_CAPTURE_FRAME_LIMIT"));
#else
    return fail(QStringLiteral("E_PLATFORM_UNSUPPORTED"));
#endif
}
} // namespace relink::vision
