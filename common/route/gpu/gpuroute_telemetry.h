/*
 *  nextpnr -- Next Generation Place and Route
 *
 *  Copyright (C) 2026  Deano Calver
 *
 *  Permission to use, copy, modify, and/or distribute this software for any
 *  purpose with or without fee is hereby granted, provided that the above
 *  copyright notice and this permission notice appear in all copies.
 */

#ifndef GPUROUTE_TELEMETRY_H
#define GPUROUTE_TELEMETRY_H

#include <chrono>
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>

namespace gpuroute {

// A deliberately small JSONL writer for routing telemetry. It owns no router
// state and has no access to the RNG, so enabling it cannot consume random
// numbers or influence routing order.
class Telemetry
{
  public:
    struct Field
    {
        enum class Type
        {
            STRING,
            INTEGER,
            NUMBER,
            BOOLEAN,
            NULL_VALUE
        };
        std::string name;
        Type type;
        std::string text;
        int64_t integer = 0;
        double number = 0;
        bool boolean = false;
        std::string null_reason;

        static Field string(std::string name, std::string value);
        static Field integer_value(std::string name, int64_t value);
        static Field number_value(std::string name, double value, std::string nonfinite_reason = "nonfinite");
        static Field boolean_value(std::string name, bool value);
        static Field null_value(std::string name, std::string reason);
    };

    Telemetry(const std::string &path, std::string run_id);
    ~Telemetry();
    Telemetry(const Telemetry &) = delete;
    Telemetry &operator=(const Telemetry &) = delete;

    void emit(const std::string &event, const std::string &phase, int attempt, const std::vector<Field> &fields = {});
    uint64_t sequence() const { return sequence_; }
    const std::string &run_id() const { return run_id_; }

    static std::string escape(const std::string &value);
    static std::string make_run_id(uint64_t seed);

  private:
    std::FILE *file_ = nullptr;
    std::string path_, run_id_;
    uint64_t sequence_ = 0;
    std::chrono::steady_clock::time_point start_;

    [[noreturn]] void io_error(const char *operation) const;
};

} // namespace gpuroute

#endif
