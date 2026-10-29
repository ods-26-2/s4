#include "s4/infrastructure/database/sqlite_media_clip_repository.hpp"

#include "s4/domain/errors/domain_error.hpp"

#include <sqlite3.h>

#include <chrono>
#include <cstdint>
#include <sstream>
#include <stdexcept>

namespace ods::s4::infrastructure {

namespace {

using TimePoint = std::chrono::system_clock::time_point;

std::int64_t to_epoch_ms(TimePoint tp) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(tp.time_since_epoch()).count();
}

TimePoint from_epoch_ms(std::int64_t ms) {
    return TimePoint{std::chrono::milliseconds(ms)};
}

std::string serialize_points(const std::vector<domain::PointOfInterest>& points) {
    if (points.empty()) {
        return {};
    }
    std::ostringstream out;
    for (const auto& point : points) {
        if (out.tellp() > 0) {
            out << ';';
        }
        out << point.x << ',' << point.y << ',' << point.label;
    }
    return out.str();
}

std::vector<domain::PointOfInterest> parse_points(const std::string& raw) {
    std::vector<domain::PointOfInterest> points;
    if (raw.empty()) {
        return points;
    }
    std::istringstream stream(raw);
    std::string entry;
    while (std::getline(stream, entry, ';')) {
        if (entry.empty()) {
            continue;
        }
        domain::PointOfInterest point;
        const std::size_t firstComma = entry.find(',');
        const std::size_t secondComma = firstComma == std::string::npos
            ? std::string::npos
            : entry.find(',', firstComma + 1);
        if (firstComma == std::string::npos || secondComma == std::string::npos) {
            throw domain::SqliteStorageError("malformed points_of_interest payload");
        }
        point.x = std::stod(entry.substr(0, firstComma));
        point.y = std::stod(entry.substr(firstComma + 1, secondComma - firstComma - 1));
        point.label = entry.substr(secondComma + 1);
        points.push_back(std::move(point));
    }
    return points;
}

domain::MediaClip clip_from_row(sqlite3_stmt* stmt) {
    const std::string clipId(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0)));
    const std::string eventId(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1)));
    const std::string filePath(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2)));
    const std::string sha256Hash(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3)));
    const auto startTime = from_epoch_ms(sqlite3_column_int64(stmt, 4));
    const auto endTime = from_epoch_ms(sqlite3_column_int64(stmt, 5));
    const bool isRetained = sqlite3_column_int(stmt, 6) != 0;
    const bool isLockedForAudit = sqlite3_column_int(stmt, 7) != 0;
    const auto createdAt = from_epoch_ms(sqlite3_column_int64(stmt, 8));
    const std::string rawPoints(
        reinterpret_cast<const char*>(sqlite3_column_text(stmt, 9))
    );

    return domain::MediaClip(
        clipId,
        eventId,
        filePath,
        sha256Hash,
        domain::TimeWindow(startTime, endTime),
        isRetained,
        isLockedForAudit,
        createdAt,
        parse_points(rawPoints)
    );
}

void bind_clip(sqlite3_stmt* stmt, const domain::MediaClip& clip) {
    if (sqlite3_bind_text(stmt, 1, clip.clipId().c_str(), -1, SQLITE_TRANSIENT) != SQLITE_OK ||
        sqlite3_bind_text(stmt, 2, clip.eventId().c_str(), -1, SQLITE_TRANSIENT) != SQLITE_OK ||
        sqlite3_bind_text(stmt, 3, clip.filePath().c_str(), -1, SQLITE_TRANSIENT) != SQLITE_OK ||
        sqlite3_bind_text(stmt, 4, clip.sha256Hash().c_str(), -1, SQLITE_TRANSIENT) != SQLITE_OK ||
        sqlite3_bind_int64(stmt, 5, to_epoch_ms(clip.timeWindow().startTime())) != SQLITE_OK ||
        sqlite3_bind_int64(stmt, 6, to_epoch_ms(clip.timeWindow().endTime())) != SQLITE_OK ||
        sqlite3_bind_int(stmt, 7, clip.isRetained() ? 1 : 0) != SQLITE_OK ||
        sqlite3_bind_int(stmt, 8, clip.isLockedForAudit() ? 1 : 0) != SQLITE_OK ||
        sqlite3_bind_int64(stmt, 9, to_epoch_ms(clip.createdAt())) != SQLITE_OK ||
        sqlite3_bind_text(stmt, 10, serialize_points(clip.pointsOfInterest()).c_str(), -1, SQLITE_TRANSIENT) != SQLITE_OK) {
        throw domain::SqliteStorageError("failed to bind media clip parameters");
    }
}

void run_mutation(sqlite3* db, sqlite3_stmt* stmt) {
    const int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        throw domain::SqliteStorageError(
            std::string("SQLite statement failed: ") + sqlite3_errmsg(db)
        );
    }
}

} // namespace

SQLiteMediaClipRepository::SQLiteMediaClipRepository(const std::string& dbPath)
    : m_dbPath(dbPath), m_db(nullptr) {
    if (sqlite3_open(m_dbPath.c_str(), &m_db) != SQLITE_OK) {
        const std::string message = sqlite3_errmsg(m_db);
        if (m_db != nullptr) {
            sqlite3_close(m_db);
            m_db = nullptr;
        }
        throw domain::SqliteStorageError("cannot open database " + m_dbPath + ": " + message);
    }
    initSchema();
}

SQLiteMediaClipRepository::~SQLiteMediaClipRepository() {
    if (m_db != nullptr) {
        sqlite3_close(m_db);
    }
}

void SQLiteMediaClipRepository::initSchema() {
    constexpr const char* kSchema =
        "CREATE TABLE IF NOT EXISTS media_clips ("
        "  clip_id TEXT PRIMARY KEY,"
        "  event_id TEXT NOT NULL,"
        "  file_path TEXT NOT NULL,"
        "  sha256_hash TEXT NOT NULL,"
        "  start_time_ms INTEGER NOT NULL,"
        "  end_time_ms INTEGER NOT NULL,"
        "  is_retained INTEGER NOT NULL,"
        "  is_locked_for_audit INTEGER NOT NULL,"
        "  created_at_ms INTEGER NOT NULL,"
        "  points_of_interest TEXT NOT NULL DEFAULT ''"
        ");";

    char* errorMessage = nullptr;
    if (sqlite3_exec(m_db, kSchema, nullptr, nullptr, &errorMessage) != SQLITE_OK) {
        const std::string message = errorMessage != nullptr ? errorMessage : sqlite3_errmsg(m_db);
        sqlite3_free(errorMessage);
        throw domain::SqliteStorageError("failed to init schema: " + message);
    }
}

void SQLiteMediaClipRepository::save(const domain::MediaClip& clip) {
    constexpr const char* kSql =
        "INSERT OR REPLACE INTO media_clips ("
        " clip_id, event_id, file_path, sha256_hash, start_time_ms, end_time_ms,"
        " is_retained, is_locked_for_audit, created_at_ms, points_of_interest"
        ") VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10);";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, kSql, -1, &stmt, nullptr) != SQLITE_OK) {
        throw domain::SqliteStorageError(sqlite3_errmsg(m_db));
    }
    bind_clip(stmt, clip);
    run_mutation(m_db, stmt);
}

std::optional<domain::MediaClip> SQLiteMediaClipRepository::findById(const std::string& clipId) {
    constexpr const char* kSql =
        "SELECT clip_id, event_id, file_path, sha256_hash, start_time_ms, end_time_ms,"
        "       is_retained, is_locked_for_audit, created_at_ms, points_of_interest"
        " FROM media_clips WHERE clip_id = ?1 LIMIT 1;";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, kSql, -1, &stmt, nullptr) != SQLITE_OK) {
        throw domain::SqliteStorageError(sqlite3_errmsg(m_db));
    }
    sqlite3_bind_text(stmt, 1, clipId.c_str(), -1, SQLITE_TRANSIENT);

    const int rc = sqlite3_step(stmt);
    std::optional<domain::MediaClip> result;
    if (rc == SQLITE_ROW) {
        result = clip_from_row(stmt);
    } else if (rc != SQLITE_DONE) {
        sqlite3_finalize(stmt);
        throw domain::SqliteStorageError(sqlite3_errmsg(m_db));
    }
    sqlite3_finalize(stmt);
    return result;
}

std::vector<domain::MediaClip> SQLiteMediaClipRepository::findByEventId(const std::string& eventId) {
    constexpr const char* kSql =
        "SELECT clip_id, event_id, file_path, sha256_hash, start_time_ms, end_time_ms,"
        "       is_retained, is_locked_for_audit, created_at_ms, points_of_interest"
        " FROM media_clips WHERE event_id = ?1;";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, kSql, -1, &stmt, nullptr) != SQLITE_OK) {
        throw domain::SqliteStorageError(sqlite3_errmsg(m_db));
    }
    sqlite3_bind_text(stmt, 1, eventId.c_str(), -1, SQLITE_TRANSIENT);

    std::vector<domain::MediaClip> results;
    for (int rc = sqlite3_step(stmt); rc == SQLITE_ROW; rc = sqlite3_step(stmt)) {
        results.push_back(clip_from_row(stmt));
    }
    sqlite3_finalize(stmt);
    return results;
}

std::vector<domain::MediaClip> SQLiteMediaClipRepository::findPurgeCandidates() {
    constexpr const char* kSql =
        "SELECT clip_id, event_id, file_path, sha256_hash, start_time_ms, end_time_ms,"
        "       is_retained, is_locked_for_audit, created_at_ms, points_of_interest"
        " FROM media_clips WHERE is_retained = 1 AND is_locked_for_audit = 0"
        " ORDER BY created_at_ms ASC;";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, kSql, -1, &stmt, nullptr) != SQLITE_OK) {
        throw domain::SqliteStorageError(sqlite3_errmsg(m_db));
    }

    std::vector<domain::MediaClip> results;
    for (int rc = sqlite3_step(stmt); rc == SQLITE_ROW; rc = sqlite3_step(stmt)) {
        results.push_back(clip_from_row(stmt));
    }
    sqlite3_finalize(stmt);
    return results;
}

void SQLiteMediaClipRepository::update(const domain::MediaClip& clip) {
    save(clip);
}

void SQLiteMediaClipRepository::remove(const std::string& clipId) {
    constexpr const char* kSql = "DELETE FROM media_clips WHERE clip_id = ?1;";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, kSql, -1, &stmt, nullptr) != SQLITE_OK) {
        throw domain::SqliteStorageError(sqlite3_errmsg(m_db));
    }
    sqlite3_bind_text(stmt, 1, clipId.c_str(), -1, SQLITE_TRANSIENT);
    run_mutation(m_db, stmt);
}

} // namespace ods::s4::infrastructure