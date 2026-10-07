#include "worker_protocol.h"

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QRegularExpression>
#include <QStringConverter>

#include <cmath>
#include <limits>

namespace relink::vision {
namespace {

constexpr qint64 kJsonSafeInteger = 9007199254740991LL;
const QRegularExpression kId(QStringLiteral("^[A-Za-z0-9][A-Za-z0-9._:-]{0,127}$"));
const QRegularExpression kMapping(QStringLiteral("^Local\\\\RelinkVision_([A-Za-z0-9][A-Za-z0-9._:-]{0,127})_([01])$"));

ProtocolError makeError(const char* code, const QString& message)
{
    return {QString::fromLatin1(code), message};
}

bool fail(ProtocolError* output, const char* code, const QString& message)
{
    if (output) *output = makeError(code, message);
    return false;
}

bool validLimits(const ProtocolLimits& limits, ProtocolError* error)
{
    if (limits.maxLineBytes < 64 || limits.maxLineBytes > 1048576
        || limits.maxFrameBytes < 4 || limits.maxFrameBytes > 134217728
        || limits.maxTokens < 0 || limits.maxTokens > 2000
        || limits.maxTokenText < 1 || limits.maxTokenText > 4096
        || limits.maxRois < 1 || limits.maxRois > 64 || limits.maxInFlight != 1
        || limits.maxSessionMessages < 8 || limits.maxSessionMessages > 65536)
        return fail(error, "E_MESSAGE_INVALID", QStringLiteral("protocol limits exceed supported bounds"));
    return true;
}

bool drainCompletion(const WorkerEnvelope& envelope)
{
    return envelope.kind == QStringLiteral("result")
        || (envelope.kind == QStringLiteral("error")
            && !envelope.body.value(QStringLiteral("request_id")).isNull())
        || envelope.kind == QStringLiteral("release_frame")
        || envelope.kind == QStringLiteral("shutdown")
        || envelope.kind == QStringLiteral("bye");
}

bool exactKeys(const QJsonObject& object, const QSet<QString>& required,
               const QSet<QString>& optional, ProtocolError* error)
{
    for (const QString& key : object.keys()) {
        if (!required.contains(key) && !optional.contains(key))
            return fail(error, "E_MESSAGE_INVALID", QStringLiteral("unexpected field: %1").arg(key));
    }
    for (const QString& key : required) {
        if (!object.contains(key))
            return fail(error, "E_MESSAGE_INVALID", QStringLiteral("missing field: %1").arg(key));
    }
    return true;
}

bool validId(const QJsonValue& value, ProtocolError* error, const QString& field)
{
    return value.isString() && kId.match(value.toString()).hasMatch()
        ? true : fail(error, "E_MESSAGE_INVALID", QStringLiteral("invalid id: %1").arg(field));
}

bool validInteger(const QJsonValue& value, qint64 minimum, qint64 maximum,
                 ProtocolError* error, const QString& field)
{
    if (!value.isDouble())
        return fail(error, "E_MESSAGE_INVALID", QStringLiteral("invalid integer: %1").arg(field));
    const double number = value.toDouble();
    if (!std::isfinite(number) || number < minimum || number > maximum || std::floor(number) != number)
        return fail(error, "E_MESSAGE_INVALID", QStringLiteral("integer out of bounds: %1").arg(field));
    return true;
}

bool validNumber(const QJsonValue& value, double minimum, double maximum,
                 ProtocolError* error, const QString& field)
{
    if (!value.isDouble() || !std::isfinite(value.toDouble())
        || value.toDouble() < minimum || value.toDouble() > maximum)
        return fail(error, "E_MESSAGE_INVALID", QStringLiteral("number out of bounds: %1").arg(field));
    return true;
}

bool validContext(const QJsonValue& value, ProtocolError* error)
{
    if (!value.isObject()) return fail(error, "E_MESSAGE_INVALID", QStringLiteral("context must be an object"));
    const auto object = value.toObject();
    if (!exactKeys(object, {QStringLiteral("run_id"), QStringLiteral("session_id"),
                            QStringLiteral("clock_domain_id"), QStringLiteral("step_id"),
                            QStringLiteral("cancel_epoch"), QStringLiteral("viewport_generation")}, {}, error)) return false;
    for (const auto& field : {QStringLiteral("run_id"), QStringLiteral("session_id"),
                              QStringLiteral("clock_domain_id"), QStringLiteral("step_id")}) {
        if (!validId(object.value(field), error, field)) return false;
    }
    return validInteger(object.value(QStringLiteral("cancel_epoch")), 0, kJsonSafeInteger, error, QStringLiteral("cancel_epoch"))
        && validInteger(object.value(QStringLiteral("viewport_generation")), 0, kJsonSafeInteger, error, QStringLiteral("viewport_generation"));
}

bool validDescriptor(const QJsonValue& value, const QString& sessionId,
                     const ProtocolLimits& limits, ProtocolError* error)
{
    if (!value.isObject()) return fail(error, "E_MESSAGE_INVALID", QStringLiteral("frame descriptor must be an object"));
    const auto object = value.toObject();
    if (!exactKeys(object, {QStringLiteral("transport"), QStringLiteral("mapping_name"), QStringLiteral("lease_id"),
                            QStringLiteral("slot_index"), QStringLiteral("slot_generation"), QStringLiteral("offset_bytes"),
                            QStringLiteral("byte_length"), QStringLiteral("capacity_bytes")}, {}, error)) return false;
    if (object.value(QStringLiteral("transport")).toString() != QStringLiteral("win32_named_shared_memory"))
        return fail(error, "E_SHM_OPEN", QStringLiteral("file transport is not allowed"));
    const auto mapping = kMapping.match(object.value(QStringLiteral("mapping_name")).toString());
    if (!mapping.hasMatch() || mapping.captured(1) != sessionId)
        return fail(error, "E_SHM_OPEN", QStringLiteral("mapping name is not bound to this session"));
    if (!validId(object.value(QStringLiteral("lease_id")), error, QStringLiteral("lease_id"))) return false;
    if (!validInteger(object.value(QStringLiteral("slot_index")), 0, 1, error, QStringLiteral("slot_index"))) return false;
    if (mapping.captured(2).toInt() != object.value(QStringLiteral("slot_index")).toInt())
        return fail(error,"E_SHM_OPEN",QStringLiteral("mapping slot differs from descriptor slot"));
    if (!validInteger(object.value(QStringLiteral("slot_generation")), 1, kJsonSafeInteger, error, QStringLiteral("slot_generation"))) return false;
    if (!validInteger(object.value(QStringLiteral("offset_bytes")), 0, limits.maxFrameBytes, error, QStringLiteral("offset_bytes"))) return false;
    if (!validInteger(object.value(QStringLiteral("byte_length")), 1, limits.maxFrameBytes, error, QStringLiteral("byte_length"))) return false;
    if (!validInteger(object.value(QStringLiteral("capacity_bytes")), 1, limits.maxFrameBytes, error, QStringLiteral("capacity_bytes"))) return false;
    const qint64 offset = object.value(QStringLiteral("offset_bytes")).toInteger();
    const qint64 length = object.value(QStringLiteral("byte_length")).toInteger();
    const qint64 capacity = object.value(QStringLiteral("capacity_bytes")).toInteger();
    return offset <= capacity && length <= capacity - offset
        ? true : fail(error, "E_SHM_BOUNDS", QStringLiteral("descriptor exceeds section capacity"));
}

bool validFrame(const QJsonValue& value, const QString& sessionId,
                const ProtocolLimits& limits, ProtocolError* error)
{
    if (!value.isObject()) return fail(error, "E_MESSAGE_INVALID", QStringLiteral("frame must be an object"));
    const auto object = value.toObject();
    if (!exactKeys(object, {QStringLiteral("frame_id"), QStringLiteral("demand_id"), QStringLiteral("capture_session_id"),
                            QStringLiteral("context"), QStringLiteral("source_kind"), QStringLiteral("window_ref"),
                            QStringLiteral("width"), QStringLiteral("height"), QStringLiteral("dpi_x"), QStringLiteral("dpi_y"),
                            QStringLiteral("pixel_format"), QStringLiteral("stride_bytes"), QStringLiteral("valid_bytes"),
                            QStringLiteral("capture_start_mono_ms"), QStringLiteral("capture_end_mono_ms"),
                            QStringLiteral("freshness_basis"), QStringLiteral("source_mono_ms"),
                            QStringLiteral("source_uncertainty_ms"), QStringLiteral("descriptor")}, {}, error)) return false;
    for (const auto& field : {QStringLiteral("frame_id"), QStringLiteral("demand_id"),
                              QStringLiteral("capture_session_id"), QStringLiteral("window_ref")}) {
        if (!validId(object.value(field), error, field)) return false;
    }
    if (!validContext(object.value(QStringLiteral("context")), error)) return false;
    if (!QStringList{QStringLiteral("replay"), QStringLiteral("wgc"), QStringLiteral("dxgi")}.contains(object.value(QStringLiteral("source_kind")).toString()))
        return fail(error, "E_MESSAGE_INVALID", QStringLiteral("unsupported frame source"));
    if (object.value(QStringLiteral("pixel_format")).toString() != QStringLiteral("BGRA8"))
        return fail(error, "E_PIXEL_FORMAT", QStringLiteral("only BGRA8 is supported"));
    if (!validInteger(object.value(QStringLiteral("width")), 1, 8192, error, QStringLiteral("width"))
        || !validInteger(object.value(QStringLiteral("height")), 1, 8192, error, QStringLiteral("height"))
        || !validInteger(object.value(QStringLiteral("dpi_x")), 48, 768, error, QStringLiteral("dpi_x"))
        || !validInteger(object.value(QStringLiteral("dpi_y")), 48, 768, error, QStringLiteral("dpi_y"))) return false;
    if (!validInteger(object.value(QStringLiteral("stride_bytes")), 1, 32768, error, QStringLiteral("stride_bytes"))
        || !validInteger(object.value(QStringLiteral("valid_bytes")), 1, limits.maxFrameBytes, error, QStringLiteral("valid_bytes"))) return false;
    const qint64 stride = object.value(QStringLiteral("stride_bytes")).toInteger();
    const qint64 height = object.value(QStringLiteral("height")).toInteger();
    if (stride < object.value(QStringLiteral("width")).toInteger() * 4
        || stride > std::numeric_limits<qint64>::max() / height
        || stride * height != object.value(QStringLiteral("valid_bytes")).toInteger())
        return fail(error, "E_SHM_BOUNDS", QStringLiteral("stride*height must equal valid_bytes"));
    if (!validInteger(object.value(QStringLiteral("capture_start_mono_ms")), 0, kJsonSafeInteger, error, QStringLiteral("capture_start_mono_ms"))
        || !validInteger(object.value(QStringLiteral("capture_end_mono_ms")), 0, kJsonSafeInteger, error, QStringLiteral("capture_end_mono_ms"))) return false;
    if (object.value(QStringLiteral("capture_end_mono_ms")).toInteger() < object.value(QStringLiteral("capture_start_mono_ms")).toInteger())
        return fail(error, "E_MESSAGE_INVALID", QStringLiteral("capture interval is reversed"));
    const QString freshness = object.value(QStringLiteral("freshness_basis")).toString();
    if (!QStringList{QStringLiteral("source_timestamp"), QStringLiteral("post_barrier_capture"), QStringLiteral("unproven")}.contains(freshness))
        return fail(error, "E_MESSAGE_INVALID", QStringLiteral("unsupported freshness basis"));
    if (!object.value(QStringLiteral("source_mono_ms")).isNull()
        && !validInteger(object.value(QStringLiteral("source_mono_ms")), 0, kJsonSafeInteger, error, QStringLiteral("source_mono_ms"))) return false;
    if (!object.value(QStringLiteral("source_uncertainty_ms")).isNull()
        && !validInteger(object.value(QStringLiteral("source_uncertainty_ms")), 0, 60000, error, QStringLiteral("source_uncertainty_ms"))) return false;
    if (freshness == QStringLiteral("source_timestamp")) {
        if (object.value(QStringLiteral("source_mono_ms")).isNull() || object.value(QStringLiteral("source_uncertainty_ms")).isNull())
            return fail(error,"E_MESSAGE_INVALID",QStringLiteral("source timestamp requires time and uncertainty"));
    } else if (!object.value(QStringLiteral("source_mono_ms")).isNull() || !object.value(QStringLiteral("source_uncertainty_ms")).isNull())
        return fail(error,"E_MESSAGE_INVALID",QStringLiteral("non-source freshness must have null source time"));
    if (object.value(QStringLiteral("descriptor")).toObject().value(QStringLiteral("byte_length")) != object.value(QStringLiteral("valid_bytes")))
        return fail(error,"E_SHM_BOUNDS",QStringLiteral("descriptor length differs from valid bytes"));
    return validDescriptor(object.value(QStringLiteral("descriptor")), sessionId, limits, error);
}

bool validRoiSpecs(const QJsonValue& value, const ProtocolLimits& limits, ProtocolError* error)
{
    const auto rois = value.toArray();
    if (rois.isEmpty() || rois.size() > limits.maxRois) return fail(error, "E_MESSAGE_INVALID", QStringLiteral("ROI count exceeds limit"));
    for (const auto& item : rois) {
        if (!item.isObject()) return fail(error, "E_MESSAGE_INVALID", QStringLiteral("ROI is not an object"));
        const auto object = item.toObject();
        if (!exactKeys(object, {QStringLiteral("roi_id"), QStringLiteral("coordinate_space"), QStringLiteral("x"),
                                QStringLiteral("y"), QStringLiteral("width"), QStringLiteral("height"),
                                QStringLiteral("transform_version")}, {}, error)) return false;
        if (!validId(object.value(QStringLiteral("roi_id")), error, QStringLiteral("roi_id"))
            || object.value(QStringLiteral("coordinate_space")).toString() != QStringLiteral("client_physical_px")
            || !validInteger(object.value(QStringLiteral("x")), 0, 8191, error, QStringLiteral("x"))
            || !validInteger(object.value(QStringLiteral("y")), 0, 8191, error, QStringLiteral("y"))
            || !validInteger(object.value(QStringLiteral("width")), 1, 8192, error, QStringLiteral("width"))
            || !validInteger(object.value(QStringLiteral("height")), 1, 8192, error, QStringLiteral("height"))
            || !validInteger(object.value(QStringLiteral("transform_version")), 1, kJsonSafeInteger, error, QStringLiteral("transform_version"))) return false;
    }
    return true;
}

bool validBody(const WorkerEnvelope& envelope, const ProtocolLimits& limits, ProtocolError* error)
{
    const auto& body = envelope.body;
    if (envelope.kind == QStringLiteral("hello")) {
        if (!exactKeys(body, {QStringLiteral("supported_versions"), QStringLiteral("transport"), QStringLiteral("pixel_formats"),
                              QStringLiteral("max_line_bytes"), QStringLiteral("max_frame_bytes"), QStringLiteral("max_tokens"),
                              QStringLiteral("max_in_flight"), QStringLiteral("memory_pool")}, {}, error)) return false;
        if (body.value(QStringLiteral("supported_versions")).toArray() != QJsonArray{1}
            || body.value(QStringLiteral("transport")).toString() != QStringLiteral("win32_named_shared_memory")
            || body.value(QStringLiteral("pixel_formats")).toArray() != QJsonArray{QStringLiteral("BGRA8")})
            return fail(error, "E_PROTOCOL_VERSION", QStringLiteral("hello capabilities do not match v1"));
        if (body.value(QStringLiteral("max_line_bytes")).toInteger() != limits.maxLineBytes
            || body.value(QStringLiteral("max_frame_bytes")).toInteger() != limits.maxFrameBytes
            || body.value(QStringLiteral("max_tokens")).toInteger() != limits.maxTokens
            || body.value(QStringLiteral("max_in_flight")).toInteger() != limits.maxInFlight)
            return fail(error, "E_MESSAGE_INVALID", QStringLiteral("hello protection limits do not match coordinator"));
        const auto pool = body.value(QStringLiteral("memory_pool")).toArray();
        if (pool.size() != 2) return fail(error, "E_MESSAGE_INVALID", QStringLiteral("memory_pool must contain two slots"));
        QSet<int> slotIndices;
        for (const auto& item : pool) {
            if (!item.isObject()) return fail(error, "E_MESSAGE_INVALID", QStringLiteral("memory_pool slot is not an object"));
            const auto object = item.toObject();
            if (!exactKeys(object, {QStringLiteral("slot_index"), QStringLiteral("mapping_name"), QStringLiteral("capacity_bytes")}, {}, error)) return false;
            if (!validInteger(object.value(QStringLiteral("slot_index")), 0, 1, error, QStringLiteral("slot_index"))) return false;
            const int slot = object.value(QStringLiteral("slot_index")).toInt();
            if (slotIndices.contains(slot)) return fail(error, "E_MESSAGE_INVALID", QStringLiteral("duplicate memory_pool slot"));
            slotIndices.insert(slot);
            const auto mapping = kMapping.match(object.value(QStringLiteral("mapping_name")).toString());
            if (!mapping.hasMatch() || mapping.captured(1) != envelope.sessionId || mapping.captured(2).toInt() != slot)
                return fail(error, "E_SHM_OPEN", QStringLiteral("memory_pool mapping is not bound to session"));
            if (!validInteger(object.value(QStringLiteral("capacity_bytes")), 4, limits.maxFrameBytes, error, QStringLiteral("capacity_bytes"))) return false;
        }
        return slotIndices == QSet<int>{0, 1};
    }
    if (envelope.kind == QStringLiteral("ready")) {
        if (!exactKeys(body, {QStringLiteral("selected_version"), QStringLiteral("worker_build"), QStringLiteral("provider"),
                              QStringLiteral("model_id"), QStringLiteral("model_sha256"), QStringLiteral("capabilities")}, {}, error)) return false;
        if (body.value(QStringLiteral("selected_version")).toInteger() != 1
            || !validId(body.value(QStringLiteral("worker_build")), error, QStringLiteral("worker_build"))
            || !validId(body.value(QStringLiteral("provider")), error, QStringLiteral("provider"))
            || !validId(body.value(QStringLiteral("model_id")), error, QStringLiteral("model_id"))
            || !QRegularExpression(QStringLiteral("^[a-f0-9]{64}$")).match(body.value(QStringLiteral("model_sha256")).toString()).hasMatch()
            || body.value(QStringLiteral("capabilities")).toArray() != QJsonArray{QStringLiteral("BGRA8"), QStringLiteral("win32_named_shared_memory"), QStringLiteral("cancel"), QStringLiteral("release_frame")})
            return fail(error, "E_MESSAGE_INVALID", QStringLiteral("ready capabilities are invalid"));
        return true;
    }
    if (envelope.kind == QStringLiteral("recognize")) {
        if (!exactKeys(body, {QStringLiteral("request_id"), QStringLiteral("demand_id"), QStringLiteral("context"), QStringLiteral("frame"), QStringLiteral("roi_specs"), QStringLiteral("relative_budget_ms")}, {}, error)) return false;
        return validId(body.value(QStringLiteral("request_id")), error, QStringLiteral("request_id"))
            && validId(body.value(QStringLiteral("demand_id")), error, QStringLiteral("demand_id"))
            && validContext(body.value(QStringLiteral("context")), error)
            && validFrame(body.value(QStringLiteral("frame")), envelope.sessionId, limits, error)
            && validRoiSpecs(body.value(QStringLiteral("roi_specs")), limits, error)
            && validInteger(body.value(QStringLiteral("relative_budget_ms")), 1, 60000, error, QStringLiteral("relative_budget_ms"));
    }
    if (envelope.kind == QStringLiteral("result")) {
        if (!exactKeys(body, {QStringLiteral("request_id"), QStringLiteral("demand_id"), QStringLiteral("frame_id"), QStringLiteral("lease_id"), QStringLiteral("context"), QStringLiteral("status"), QStringLiteral("reason_code"), QStringLiteral("provider"), QStringLiteral("model_id"), QStringLiteral("elapsed_ms"), QStringLiteral("tokens")}, {}, error)) return false;
        if (!validId(body.value(QStringLiteral("request_id")), error, QStringLiteral("request_id"))
            || !validId(body.value(QStringLiteral("demand_id")), error, QStringLiteral("demand_id"))
            || !validId(body.value(QStringLiteral("frame_id")), error, QStringLiteral("frame_id"))
            || !validId(body.value(QStringLiteral("lease_id")), error, QStringLiteral("lease_id"))
            || !validContext(body.value(QStringLiteral("context")), error)
            || !validId(body.value(QStringLiteral("provider")), error, QStringLiteral("provider"))
            || !validId(body.value(QStringLiteral("model_id")), error, QStringLiteral("model_id"))
            || !validNumber(body.value(QStringLiteral("elapsed_ms")), 0, 3600000, error, QStringLiteral("elapsed_ms"))) return false;
        const QString status = body.value(QStringLiteral("status")).toString();
        if (status != QStringLiteral("ok") && status != QStringLiteral("unknown")) return fail(error, "E_MESSAGE_INVALID", QStringLiteral("invalid result status"));
        const QJsonValue reason = body.value(QStringLiteral("reason_code"));
        if ((status == QStringLiteral("ok") && !reason.isNull())
            || (status == QStringLiteral("unknown") && !QStringList{QStringLiteral("E_UNKNOWN_PAGE"), QStringLiteral("E_NO_TEXT")}.contains(reason.toString())))
            return fail(error, "E_MESSAGE_INVALID", QStringLiteral("result reason does not match status"));
        if (!body.value(QStringLiteral("tokens")).isArray()) return fail(error,"E_MESSAGE_INVALID",QStringLiteral("tokens must be an array"));
        const auto tokens = body.value(QStringLiteral("tokens")).toArray();
        if (tokens.size() > limits.maxTokens) return fail(error, "E_MESSAGE_TOO_LARGE", QStringLiteral("token count exceeds limit"));
        for (const auto& item : tokens) {
            if (!item.isObject()) return fail(error, "E_MESSAGE_INVALID", QStringLiteral("token is not an object"));
            const auto token = item.toObject();
            if (!exactKeys(token, {QStringLiteral("roi_id"), QStringLiteral("text"), QStringLiteral("score"), QStringLiteral("box_px")}, {}, error)) return false;
            if (!validId(token.value(QStringLiteral("roi_id")), error, QStringLiteral("roi_id"))
                || !token.value(QStringLiteral("text")).isString()
                || token.value(QStringLiteral("text")).toString().size() > limits.maxTokenText
                || !validNumber(token.value(QStringLiteral("score")), 0, 1, error, QStringLiteral("score"))) return false;
            const auto box = token.value(QStringLiteral("box_px")).toArray();
            if (box.size() != 4) return fail(error, "E_MESSAGE_INVALID", QStringLiteral("token box must contain four points"));
            for (const auto& point : box) {
                const auto pair = point.toArray();
                if (pair.size() != 2 || !validNumber(pair.at(0), 0, 8192, error, QStringLiteral("box_x"))
                    || !validNumber(pair.at(1), 0, 8192, error, QStringLiteral("box_y"))) return false;
            }
        }
        return true;
    }
    if (envelope.kind == QStringLiteral("release_frame")) {
        if (!exactKeys(body, {QStringLiteral("request_id"), QStringLiteral("lease_id"), QStringLiteral("slot_index"), QStringLiteral("slot_generation")}, {}, error)) return false;
        return validId(body.value(QStringLiteral("request_id")), error, QStringLiteral("request_id"))
            && validId(body.value(QStringLiteral("lease_id")), error, QStringLiteral("lease_id"))
            && validInteger(body.value(QStringLiteral("slot_index")), 0, 1, error, QStringLiteral("slot_index"))
            && validInteger(body.value(QStringLiteral("slot_generation")), 1, kJsonSafeInteger, error, QStringLiteral("slot_generation"));
    }
    if (envelope.kind == QStringLiteral("cancel")) {
        if (!exactKeys(body, {QStringLiteral("target_kind"), QStringLiteral("target_id"), QStringLiteral("reason")}, {}, error)) return false;
        return (body.value(QStringLiteral("target_kind")).toString() == QStringLiteral("request") || body.value(QStringLiteral("target_kind")).toString() == QStringLiteral("demand"))
            && validId(body.value(QStringLiteral("target_id")), error, QStringLiteral("target_id"))
            && QStringList{QStringLiteral("pause"), QStringLiteral("stop"), QStringLiteral("step_exit"), QStringLiteral("deadline"), QStringLiteral("viewport_changed"), QStringLiteral("shutdown")}.contains(body.value(QStringLiteral("reason")).toString())
            ? true : fail(error, "E_MESSAGE_INVALID", QStringLiteral("invalid cancel body"));
    }
    if (envelope.kind == QStringLiteral("cancelled")) {
        if (!exactKeys(body, {QStringLiteral("target_kind"), QStringLiteral("target_id"), QStringLiteral("registered")}, {}, error)) return false;
        return (body.value(QStringLiteral("target_kind")).toString() == QStringLiteral("request") || body.value(QStringLiteral("target_kind")).toString() == QStringLiteral("demand"))
            && validId(body.value(QStringLiteral("target_id")), error, QStringLiteral("target_id"))
            && body.value(QStringLiteral("registered")).toBool() == true
            ? true : fail(error, "E_MESSAGE_INVALID", QStringLiteral("invalid cancelled body"));
    }
    if (envelope.kind == QStringLiteral("error")) {
        if (!exactKeys(body, {QStringLiteral("request_id"), QStringLiteral("code"), QStringLiteral("retryable"), QStringLiteral("message")}, {}, error)) return false;
        if (!body.value(QStringLiteral("request_id")).isNull() && !validId(body.value(QStringLiteral("request_id")), error, QStringLiteral("request_id"))) return false;
        static const QStringList codes = {QStringLiteral("E_PROTOCOL_VERSION"), QStringLiteral("E_HANDSHAKE_REQUIRED"), QStringLiteral("E_MESSAGE_TOO_LARGE"), QStringLiteral("E_MESSAGE_INVALID"), QStringLiteral("E_SHM_OPEN"), QStringLiteral("E_SHM_BOUNDS"), QStringLiteral("E_LEASE_UNKNOWN"), QStringLiteral("E_ROI_OUT_OF_BOUNDS"), QStringLiteral("E_PIXEL_FORMAT"), QStringLiteral("E_CANCELLED"), QStringLiteral("E_DEADLINE"), QStringLiteral("E_WORKER_CRASH"), QStringLiteral("E_WORKER_UNRESPONSIVE"), QStringLiteral("E_CAPTURE_LOST"), QStringLiteral("E_WINDOW_LOST"), QStringLiteral("E_EMPTY_FRAME"), QStringLiteral("E_STALE_OR_UNPROVEN_FRAME"), QStringLiteral("E_CONTEXT_MISMATCH"), QStringLiteral("E_OCR_PROVIDER"), QStringLiteral("E_MODEL_MISMATCH"), QStringLiteral("E_UNKNOWN_PAGE"), QStringLiteral("E_NO_TEXT"), QStringLiteral("E_DUPLICATE_MESSAGE")};
        if (!codes.contains(body.value(QStringLiteral("code")).toString()) || !body.value(QStringLiteral("retryable")).isBool() || !body.value(QStringLiteral("message")).isString() || body.value(QStringLiteral("message")).toString().size() > 1024) return fail(error, "E_MESSAGE_INVALID", QStringLiteral("invalid error body"));
        return true;
    }
    if (envelope.kind == QStringLiteral("shutdown")) {
        if (!exactKeys(body, {QStringLiteral("reason")}, {}, error)) return false;
        return QStringList{QStringLiteral("normal"), QStringLiteral("fault"), QStringLiteral("user_stop")}.contains(body.value(QStringLiteral("reason")).toString()) ? true : fail(error, "E_MESSAGE_INVALID", QStringLiteral("invalid shutdown reason"));
    }
    if (envelope.kind == QStringLiteral("bye")) {
        if (!exactKeys(body, {QStringLiteral("all_leases_closed")}, {}, error)) return false;
        return body.value(QStringLiteral("all_leases_closed")).toBool() ? true : fail(error, "E_MESSAGE_INVALID", QStringLiteral("bye before leases closed"));
    }
    return fail(error, "E_MESSAGE_INVALID", QStringLiteral("unknown worker message kind: %1").arg(envelope.kind));
}

QByteArray fingerprint(const WorkerEnvelope& envelope)
{
    return QCryptographicHash::hash(QJsonDocument(QJsonObject{{QStringLiteral("protocol_version"), envelope.protocolVersion},
                                     {QStringLiteral("session_id"), envelope.sessionId},
                                     {QStringLiteral("message_id"), envelope.messageId},
                                     {QStringLiteral("kind"), envelope.kind},
                                     {QStringLiteral("body"), envelope.body}}).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256);
}

bool idempotentKind(const QString& kind)
{
    return kind == QStringLiteral("cancel") || kind == QStringLiteral("cancelled")
        || kind == QStringLiteral("release_frame") || kind == QStringLiteral("shutdown");
}

} // namespace

NdjsonFramer::NdjsonFramer(ProtocolLimits limits) : m_limits(limits) {}

ParseBatch NdjsonFramer::feed(const QByteArray& chunk)
{
    ParseBatch output;
    if (m_failed) { output.error=makeError("E_MESSAGE_INVALID",QStringLiteral("framer is faulted")); return output; }
    if (!validLimits(m_limits, &output.error)) { m_failed = true; return output; }
    qsizetype position = 0;
    while (position < chunk.size()) {
        const qsizetype newline = chunk.indexOf('\n', position);
        const qsizetype end = newline < 0 ? chunk.size() : newline;
        const qsizetype segmentLength = end-position;
        if (segmentLength > m_limits.maxLineBytes - m_buffer.size()) {
            output.error=makeError("E_MESSAGE_TOO_LARGE",QStringLiteral("NDJSON line exceeds limit"));
            m_buffer.clear(); m_failed=true; return output;
        }
        m_buffer.append(chunk.constData()+position, segmentLength);
        position=end+(newline>=0 ? 1 : 0);
        if (newline<0) break;
        WorkerEnvelope envelope;
        if (!WorkerProtocol::decodeLine(m_buffer, &envelope, &output.error, m_limits)) {
            m_buffer.clear(); m_failed=true; return output;
        }
        if (output.messages.size() >= 256) {
            output.error=makeError("E_MESSAGE_TOO_LARGE",QStringLiteral("control batch exceeds 256 messages"));
            m_buffer.clear(); m_failed=true; return output;
        }
        output.messages.push_back(envelope); m_buffer.clear();
    }
    return output;
}

ParseBatch NdjsonFramer::finish()
{
    ParseBatch output;
    if (m_failed) { output.error = makeError("E_MESSAGE_INVALID", QStringLiteral("framer is faulted")); return output; }
    if (!validLimits(m_limits, &output.error)) { m_failed = true; return output; }
    if (!m_buffer.isEmpty()) {
        output.error = makeError("E_MESSAGE_INVALID", QStringLiteral("NDJSON ended with a partial line"));
        m_failed = true;
    }
    return output;
}

void NdjsonFramer::reset()
{
    m_buffer.clear();
    m_failed = false;
}

WorkerProtocol::WorkerProtocol(QString sessionId, WorkerRole role, ProtocolLimits limits)
    : m_role(role), m_sessionId(std::move(sessionId)), m_limits(limits) {}

bool WorkerProtocol::validateEnvelope(const WorkerEnvelope& envelope, const ProtocolLimits& limits, ProtocolError* error)
{
    if (error) *error = {};
    if (!validLimits(limits, error)) return false;
    if (envelope.protocolVersion != 1) return fail(error, "E_PROTOCOL_VERSION", QStringLiteral("protocol version must be 1"));
    if (!validId(QJsonValue(envelope.sessionId), error, QStringLiteral("session_id"))
        || !validId(QJsonValue(envelope.messageId), error, QStringLiteral("message_id"))
        || envelope.kind.isEmpty()) {
        if (error && !error->isError()) *error = makeError("E_MESSAGE_INVALID", QStringLiteral("empty message kind"));
        return false;
    }
    if (!validBody(envelope, limits, error)) {
        if (error && !error->isError()) *error = makeError("E_MESSAGE_INVALID", QStringLiteral("invalid body field type or value"));
        return false;
    }
    return true;
}

bool WorkerProtocol::decodeLine(const QByteArray& line, WorkerEnvelope* envelope,
                                ProtocolError* error, ProtocolLimits limits)
{
    if (error) *error = {};
    if (!validLimits(limits, error)) return false;
    if (line.size() > limits.maxLineBytes) return fail(error, "E_MESSAGE_TOO_LARGE", QStringLiteral("NDJSON line exceeds limit"));
    if (line.isEmpty() || line.contains('\r') || line.contains('\n') || line.contains('\0') || line.startsWith("\xEF\xBB\xBF"))
        return fail(error, "E_MESSAGE_INVALID", QStringLiteral("invalid NDJSON framing or BOM"));
    QStringDecoder utf8(QStringDecoder::Utf8);
    const QString decoded=utf8.decode(line);
    Q_UNUSED(decoded);
    if (utf8.hasError()) return fail(error,"E_MESSAGE_INVALID",QStringLiteral("invalid UTF-8"));
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(line, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) return fail(error, "E_MESSAGE_INVALID", QStringLiteral("line is not one JSON object"));
    const auto object = document.object();
    if (!exactKeys(object, {QStringLiteral("protocol_version"), QStringLiteral("session_id"), QStringLiteral("message_id"), QStringLiteral("kind"), QStringLiteral("body")}, {}, error)) return false;
    if (!object.value(QStringLiteral("body")).isObject()
        || !validInteger(object.value(QStringLiteral("protocol_version")), 1, 1, error, QStringLiteral("protocol_version")))
        return fail(error,"E_MESSAGE_INVALID",QStringLiteral("invalid envelope types"));
    WorkerEnvelope value;
    value.protocolVersion = object.value(QStringLiteral("protocol_version")).toInt();
    value.sessionId = object.value(QStringLiteral("session_id")).toString();
    value.messageId = object.value(QStringLiteral("message_id")).toString();
    value.kind = object.value(QStringLiteral("kind")).toString();
    value.body = object.value(QStringLiteral("body")).toObject();
    if (!validateEnvelope(value, limits, error)) return false;
    if (envelope) *envelope = value;
    return true;
}

QByteArray WorkerProtocol::encodeLine(const WorkerEnvelope& envelope, ProtocolError* error, ProtocolLimits limits)
{
    if (!validateEnvelope(envelope, limits, error)) return {};
    const QByteArray bytes = QJsonDocument(QJsonObject{{QStringLiteral("protocol_version"), envelope.protocolVersion},
                                                       {QStringLiteral("session_id"), envelope.sessionId},
                                                       {QStringLiteral("message_id"), envelope.messageId},
                                                       {QStringLiteral("kind"), envelope.kind},
                                                       {QStringLiteral("body"), envelope.body}}).toJson(QJsonDocument::Compact) + '\n';
    if (bytes.size() - 1 > limits.maxLineBytes) {
        if (error) *error = makeError("E_MESSAGE_TOO_LARGE", QStringLiteral("encoded line exceeds limit"));
        return {};
    }
    return bytes;
}

bool WorkerProtocol::send(const WorkerEnvelope& envelope, QByteArray* wire, ProtocolError* error)
{
    if (error) *error = {};
    if (envelope.sessionId != m_sessionId) return fail(error, "E_CONTEXT_MISMATCH", QStringLiteral("session id mismatch"));
    if (!validateEnvelope(envelope, m_limits, error) || !validateDirection(true, envelope.kind, error)) return false;
    const QByteArray bytes = encodeLine(envelope, error, m_limits);
    if (bytes.isEmpty()) return false;
    const QByteArray fp = fingerprint(envelope);
    if (m_seenMessages.contains(envelope.messageId)) {
        if (m_seenMessages.value(envelope.messageId) != fp || !idempotentKind(envelope.kind))
            return fail(error, "E_DUPLICATE_MESSAGE", QStringLiteral("message id was already consumed"));
        if (wire) *wire = bytes;
        return true;
    }
    WorkerProtocol candidate = *this;
    if (!candidate.transition(true, envelope, error)
        || !(m_role == WorkerRole::Coordinator ? candidate.trackOutgoing(envelope, error) : candidate.trackIncoming(envelope, error))
        || !candidate.registerMessage(envelope, error)) return false;
    *this = std::move(candidate);
    if (wire) *wire = bytes;
    return true;
}

bool WorkerProtocol::receive(const WorkerEnvelope& envelope, ProtocolError* error)
{
    if (error) *error = {};
    if (envelope.sessionId != m_sessionId) return fail(error, "E_CONTEXT_MISMATCH", QStringLiteral("session id mismatch"));
    if (!validateEnvelope(envelope, m_limits, error) || !validateDirection(false, envelope.kind, error)) return false;
    // Direct in-memory receive obeys the same byte limit as NDJSON input.
    if (encodeLine(envelope, error, m_limits).isEmpty()) return false;
    const QByteArray fp = fingerprint(envelope);
    if (m_seenMessages.contains(envelope.messageId)) {
        if (m_seenMessages.value(envelope.messageId) != fp || !idempotentKind(envelope.kind))
            return fail(error, "E_DUPLICATE_MESSAGE", QStringLiteral("message id was already consumed"));
        return true;
    }
    WorkerProtocol candidate = *this;
    if (!candidate.transition(false, envelope, error)
        || !(m_role == WorkerRole::Coordinator ? candidate.trackIncoming(envelope, error) : candidate.trackOutgoing(envelope, error))
        || !candidate.registerMessage(envelope, error)) return false;
    *this = std::move(candidate);
    return true;
}

bool WorkerProtocol::validateDirection(bool outgoing, const QString& kind, ProtocolError* error) const
{
    const bool coordinator = m_role == WorkerRole::Coordinator;
    const QStringList outbound = coordinator
        ? QStringList{QStringLiteral("hello"), QStringLiteral("recognize"), QStringLiteral("cancel"), QStringLiteral("shutdown")}
        : QStringList{QStringLiteral("ready"), QStringLiteral("result"), QStringLiteral("error"), QStringLiteral("release_frame"), QStringLiteral("cancelled"), QStringLiteral("bye")};
    const QStringList inbound = coordinator
        ? QStringList{QStringLiteral("ready"), QStringLiteral("result"), QStringLiteral("error"), QStringLiteral("release_frame"), QStringLiteral("cancelled"), QStringLiteral("bye")}
        : QStringList{QStringLiteral("hello"), QStringLiteral("recognize"), QStringLiteral("cancel"), QStringLiteral("shutdown")};
    return (outgoing ? outbound : inbound).contains(kind)
        ? true : fail(error, "E_MESSAGE_INVALID", QStringLiteral("message direction is invalid"));
}

bool WorkerProtocol::registerMessage(const WorkerEnvelope& envelope, ProtocolError* error)
{
    if (m_seenMessages.contains(envelope.messageId)) return fail(error, "E_DUPLICATE_MESSAGE", QStringLiteral("message id was already consumed"));
    if (m_seenMessages.size() >= m_limits.maxSessionMessages
        && (!drainCompletion(envelope) || m_seenMessages.size() >= m_limits.maxSessionMessages + 4))
        return fail(error,"E_MESSAGE_TOO_LARGE",QStringLiteral("session message limit reached; drain and start new session"));
    m_seenMessages.insert(envelope.messageId, fingerprint(envelope));
    return true;
}

bool WorkerProtocol::trackOutgoing(const WorkerEnvelope& envelope, ProtocolError* error)
{
    const auto& body = envelope.body;
    if (envelope.kind == QStringLiteral("hello")) {
        for (const auto& value : body.value(QStringLiteral("memory_pool")).toArray()) {
            const auto slot = value.toObject();
            m_pool.insert(slot.value(QStringLiteral("slot_index")).toInt(), slot);
        }
    } else if (envelope.kind == QStringLiteral("recognize")) {
        const QString requestId = body.value(QStringLiteral("request_id")).toString();
        if (m_requests.contains(requestId)) return fail(error, "E_DUPLICATE_MESSAGE", QStringLiteral("request already exists"));
        if (inFlightCount() >= m_limits.maxInFlight) return fail(error, "E_MESSAGE_INVALID", QStringLiteral("only one request may be in flight"));
        const auto frame = body.value(QStringLiteral("frame")).toObject();
        const auto descriptor = frame.value(QStringLiteral("descriptor")).toObject();
        const auto context = body.value(QStringLiteral("context")).toObject();
        if (context != frame.value(QStringLiteral("context")).toObject()
            || context.value(QStringLiteral("session_id")).toString() != m_sessionId
            || body.value(QStringLiteral("demand_id")) != frame.value(QStringLiteral("demand_id")))
            return fail(error, "E_CONTEXT_MISMATCH", QStringLiteral("request and frame contexts differ"));
        const int slotIndex = descriptor.value(QStringLiteral("slot_index")).toInt();
        const auto slot = m_pool.value(slotIndex);
        if (slot.isEmpty() || slot.value(QStringLiteral("mapping_name")) != descriptor.value(QStringLiteral("mapping_name"))
            || slot.value(QStringLiteral("capacity_bytes")) != descriptor.value(QStringLiteral("capacity_bytes")))
            return fail(error, "E_LEASE_UNKNOWN", QStringLiteral("descriptor is not in the negotiated pool"));
        Request request;
        request.demandId = body.value(QStringLiteral("demand_id")).toString();
        if (m_cancelledDemands.contains(request.demandId)) return fail(error,"E_CANCELLED",QStringLiteral("demand has been cancelled"));
        request.leaseId = descriptor.value(QStringLiteral("lease_id")).toString();
        request.frameId = frame.value(QStringLiteral("frame_id")).toString();
        request.context = context;
        request.slotIndex = slotIndex;
        request.slotGeneration = descriptor.value(QStringLiteral("slot_generation")).toInteger();
        for (const auto& previous : m_requests) {
            if (previous.leaseId == request.leaseId || (previous.slotIndex == slotIndex && previous.slotGeneration >= request.slotGeneration))
                return fail(error,"E_LEASE_UNKNOWN",QStringLiteral("lease identity or generation was reused"));
        }
        for (const auto& value : body.value(QStringLiteral("roi_specs")).toArray()) {
            const auto roi = value.toObject();
            const auto roiId = roi.value(QStringLiteral("roi_id")).toString();
            if (request.roiIds.contains(roiId)) return fail(error,"E_MESSAGE_INVALID",QStringLiteral("duplicate ROI id"));
            request.roiIds.insert(roiId);
            if (roi.value(QStringLiteral("x")).toInt() > frame.value(QStringLiteral("width")).toInt() - roi.value(QStringLiteral("width")).toInt()
                || roi.value(QStringLiteral("y")).toInt() > frame.value(QStringLiteral("height")).toInt() - roi.value(QStringLiteral("height")).toInt())
                return fail(error,"E_ROI_OUT_OF_BOUNDS",QStringLiteral("ROI exceeds frame bounds"));
        }
        m_requests.insert(requestId, request);
    } else if (envelope.kind == QStringLiteral("cancel")) {
        const QString target = body.value(QStringLiteral("target_id")).toString();
        if (body.value(QStringLiteral("target_kind")).toString() == QStringLiteral("request")) {
            if (!m_requests.contains(target)) return fail(error,"E_MESSAGE_INVALID",QStringLiteral("cancel request is unknown"));
            m_requests[target].cancelled = true;
        } else {
            m_cancelledDemands.insert(target);
            for (auto& request : m_requests) if (request.demandId == target) request.cancelled = true;
        }
    } else if (envelope.kind == QStringLiteral("shutdown")) {
        for (auto& request : m_requests) if (!request.released) request.cancelled = true;
    }
    return true;
}

bool WorkerProtocol::trackIncoming(const WorkerEnvelope& envelope, ProtocolError* error)
{
    const auto& body = envelope.body;
    if (envelope.kind == QStringLiteral("ready")) {
        m_provider = body.value(QStringLiteral("provider")).toString();
        m_modelId = body.value(QStringLiteral("model_id")).toString();
    } else if (envelope.kind == QStringLiteral("result") || envelope.kind == QStringLiteral("error")) {
        const QString requestId = body.value(QStringLiteral("request_id")).toString();
        if (body.value(QStringLiteral("request_id")).isNull() && envelope.kind == QStringLiteral("error")) return true;
        if (!m_requests.contains(requestId)) return fail(error,"E_MESSAGE_INVALID",QStringLiteral("terminal request is unknown"));
        auto& request = m_requests[requestId];
        if (request.terminal) return fail(error,"E_MESSAGE_INVALID",QStringLiteral("request already has a terminal response"));
        if (request.released) return fail(error,"E_MESSAGE_INVALID",QStringLiteral("result after release"));
        if (envelope.kind == QStringLiteral("result")) {
            if (request.leaseId != body.value(QStringLiteral("lease_id")).toString()
                || request.frameId != body.value(QStringLiteral("frame_id")).toString()
                || request.demandId != body.value(QStringLiteral("demand_id")).toString()
                || request.context != body.value(QStringLiteral("context")).toObject())
                return fail(error,"E_CONTEXT_MISMATCH",QStringLiteral("result correlation mismatch"));
            if (m_provider != body.value(QStringLiteral("provider")).toString() || m_modelId != body.value(QStringLiteral("model_id")).toString())
                return fail(error,"E_MODEL_MISMATCH",QStringLiteral("result model does not match ready"));
            for (const auto& value : body.value(QStringLiteral("tokens")).toArray())
                if (!request.roiIds.contains(value.toObject().value(QStringLiteral("roi_id")).toString()))
                    return fail(error,"E_CONTEXT_MISMATCH",QStringLiteral("token ROI was not requested"));
        }
        if (request.cancelled && envelope.kind == QStringLiteral("result"))
            return fail(error,"E_CANCELLED",QStringLiteral("cancelled request result is ignored"));
        request.terminal = true;
    } else if (envelope.kind == QStringLiteral("release_frame")) {
        const QString requestId = body.value(QStringLiteral("request_id")).toString();
        if (!m_requests.contains(requestId)) return fail(error,"E_LEASE_UNKNOWN",QStringLiteral("release references unknown request"));
        auto& request = m_requests[requestId];
        if (request.leaseId != body.value(QStringLiteral("lease_id")).toString()
            || request.slotIndex != body.value(QStringLiteral("slot_index")).toInt()
            || request.slotGeneration != body.value(QStringLiteral("slot_generation")).toInteger())
            return fail(error,"E_LEASE_UNKNOWN",QStringLiteral("release lease generation does not match request"));
        if (!request.terminal && !request.cancelled) return fail(error,"E_MESSAGE_INVALID",QStringLiteral("release arrived before result or cancellation"));
        if (request.released) return fail(error,"E_DUPLICATE_MESSAGE",QStringLiteral("lease already released; repeat the original message id"));
        request.released = true;
    } else if (envelope.kind == QStringLiteral("cancelled")) {
        const QString target = body.value(QStringLiteral("target_id")).toString();
        if (body.value(QStringLiteral("target_kind")).toString() == QStringLiteral("request")) {
            if (!m_requests.contains(target) || !m_requests.value(target).cancelled)
                return fail(error,"E_MESSAGE_INVALID",QStringLiteral("unsolicited cancellation acknowledgement"));
        } else if (!m_cancelledDemands.contains(target)) return fail(error,"E_MESSAGE_INVALID",QStringLiteral("unsolicited demand acknowledgement"));
    } else if (envelope.kind == QStringLiteral("bye")) {
        if (inFlightCount() != 0) return fail(error,"E_MESSAGE_INVALID",QStringLiteral("bye before all leases released"));
    }
    return true;
}

void WorkerProtocol::fault(const ProtocolError& error)
{
    Q_UNUSED(error);
    m_state = SessionState::Faulted;
}

bool WorkerProtocol::transition(bool outgoing, const WorkerEnvelope& envelope, ProtocolError* error)
{
    Q_UNUSED(outgoing);
    if (m_state == SessionState::Faulted || m_state == SessionState::Closed)
        return fail(error,"E_MESSAGE_INVALID",QStringLiteral("session is closed"));
    if (envelope.kind == QStringLiteral("hello")) {
        if (m_state != SessionState::Fresh) return fail(error,"E_MESSAGE_INVALID",QStringLiteral("hello already exchanged"));
        m_state = m_role == WorkerRole::Coordinator ? SessionState::HelloSent : SessionState::HelloReceived;
    } else if (envelope.kind == QStringLiteral("ready")) {
        if (m_state != SessionState::HelloSent && m_state != SessionState::HelloReceived)
            return fail(error,"E_HANDSHAKE_REQUIRED",QStringLiteral("hello must precede ready"));
        m_state = SessionState::Ready;
    } else if (envelope.kind == QStringLiteral("shutdown")) {
        if (m_state != SessionState::Ready && m_state != SessionState::HelloSent && m_state != SessionState::HelloReceived)
            return fail(error,"E_HANDSHAKE_REQUIRED",QStringLiteral("hello must precede shutdown"));
        m_state = SessionState::ShuttingDown;
    } else if (envelope.kind == QStringLiteral("bye")) {
        if (m_state != SessionState::ShuttingDown) return fail(error,"E_MESSAGE_INVALID",QStringLiteral("shutdown must precede bye"));
        m_state = SessionState::Closed;
    } else if (envelope.kind == QStringLiteral("error") && envelope.body.value(QStringLiteral("request_id")).isNull()) {
        if (m_state == SessionState::Fresh) return fail(error,"E_HANDSHAKE_REQUIRED",QStringLiteral("hello is required"));
    } else if (m_state != SessionState::Ready && !(m_state == SessionState::ShuttingDown && envelope.kind != QStringLiteral("recognize"))) {
        return fail(error,"E_HANDSHAKE_REQUIRED",QStringLiteral("ready is required before this message"));
    }
    return true;
}

int WorkerProtocol::inFlightCount() const
{
    int count = 0;
    for (const auto& request : m_requests) if (!request.released) ++count;
    return count;
}

bool WorkerProtocol::requestCancelled(const QString& requestId) const { return m_requests.contains(requestId) && m_requests.value(requestId).cancelled; }
bool WorkerProtocol::requestTerminal(const QString& requestId) const { return m_requests.contains(requestId) && m_requests.value(requestId).terminal; }
bool WorkerProtocol::requestReleased(const QString& requestId) const { return m_requests.contains(requestId) && m_requests.value(requestId).released; }
bool WorkerProtocol::hasRequest(const QString& requestId) const { return m_requests.contains(requestId); }

} // namespace relink::vision
