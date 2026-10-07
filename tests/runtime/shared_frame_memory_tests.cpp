#include "application/vision/shared_frame_memory.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QUuid>

#include <iostream>
#include <limits>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

using namespace relink::vision;
namespace {
int assertions = 0;
int failures = 0;
void check(bool value, const QString& label)
{
    ++assertions;
    if (!value) ++failures;
    std::cout << (value ? "PASS " : "FAIL ") << label.toStdString() << '\n';
}

QString uniqueSession()
{
    return QStringLiteral("test-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
}

LeaseHandle lease(int slot, qint64 generation, qint64 bytes)
{
    LeaseHandle result;
    result.leaseId = QStringLiteral("lease-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    result.slotIndex = slot;
    result.generation = generation;
    result.byteLength = bytes;
    result.state = LeaseState::Pending;
    return result;
}
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    Q_UNUSED(app);

    const QString session = uniqueSession();
    const QByteArray pixels("synthetic-bgra-payload");
    SharedFrameMemory owner;
    QString error;
    check(owner.create(session, 0, 4096, &error), QStringLiteral("create_owner_mapping"));
    check(error.isEmpty() && owner.isOpen(), QStringLiteral("owner_open_state"));
    check(!owner.create(session, 0, 4096, &error) && error == QStringLiteral("E_SHM_BUSY")
              && owner.isOpen(), QStringLiteral("owner_reconfigure_requires_explicit_close"));
    check(owner.poolEntry().value(QStringLiteral("mapping_name")).toString()
              == QStringLiteral("Local\\RelinkVision_%1_0").arg(session),
          QStringLiteral("pool_entry_name_bound_to_session_slot"));
    check(owner.write(pixels, 32, &error) && error.isEmpty(), QStringLiteral("write_in_memory_bytes"));

    const LeaseHandle frameLease = lease(0, 1, pixels.size());
    const QJsonObject descriptor = owner.descriptor(frameLease);
    auto oversizedGeneration = frameLease;
    oversizedGeneration.generation = 9007199254740992LL;
    check(owner.descriptor(oversizedGeneration).isEmpty(),
          QStringLiteral("descriptor_rejects_unsafe_generation"));
    check(!descriptor.isEmpty(), QStringLiteral("descriptor_after_write"));
    check(descriptor.value(QStringLiteral("offset_bytes")).toInteger() == 32
              && descriptor.value(QStringLiteral("byte_length")).toInteger() == pixels.size()
              && descriptor.value(QStringLiteral("capacity_bytes")).toInteger() == 4096,
          QStringLiteral("descriptor_exact_range"));

    const QString session2 = uniqueSession();
    SharedFrameMemory duplicate;
    check(!duplicate.create(session, 0, 4096, &error) && error == QStringLiteral("E_SHM_EXISTS"),
          QStringLiteral("duplicate_mapping_name_rejected"));

    const QJsonArray pool{owner.poolEntry(), QJsonObject{
        {QStringLiteral("slot_index"), 1},
        {QStringLiteral("mapping_name"), QStringLiteral("Local\\RelinkVision_%1_1").arg(session)},
        {QStringLiteral("capacity_bytes"), 4096}}};
    ReadOnlyFrameMemory reader;
    check(reader.open(session, descriptor, pool, &error) && error.isEmpty() && reader.isOpen(),
          QStringLiteral("reader_open_valid_descriptor"));
    check(reader.copyBytes(&error) == pixels && error.isEmpty(), QStringLiteral("reader_copies_exact_bytes"));
    check(!reader.open(session, descriptor, pool, &error) && error == QStringLiteral("E_SHM_BUSY")
              && reader.isOpen(), QStringLiteral("reader_reopen_requires_explicit_close"));

    auto malformed = descriptor;
    malformed.insert(QStringLiteral("slot_index"), 1);
    ReadOnlyFrameMemory wrongSlot;
    check(!wrongSlot.open(session, malformed, pool, &error) && error == QStringLiteral("E_SHM_BOUNDS"),
          QStringLiteral("cross_slot_descriptor_rejected"));
    malformed = descriptor;
    malformed.insert(QStringLiteral("mapping_name"), QStringLiteral("Local\\RelinkVision_%1_0").arg(session2));
    check(!wrongSlot.open(session, malformed, pool, &error) && error == QStringLiteral("E_SHM_BOUNDS"),
          QStringLiteral("cross_session_mapping_rejected"));
    malformed = descriptor;
    malformed.insert(QStringLiteral("offset_bytes"), 4090);
    malformed.insert(QStringLiteral("byte_length"), 32);
    check(!wrongSlot.open(session, malformed, pool, &error) && error == QStringLiteral("E_SHM_BOUNDS"),
          QStringLiteral("descriptor_capacity_bounds_rejected"));
    malformed = descriptor;
    malformed.insert(QStringLiteral("capacity_bytes"), 8192);
    check(!wrongSlot.open(session, malformed, pool, &error) && error == QStringLiteral("E_SHM_BOUNDS"),
          QStringLiteral("descriptor_negotiated_capacity_mismatch_rejected"));
    malformed = descriptor;
    malformed.remove(QStringLiteral("lease_id"));
    check(!wrongSlot.open(session, malformed, pool, &error) && error == QStringLiteral("E_MESSAGE_INVALID"),
          QStringLiteral("descriptor_required_fields_rejected"));
    malformed = descriptor;
    malformed.insert(QStringLiteral("transport"), QStringLiteral("file"));
    check(!wrongSlot.open(session, malformed, pool, &error) && error == QStringLiteral("E_SHM_OPEN"),
          QStringLiteral("file_transport_rejected"));
    check(!wrongSlot.open(session, descriptor, QJsonArray{owner.poolEntry()}, &error)
              && error == QStringLiteral("E_SHM_OPEN"),
          QStringLiteral("incomplete_negotiated_pool_rejected"));

    check(!owner.write(QByteArray(), 0, &error) && error == QStringLiteral("E_SHM_BOUNDS"),
          QStringLiteral("empty_write_rejected"));
    check(!owner.write(pixels, -1, &error) && error == QStringLiteral("E_SHM_BOUNDS"),
          QStringLiteral("negative_write_offset_rejected"));
    check(!owner.write(pixels, std::numeric_limits<qint64>::max(), &error)
              && error == QStringLiteral("E_SHM_BOUNDS"), QStringLiteral("overflow_write_offset_rejected"));
    check(!owner.write(QByteArray(4097, 'x'), 0, &error) && error == QStringLiteral("E_SHM_BOUNDS"),
          QStringLiteral("oversized_write_rejected"));
    check(reader.copyBytes() == pixels && owner.descriptor(frameLease) == descriptor,
          QStringLiteral("failed_write_preserves_range_and_bytes"));
    auto invalidLease = frameLease;
    invalidLease.slotIndex = 1;
    check(owner.descriptor(invalidLease).isEmpty(), QStringLiteral("owner_descriptor_wrong_slot_rejected"));
    invalidLease = frameLease; invalidLease.byteLength += 1;
    check(owner.descriptor(invalidLease).isEmpty(), QStringLiteral("owner_descriptor_wrong_length_rejected"));
    invalidLease = frameLease; invalidLease.generation = 0;
    check(owner.descriptor(invalidLease).isEmpty(), QStringLiteral("owner_descriptor_zero_generation_rejected"));
    invalidLease = frameLease; invalidLease.leaseId = QStringLiteral("../not-lease");
    check(owner.descriptor(invalidLease).isEmpty(), QStringLiteral("owner_descriptor_malformed_lease_rejected"));
    for (const auto& field : {QStringLiteral("slot_index"), QStringLiteral("slot_generation"),
                              QStringLiteral("offset_bytes"), QStringLiteral("byte_length"),
                              QStringLiteral("capacity_bytes")}) {
        malformed = descriptor; malformed.insert(field, QStringLiteral("1"));
        check(!wrongSlot.open(session, malformed, pool, &error), QStringLiteral("string_integer_rejected_") + field);
        malformed = descriptor; malformed.insert(field, 0.5);
        check(!wrongSlot.open(session, malformed, pool, &error), QStringLiteral("fractional_integer_rejected_") + field);
    }
    malformed = descriptor; malformed.insert(QStringLiteral("offset_bytes"), QJsonValue(std::numeric_limits<qint64>::max()));
    check(!wrongSlot.open(session, malformed, pool, &error), QStringLiteral("overflow_descriptor_range_rejected"));
    malformed = descriptor; malformed.insert(QStringLiteral("image_path"), QStringLiteral("forbidden.png"));
    check(!wrongSlot.open(session, malformed, pool, &error), QStringLiteral("extra_image_path_field_rejected"));
    malformed = descriptor; malformed.insert(QStringLiteral("mapping_name"), QStringLiteral("Global\\RelinkVision_%1_0").arg(session));
    check(!wrongSlot.open(session, malformed, pool, &error), QStringLiteral("global_mapping_namespace_rejected"));
    check(!wrongSlot.open(session, descriptor, QJsonArray{pool.first(), pool.first()}, &error),
          QStringLiteral("duplicate_negotiated_slot_rejected"));
    auto badPool = pool; auto poolEntry = badPool[1].toObject();
    poolEntry.insert(QStringLiteral("mapping_name"), QStringLiteral("Local\\RelinkVision_other_1")); badPool[1] = poolEntry;
    check(!wrongSlot.open(session, descriptor, badPool, &error), QStringLiteral("unselected_pool_slot_session_validated"));
    check(reader.copyBytes() == pixels, QStringLiteral("invalid_reader_open_leaves_other_reader_unchanged"));

    owner.close();
    check(!owner.isOpen() && owner.poolEntry().isEmpty() && owner.descriptor(frameLease).isEmpty(),
          QStringLiteral("closed_owner_has_no_descriptor"));
    check(reader.copyBytes() == pixels, QStringLiteral("reader_survives_owner_close"));
    check(!duplicate.create(session, 0, 4096, &error) && error == QStringLiteral("E_SHM_EXISTS"),
          QStringLiteral("live_reader_prevents_mapping_name_reuse"));
    reader.close();
    check(!reader.isOpen(), QStringLiteral("reader_close_releases_view"));
    check(reader.copyBytes(&error).isEmpty() && error == QStringLiteral("E_SHM_OPEN"),
          QStringLiteral("closed_reader_copy_rejected"));
    owner.close();
    check(!owner.isOpen(), QStringLiteral("owner_close_releases_view"));
    SharedFrameMemory reopened;
    check(reopened.create(session, 0, 4096, &error), QStringLiteral("mapping_reopens_after_owner_close"));
    check(reopened.write(QByteArray("again"), 0, &error), QStringLiteral("reopened_mapping_writable"));
    reopened.close();
    check(!reopened.isOpen(), QStringLiteral("reopened_mapping_close"));

    SharedFrameMemory invalid;
    check(!invalid.create(session, 2, 4096, &error) && error == QStringLiteral("E_SHM_OPEN"),
          QStringLiteral("invalid_slot_rejected"));
    check(!invalid.create(session, 0, 134217729, &error) && error == QStringLiteral("E_SHM_BOUNDS"),
          QStringLiteral("oversized_capacity_rejected"));
    check(!invalid.create(QStringLiteral("../bad"), 0, 4096, &error) && error == QStringLiteral("E_SHM_OPEN"),
          QStringLiteral("invalid_session_name_rejected"));

    const QString scopedSession = uniqueSession();
    QJsonObject scopedDescriptor;
    QJsonArray scopedPool;
    ReadOnlyFrameMemory survivor;
    {
        SharedFrameMemory scopedOwner, scopedSecond;
        check(scopedOwner.create(scopedSession, 0, 4096) && scopedSecond.create(scopedSession, 1, 4096),
              QStringLiteral("both_real_slots_created"));
        check(scopedOwner.write(pixels) && scopedSecond.write(pixels), QStringLiteral("both_real_slots_written"));
        scopedPool = QJsonArray{scopedOwner.poolEntry(), scopedSecond.poolEntry()};
        scopedDescriptor = scopedSecond.descriptor(lease(1, 1, pixels.size()));
        check(survivor.open(scopedSession, scopedDescriptor, scopedPool) && survivor.copyBytes() == pixels,
              QStringLiteral("second_slot_mapping_readable"));
    }
    check(survivor.isOpen() && survivor.copyBytes() == pixels,
          QStringLiteral("reader_survives_owner_destructors"));
    survivor.close();
    check(!survivor.open(scopedSession, scopedDescriptor, scopedPool, &error) && !survivor.isOpen(),
          QStringLiteral("all_raii_owners_closed_mapping_name_disappears"));
    SharedFrameMemory scopedReopen;
    check(scopedReopen.create(scopedSession, 1, 4096), QStringLiteral("destructor_released_name_reusable"));
    scopedReopen.close();

#ifdef Q_OS_WIN
    // Warm the Qt/regex paths before comparing kernel handles across RAII churn.
    DWORD handlesBefore = 0, handlesAfter = 0;
    check(GetProcessHandleCount(GetCurrentProcess(), &handlesBefore), QStringLiteral("handle_count_baseline_available"));
    bool churnPass = true;
    for (int i = 0; i < 64; ++i) {
        SharedFrameMemory churnOwner;
        const QString churnSession = uniqueSession();
        churnPass = churnPass && churnOwner.create(churnSession, 0, 4096) && churnOwner.write(pixels);
        const QJsonArray churnPool{churnOwner.poolEntry(), QJsonObject{
            {QStringLiteral("slot_index"), 1},
            {QStringLiteral("mapping_name"), QStringLiteral("Local\\RelinkVision_%1_1").arg(churnSession)},
            {QStringLiteral("capacity_bytes"), 4096}}};
        const auto churnDescriptor = churnOwner.descriptor(lease(0, 1, pixels.size()));
        ReadOnlyFrameMemory churnReader;
        churnPass = churnPass && churnReader.open(churnSession, churnDescriptor, churnPool)
            && churnReader.copyBytes() == pixels;
    }
    check(churnPass, QStringLiteral("mapping_raii_churn_identity"));
    check(GetProcessHandleCount(GetCurrentProcess(), &handlesAfter) && handlesAfter == handlesBefore,
          QStringLiteral("mapping_raii_churn_no_handle_leak"));
#endif

    std::cout << "SHARED_FRAME_MEMORY_TESTS=" << (failures == 0 ? "PASS" : "FAIL")
              << "; assertions=" << assertions << "; failures=" << failures
              << "; external_processes=0; image_file_writes=0\n";
    return failures == 0 ? 0 : 1;
}
