#pragma once

#include <QByteArray>
#include <QHash>
#include <QJsonObject>
#include <QSet>
#include <QString>
#include <QVector>

namespace relink::vision {

enum class WorkerRole { Coordinator, Worker };
enum class SessionState { Fresh, HelloSent, HelloReceived, Ready, ShuttingDown, Closed, Faulted };

struct ProtocolLimits {
    int maxLineBytes = 1048576;
    qint64 maxFrameBytes = 134217728;
    int maxTokens = 2000;
    int maxTokenText = 4096;
    int maxRois = 64;
    int maxInFlight = 1;
    // A soft history ceiling stops new work while reserving terminal/release/
    // shutdown/bye slots to drain the single active request.
    int maxSessionMessages = 4096;
};

struct ProtocolError {
    QString code;
    QString message;
    bool isError() const { return !code.isEmpty(); }
};

struct WorkerEnvelope {
    int protocolVersion = 1;
    QString sessionId;
    QString messageId;
    QString kind;
    QJsonObject body;
};

struct ParseBatch {
    QVector<WorkerEnvelope> messages;
    ProtocolError error;
    bool ok() const { return !error.isError(); }
};

class NdjsonFramer final {
public:
    explicit NdjsonFramer(ProtocolLimits limits = {});
    ParseBatch feed(const QByteArray& chunk);
    ParseBatch finish();
    bool failed() const { return m_failed; }
    void reset();

private:
    ProtocolLimits m_limits;
    QByteArray m_buffer;
    bool m_failed = false;
};

class WorkerProtocol final {
public:
    WorkerProtocol(QString sessionId, WorkerRole role,
                   ProtocolLimits limits = {});

    static bool validateEnvelope(const WorkerEnvelope& envelope,
                                 const ProtocolLimits& limits = {},
                                 ProtocolError* error = nullptr);
    static bool decodeLine(const QByteArray& line, WorkerEnvelope* envelope,
                           ProtocolError* error = nullptr,
                           ProtocolLimits limits = {});
    static QByteArray encodeLine(const WorkerEnvelope& envelope,
                                 ProtocolError* error = nullptr,
                                 ProtocolLimits limits = {});

    bool send(const WorkerEnvelope& envelope, QByteArray* wire = nullptr,
              ProtocolError* error = nullptr);
    bool receive(const WorkerEnvelope& envelope, ProtocolError* error = nullptr);

    WorkerRole role() const { return m_role; }
    SessionState state() const { return m_state; }
    QString sessionId() const { return m_sessionId; }
    const ProtocolLimits& limits() const { return m_limits; }
    int inFlightCount() const;
    int messageHistorySize() const { return m_seenMessages.size(); }
    bool requestCancelled(const QString& requestId) const;
    bool requestTerminal(const QString& requestId) const;
    bool requestReleased(const QString& requestId) const;
    bool hasRequest(const QString& requestId) const;

private:
    struct Request {
        QString demandId;
        QString leaseId;
        QString frameId;
        QJsonObject context;
        QSet<QString> roiIds;
        int slotIndex = 0;
        qint64 slotGeneration = 0;
        bool cancelled = false;
        bool terminal = false;
        bool released = false;
    };

    bool transition(bool outgoing, const WorkerEnvelope& envelope, ProtocolError* error);
    bool validateDirection(bool outgoing, const QString& kind, ProtocolError* error) const;
    bool registerMessage(const WorkerEnvelope& envelope, ProtocolError* error);
    bool trackOutgoing(const WorkerEnvelope& envelope, ProtocolError* error);
    bool trackIncoming(const WorkerEnvelope& envelope, ProtocolError* error);
    void fault(const ProtocolError& error);

    WorkerRole m_role;
    SessionState m_state = SessionState::Fresh;
    QString m_sessionId;
    ProtocolLimits m_limits;
    QHash<QString, QByteArray> m_seenMessages;
    QHash<QString, Request> m_requests;
    QSet<QString> m_cancelledDemands;
    QHash<int, QJsonObject> m_pool;
    QString m_provider, m_modelId;
};

} // namespace relink::vision
