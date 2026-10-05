/*
 *  nextpnr -- Next Generation Place and Route
 *
 *  Copyright (C) 2026  Deano Calver
 *
 *  Permission to use, copy, modify, and/or distribute this software for any
 *  purpose with or without fee is hereby granted, provided that the above
 *  copyright notice and this permission notice appear in all copies.
 */

#include "gpuroute_telemetry.h"

#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <stdexcept>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <share.h>
#include <sys/stat.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace gpuroute {

Telemetry::Field Telemetry::Field::string(std::string name, std::string value)
{
    Field result;
    result.name = std::move(name);
    result.type = Type::STRING;
    result.text = std::move(value);
    return result;
}

Telemetry::Field Telemetry::Field::integer_value(std::string name, int64_t value)
{
    Field result;
    result.name = std::move(name);
    result.type = Type::INTEGER;
    result.integer = value;
    return result;
}

Telemetry::Field Telemetry::Field::number_value(std::string name, double value, std::string nonfinite_reason)
{
    Field result;
    result.name = std::move(name);
    if (std::isfinite(value)) {
        result.type = Type::NUMBER;
        result.number = value;
    } else {
        result.type = Type::NULL_VALUE;
        result.null_reason = std::move(nonfinite_reason);
    }
    return result;
}

Telemetry::Field Telemetry::Field::boolean_value(std::string name, bool value)
{
    Field result;
    result.name = std::move(name);
    result.type = Type::BOOLEAN;
    result.boolean = value;
    return result;
}

Telemetry::Field Telemetry::Field::null_value(std::string name, std::string reason)
{
    Field result;
    result.name = std::move(name);
    result.type = Type::NULL_VALUE;
    result.null_reason = std::move(reason);
    return result;
}

std::string Telemetry::escape(const std::string &value)
{
    std::ostringstream out;
    out << '"';
    for (unsigned char c : value) {
        switch (c) {
        case '"': out << "\\\""; break;
        case '\\': out << "\\\\"; break;
        case '\b': out << "\\b"; break;
        case '\f': out << "\\f"; break;
        case '\n': out << "\\n"; break;
        case '\r': out << "\\r"; break;
        case '\t': out << "\\t"; break;
        default:
            if (c < 0x20)
                out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << int(c) << std::dec;
            else
                out << char(c);
        }
    }
    out << '"';
    return out.str();
}

std::string Telemetry::make_run_id(uint64_t seed)
{
    static std::atomic<uint64_t> serial{0};
    auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
    std::ostringstream out;
    out << "gpu-" << std::hex << seed << '-' << uint64_t(ticks) << '-' << serial.fetch_add(1);
    return out.str();
}

Telemetry::Telemetry(const std::string &path, std::string run_id)
        : path_(path), run_id_(std::move(run_id)), start_(std::chrono::steady_clock::now())
{
#ifndef _WIN32
    // The bounded collector records its launch epoch immediately before
    // spawning nextpnr. Python's monotonic_ns() and steady_clock both use
    // CLOCK_MONOTONIC on the supported Linux collector platform, so this
    // keeps routing observations on the same prefix clock as run budgets.
    if (const char *launch_ns = std::getenv("NEXTPNR_GPU_TELEMETRY_PROCESS_START_NS")) {
        try {
            std::size_t parsed = 0;
            const auto value = std::stoll(launch_ns, &parsed);
            if (parsed != std::strlen(launch_ns) || value < 0)
                throw std::invalid_argument("invalid process launch epoch");
            start_ = std::chrono::steady_clock::time_point(std::chrono::nanoseconds(value));
        } catch (const std::exception &) {
            throw std::runtime_error("invalid NEXTPNR_GPU_TELEMETRY_PROCESS_START_NS");
        }
    }
#endif
#ifdef _WIN32
    int fd = -1;
    errno_t err = _sopen_s(&fd, path.c_str(), _O_CREAT | _O_EXCL | _O_WRONLY | _O_BINARY, _SH_DENYRW,
                           _S_IREAD | _S_IWRITE);
    if (err == 0)
        file_ = _fdopen(fd, "wb");
#else
    // The bounded collector pre-opens and anchors output files, then passes
    // their inherited descriptors through /proc/self/fd. Preserve exclusive
    // creation for ordinary paths, but write the explicitly inherited object
    // instead of rejecting its descriptor path as already existing.
    const bool inherited_fd = path.rfind("/proc/self/fd/", 0) == 0;
    int flags = O_WRONLY | O_CLOEXEC | (inherited_fd ? O_TRUNC : (O_CREAT | O_EXCL));
    int fd = ::open(path.c_str(), flags, 0666);
    if (fd >= 0)
        file_ = fdopen(fd, "w");
#endif
    if (file_ == nullptr)
        io_error("create");
}

Telemetry::~Telemetry()
{
    if (file_ != nullptr)
        std::fclose(file_);
}

[[noreturn]] void Telemetry::io_error(const char *operation) const
{
    throw std::runtime_error("GPU telemetry " + std::string(operation) + " failed for '" + path_ + "': " +
                             std::strerror(errno));
}

void Telemetry::emit(const std::string &event, const std::string &phase, int attempt,
                     const std::vector<Field> &fields)
{
    const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start_).count();
    std::ostringstream out;
    out << std::setprecision(17) << "{\"schema_version\":1,\"sequence\":" << sequence_++
        << ",\"run_id\":" << escape(run_id_) << ",\"event\":" << escape(event) << ",\"phase\":";
    if (phase.empty())
        out << "null";
    else
        out << escape(phase);
    out << ",\"attempt\":";
    if (attempt < 0)
        out << "null";
    else
        out << attempt;
    out << ",\"elapsed_s\":" << elapsed << ",\"elapsed_unit\":\"s\"";
    for (const auto &field : fields) {
        out << ',' << escape(field.name) << ':';
        switch (field.type) {
        case Field::Type::STRING: out << escape(field.text); break;
        case Field::Type::INTEGER: out << field.integer; break;
        case Field::Type::NUMBER: out << field.number; break;
        case Field::Type::BOOLEAN: out << (field.boolean ? "true" : "false"); break;
        case Field::Type::NULL_VALUE:
            out << "null," << escape(field.name + "_unavailable_reason") << ':' << escape(field.null_reason);
            break;
        }
    }
    out << "}\n";
    const std::string line = out.str();
    if (std::fwrite(line.data(), 1, line.size(), file_) != line.size() || std::fflush(file_) != 0)
        io_error("write");
}

} // namespace gpuroute
